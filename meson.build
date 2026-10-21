project('waynergy',
 'c',
 version: '0.0.17'
)

src_c = files(
  'src/wl_idle.c',
  'src/wl_idle_gnome.c',
  'src/wl_idle_kde.c',
  'src/wl_idle_ext.c',
  'src/wl_input.c',
  'src/wl_input_wlr.c',
  'src/wl_input_kde.c',
  'src/wl_input_uinput.c',
  'src/main.c',
  'src/clip.c',
  'src/clip-bitmap.c',
  'src/config.c',
  'src/net.c',
  'src/os.c',
  'src/sig.c',
  'src/ssp.c',
  'src/wayland.c',
  'src/uSynergy.c',
  'src/log.c'
)

wayland_client = dependency('wayland-client')
xkbcommon = dependency('xkbcommon')
libtls = dependency('libtls')

if host_machine.system() == 'linux'
  add_project_arguments('-D_GNU_SOURCE ', language: 'c')
endif
if host_machine.endian() == 'big'
  add_project_arguments('-DUSYNERGY_BIG_ENDIAN', language: 'c')
else
  add_project_arguments('-DUSYNERGY_LITTLE_ENDIAN', language: 'c')
endif

subdir('protocol')
subdir('include')

executable(
  'waynergy', 
  src_c, 
  install: true, 
  dependencies : [
    client_protos,
    libtls,
    wayland_client, 
    xkbcommon,
    ver_dep,
  ],
  include_directories: [include_directories('include')],
)
clip_helper = executable(
  'waynergy-clip-update', 
  'src/clip-update.c', 
  'src/clip-bitmap.c',
  install: true,
  include_directories: [include_directories('include')],
)
clipboard_test = executable('clipboard-test',
  'test/clipboard.c', 'src/ssp.c', 'src/log.c', 'src/clip-bitmap.c',
  dependencies: [client_protos, wayland_client, xkbcommon],
  c_args: ['-DWAYNERGY_TEST', '-UNDEBUG'],
  include_directories: include_directories('include'))
test('clipboard-protocol', clipboard_test)
bitmap_converter = executable('bitmap-convert-test',
  'test/bitmap-convert.c', 'src/clip-bitmap.c',
  include_directories: include_directories('include'))
python = find_program('python3', required: false)
if python.found() and find_program('magick', required: false).found()
  test('clipboard-helper', python,
    args: [files('test/clipboard-helper.py'), clip_helper], timeout: 60)
  test('clipboard-bitmaps', python,
    args: [files('test/bitmap.py'), bitmap_converter], timeout: 60)
endif
executable(
  'waynergy-mapper',
  'src/mapper.c',
  install: true,
  dependencies : [
    client_protos,
    wayland_client,
    xkbcommon,
  ],
  include_directories: [include_directories('include')],
)
install_data(
  'waynergy.desktop',
  install_dir: get_option('datadir') / 'applications',
  rename: meson.project_name() + '.desktop',
)
