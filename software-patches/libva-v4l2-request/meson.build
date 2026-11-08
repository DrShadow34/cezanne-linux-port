# cezanne VA-API driver (fork of Bootlin's libva-v4l2-request, MIT)
project('cezanne-va', 'c', version: '0.1.0', meson_version: '>= 0.60.0',
	default_options: ['c_std=gnu11', 'warning_level=2', 'buildtype=release'])
# va-api's API version ("libva" 2.24 = VA-API 1.24)
libva_dep = dependency('libva', version: '>= 1.20.0')
libdrm_dep = dependency('libdrm')
add_project_arguments('-Wno-unused-parameter', language: 'c')
subdir('src')
