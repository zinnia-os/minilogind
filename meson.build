project(
  'minilogind',
  'c',
  version: '1.0',
  license: 'MIT',
  default_options: ['c_std=gnu23', 'warning_level=2'],
)

add_project_arguments('-Wno-unused-parameter', language: 'c')

cc = meson.get_compiler('c')

gio_unix_dep = dependency('gio-unix-2.0')
libseat_dep = dependency('libseat')
libudev_dep = dependency('libudev', required: false)

deps = [gio_unix_dep, libseat_dep]
if libudev_dep.found()
  deps += libudev_dep
else
  # We only need `DRM_DIR_NAME`
  deps += dependency('libdrm').partial_dependency(compile_args: true, includes: true)
endif

conf = configuration_data()
conf.set10('HAVE_LIBUDEV', libudev_dep.found())
conf.set10('HAVE_SYS_SYSMACROS_H', cc.has_header('sys/sysmacros.h'))
conf.set10('HAVE_SYS_VT_H', cc.has_header('sys/vt.h'))
conf.set10('HAVE_SYS_CONSIO_H', cc.has_header('sys/consio.h'))
configure_file(output: 'config.h', configuration: conf)

executable(
  'minilogind',
  'minilogind.c',
  dependencies: deps,
  install: true,
  install_dir: get_option('libexecdir'),
)

install_data(
  'org.freedesktop.login1.conf',
  install_dir: get_option('datadir') / 'dbus-1' / 'system.d',
)
