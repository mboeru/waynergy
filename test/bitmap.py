from pathlib import Path
import struct
import subprocess
import sys

converter = str(Path(sys.argv[1]).resolve())
colors = [(255, 0, 85, 255), (0, 255, 170, 85), (85, 170, 255, 0),
          (170, 85, 0, 170), (0, 0, 0, 255), (255, 255, 255, 255)]
standard_masks = (0xff0000, 0xff00, 0xff, 0xff000000)


def dib(header=40, bpp=24, compression=0, top_down=False, masks=standard_masks,
        palette=False, legacy=False):
    width, height = 3, 2
    h = bytearray(40 if legacy else header)
    struct.pack_into('<IiiHHII', h, 0, header, width, -height if top_down else height,
                     1, bpp, compression, ((width * bpp + 31) // 32) * 4 * height)
    has_alpha = compression != 0 and not legacy and (header >= 56 or compression == 6)
    if compression and not legacy:
        if header == 40:
            h.extend(struct.pack('<' + 'I' * (4 if compression == 6 else 3),
                                 *masks[:4 if compression == 6 else 3]))
        else:
            struct.pack_into('<3I', h, 40, *masks[:3])
            if header >= 56:
                struct.pack_into('<I', h, 52, masks[3])
    if palette:
        struct.pack_into('<I', h, 32, 1)
        h.extend(b'\0\0\0\0')
    rows = []
    for y in range(height):
        row = bytearray()
        for color in colors[y * width:(y + 1) * width]:
            if bpp == 24:
                row.extend(bytes(color[2::-1]))
            elif compression == 0 or legacy:
                row.extend(bytes((*color[2::-1], 0)))  # BI_RGB high byte is unused.
            else:
                value = 0
                for c, mask in zip(color, masks[:4 if has_alpha else 3]):
                    if mask:
                        shift = (mask & -mask).bit_length() - 1
                        value |= ((c * (mask >> shift) + 127) // 255) << shift
                row.extend(struct.pack('<I', value))
        row.extend(b'\xaa' * (-len(row) % 4))
        rows.append(bytes(row))
    payload = bytes(h) + b''.join(rows if top_down else reversed(rows))
    expected = bytes(c for color in colors for c in (*color[:3], color[3] if has_alpha else 255))
    return payload, expected


fixtures = [
    ('RGB24 padded', {}),
    ('RGB24 top-down', {'top_down': True}),
    ('RGB32 unused high byte', {'bpp': 32}),
    ('RGB32 top-down', {'bpp': 32, 'top_down': True}),
    ('RGB24 optimization palette', {'palette': True}),
    ('external RGB masks', {'bpp': 32, 'compression': 3,
                            'masks': (0xff, 0xff00, 0xff0000, 0)}),
    ('external RGBA masks', {'bpp': 32, 'compression': 6}),
    ('V2 RGB masks', {'header': 52, 'bpp': 32, 'compression': 3}),
    ('V3 RGBA masks', {'header': 56, 'bpp': 32, 'compression': 3}),
    ('V4 RGBA masks', {'header': 108, 'bpp': 32, 'compression': 3}),
    ('V5 top-down RGBA', {'header': 124, 'bpp': 32, 'compression': 3, 'top_down': True}),
    ('10-bit RGB with 2-bit alpha', {'header': 108, 'bpp': 32, 'compression': 3,
                                    'masks': (0x3ff00000, 0xffc00, 0x3ff, 0xc0000000)}),
    ('legacy macOS V5', {'header': 124, 'bpp': 32, 'compression': 3, 'legacy': True}),
]
fingerprint = None
for name, options in fixtures:
    payload, expected = dib(**options)
    normalized = subprocess.check_output([converter], input=payload)
    assert normalized[:2] == b'BM', name
    # Exercise the same BMP -> PNG conversion as the running receiver.
    png = subprocess.check_output(['magick', 'bmp:-', 'png:-'], input=normalized)
    rgba = subprocess.check_output(['magick', 'png:-', '-depth', '8', 'RGBA:-'], input=png)
    assert rgba == expected, (name, rgba, expected)
    actual_hash = subprocess.check_output([converter, 'hash'], input=payload)
    if fingerprint is None:
        fingerprint = actual_hash
    assert actual_hash == fingerprint, name

valid, _ = dib()
invalid = [b'', valid[:39], valid[:-1]]
for offset, value in [(4, 0), (4, 0xffffffff), (8, 0), (8, 0x80000000),
                      (12, 0), (14, 16), (16, 1), (32, 0xffffffff)]:
    broken = bytearray(valid)
    struct.pack_into('<I' if offset not in (12, 14) else '<H', broken, offset, value)
    invalid.append(bytes(broken))
for masks in [(0xff, 0xff, 0xff0000, 0), (0x55, 0xff00, 0xff0000, 0),
              (0, 0xff00, 0xff0000, 0), (0xff, 0xff00, 0xff0000, 0xff)]:
    invalid.append(dib(header=108, bpp=32, compression=3, masks=masks)[0])
for header in (52, 56, 108, 124):
    payload, _ = dib(header=header, bpp=32, compression=3)
    invalid.extend([payload[:header - 1], payload[:-1]])
for header, compression, bpp in [(108, 3, 32), (124, 0, 32), (124, 3, 24)]:
    # Only the exact known 32-bit BITFIELDS V5 shape is repairable.
    payload, _ = dib(header=header, compression=compression, bpp=bpp, legacy=True)
    invalid.append(payload)
for payload in invalid:
    assert subprocess.run([converter], input=payload, stdout=subprocess.DEVNULL).returncode != 0
    assert subprocess.run([converter, 'hash'], input=payload, stdout=subprocess.DEVNULL).returncode != 0
print(f'{len(fixtures)} bitmap layouts and {len(invalid)} invalid payloads passed')
