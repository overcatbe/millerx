project('millerx', 'c',
  version : '0.8.2',
  license : 'GPL-2.0-or-later',
  meson_version : '>= 0.60',
  default_options : ['c_std=gnu11', 'warning_level=2', 'buildtype=release'])

gtk = dependency('gtk+-3.0', version : '>= 3.22')
giounix = dependency('gio-unix-2.0')    # launchers (.desktop files) on a double-click
archive = dependency('libarchive')
threads = dependency('threads')
# WebP pictures: gdk-pixbuf has no WebP loader on many systems (Slackware), so we decode it ourselves
webp = dependency('libwebp', required : false)
deps = [gtk, giounix, archive, threads]
if webp.found()
  deps += webp
  add_project_arguments('-DHAVE_WEBP', language : 'c')
endif

# Translations: only when msgfmt is there (or asked for with -Dnls=enabled); otherwise English only
msgfmt = find_program('msgfmt', required : get_option('nls'))
if msgfmt.found()
  i18n = import('i18n')
  add_project_arguments('-DENABLE_NLS', '-DGETTEXT_PACKAGE="millerx"',
    '-DLOCALEDIR="@0@"'.format(get_option('prefix') / get_option('localedir')), language : 'c')
  subdir('po')
endif

bindir = get_option('prefix') / get_option('bindir')
datadir = get_option('prefix') / get_option('datadir')

add_project_arguments(
  '-DMILLERX_VERSION="@0@"'.format(meson.project_version()),
  '-DMILLERX_BINDIR="@0@"'.format(bindir),
  '-DMILLERX_POT="@0@"'.format(datadir / 'millerx' / 'millerx.pot'),
  '-Wno-unused-parameter', '-Wno-missing-field-initializers',
  language : 'c')

executable('millerx',
  'src/main.c', 'src/settings.c', 'src/util.c', 'src/column.c', 'src/preview.c',
  'src/job.c', 'src/window.c', 'src/places.c', 'src/dialogs.c', 'src/thumbcache.c', 'src/sorting.c', 'src/highlight.c', 'src/trash.c', 'src/search.c',
  'src/archive.c',
  dependencies : deps,
  install : true)

install_data('data/millerx.desktop', install_dir : datadir / 'applications')
install_data('data/org.mix.millerx.metainfo.xml', install_dir : datadir / 'metainfo')
install_man('data/millerx.1')
install_data('po/millerx.pot', install_dir : datadir / 'millerx')   # the start for people's own languages
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
