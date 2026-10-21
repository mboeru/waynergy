import os
from pathlib import Path
import socket
import struct
import subprocess
import sys
import tempfile
import zlib

helper = str(Path(sys.argv[1]).resolve())


def png(width, height):
    def chunk(name, data):
        return struct.pack('!I', len(data)) + name + data + struct.pack('!I', zlib.crc32(name + data))
    # Asymmetric colors verify orientation, channels and row padding.
    rgb = bytes((x * 7 + y * 19 + c * 67) % 256
                for y in range(height) for x in range(width) for c in range(3))
    scanlines = b''.join(b'\0' + rgb[y * width * 3:(y + 1) * width * 3] for y in range(height))
    return (b'\x89PNG\r\n\x1a\n' +
            chunk(b'IHDR', struct.pack('!2I5B', width, height, 8, 2, 0, 0, 0)) +
            chunk(b'IDAT', zlib.compress(scanlines)) + chunk(b'IEND', b'')), rgb


with tempfile.TemporaryDirectory(prefix='waynergy-clipboard-test-') as folder:
    root = Path(folder)
    # Isolate tests from the user's clipboard; the real converter is still used.
    mock = root / 'wl-paste'
    mock.write_text('''#!/bin/sh
case "$*" in
  *--list-types*) printf "%s\\n" "$TEST_TYPES" ;;
  *) test -n "$TEST_IMAGE" && /bin/cat "$TEST_IMAGE" ;;
esac
''')
    mock.chmod(0o755)
    env = dict(os.environ, PATH=folder + ':' + os.environ['PATH'],
               CLIPBOARD_STATE='data', TEST_TYPES='text/plain')
    server = socket.socket(socket.AF_UNIX)
    address = str(root / 'socket')
    server.bind(address)
    server.listen()
    server.settimeout(20)

    def transfer(data, bitmap=True, cid='c', text_only=False):
        args = [helper, cid, address] + (['bitmap'] if bitmap else ['text-only'] if text_only else [])
        with tempfile.TemporaryFile() as source:
            source.write(data)
            source.seek(0)
            child = subprocess.Popen(args, stdin=source, env=env)
            peer, _ = server.accept()
            with peer:
                peer.settimeout(20)
                parts = []
                while part := peer.recv(65536):
                    parts.append(part)
            assert child.wait(timeout=20) == 0
        wire = b''.join(parts)
        count = struct.calcsize('@N')
        assert len(wire) >= 1 + count
        size, = struct.unpack('@N', wire[1:1 + count])
        payload = wire[1 + count:]
        assert size == len(payload)
        return wire[:1], payload

    for width, height in [(3, 2), (1920, 1080)]:
        source, rgb = png(width, height)
        cid, dib = transfer(source)
        assert cid == b'C'
        header, w, h, planes, bpp, compression = struct.unpack('<IiiHHI', dib[:20])
        assert (header, w, h, planes, bpp, compression) == (40, width, height, 1, 24, 0)
        stride = (width * 3 + 3) & ~3
        for y in range(height):
            row = dib[40 + (height - y - 1) * stride:40 + (height - y - 1) * stride + width * 3]
            expected = rgb[y * width * 3:(y + 1) * width * 3]
            assert row[0::3] == expected[2::3]
            assert row[1::3] == expected[1::3]
            assert row[2::3] == expected[0::3]
    jpeg = subprocess.check_output(['magick', '-size', '17x9', 'gradient:red-blue', 'jpeg:-'])
    cid, dib = transfer(jpeg, cid='p')
    assert cid == b'P' and struct.unpack('<ii', dib[4:12]) == (17, 9)
    assert transfer(b'plain text', bitmap=False) == (b'c', b'plain text')
    # Mixed offers produce the same bitmap through either watcher; if the image
    # is invalid, unavailable or cannot be converted, retain the fallback text.
    env['TEST_TYPES'] = 'text/plain\nimage/png'
    fixture = root / 'mixed.png'
    fixture.write_bytes(png(3, 2)[0])
    env['TEST_IMAGE'] = str(fixture)
    assert transfer(b'fallback', bitmap=False) == transfer(fixture.read_bytes())
    assert transfer(b'fallback', bitmap=False, text_only=True) == (b'c', b'fallback')
    fixture.write_bytes(b'broken image')
    assert transfer(b'fallback', bitmap=False) == (b'c', b'fallback')
    env['TEST_IMAGE'] = str(root / 'missing-image')
    assert transfer(b'fallback', bitmap=False) == (b'c', b'fallback')
    fixture.write_bytes(png(3, 2)[0])
    env['TEST_IMAGE'] = str(fixture)
    full_path = env['PATH']
    env['PATH'] = folder  # wl-paste mock exists; ImageMagick is absent.
    assert transfer(b'fallback', bitmap=False) == (b'c', b'fallback')
    env['PATH'] = full_path
    for state in ('nil', 'clear', 'sensitive'):
        env['CLIPBOARD_STATE'] = state
        assert subprocess.run([helper, 'c', address, 'bitmap'], input=b'', env=env).returncode == 0
    env['CLIPBOARD_STATE'] = 'data'
    assert subprocess.run([helper, 'c', address, 'bitmap'], input=b'invalid image', env=env,
                          stderr=subprocess.DEVNULL).returncode != 0
    assert subprocess.run([helper], env=env).returncode != 0
    server.settimeout(0.1)
    try:
        server.accept()
        raise AssertionError('unexpected clipboard notification')
    except TimeoutError:
        pass
    server.close()
print('PNG/JPEG, large images, mixed offers and text-only/conversion-failure fallback passed')
