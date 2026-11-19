project('millerx', 'c',
  version : '0.5.4',
  license : 'GPL-2.0-or-later',
  meson_version : '>= 0.60',
  default_options : ['c_std=gnu11', 'warning_level=2', 'buildtype=release'])

gtk = dependency('gtk+-3.0', version : '>= 3.22')
archive = dependency('libarchive')
threads = dependency('threads')

bindir = get_option('prefix') / get_option('bindir')
datadir = get_option('prefix') / get_option('datadir')

add_project_arguments(
  '-DMILLERX_VERSION="@0@"'.format(meson.project_version()),
  '-DMILLERX_BINDIR="@0@"'.format(bindir),
  '-Wno-unused-parameter', '-Wno-missing-field-initializers',
  language : 'c')

executable('millerx',
  'src/main.c', 'src/settings.c', 'src/util.c', 'src/column.c', 'src/preview.c',
  'src/job.c', 'src/window.c', 'src/places.c', 'src/dialogs.c',
  dependencies : [gtk, archive, threads],
  install : true)

install_data('data/millerx.desktop', install_dir : datadir / 'applications')
install_data('data/icons/millerx.svg', install_dir : datadir / 'icons/hicolor/scalable/apps')
foreach s : ['16', '24', '32', '48', '64', '128', '256']
  install_data('data/icons/millerx-' + s + '.png',
    install_dir : datadir / 'icons/hicolor' / (s + 'x' + s) / 'apps',
    rename : 'millerx.png')
endforeach

policy = configure_file(
  input : 'data/org.mix.millerx.policy.in',
  output : 'org.mix.millerx.policy',
  configuration : {'BINDIR' : bindir})
install_data(policy, install_dir : datadir / 'polkit-1/actions')
