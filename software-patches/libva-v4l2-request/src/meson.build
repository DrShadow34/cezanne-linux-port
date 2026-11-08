sources = files('driver.c', 'v4l2.c', 'config.c', 'buffer.c', 'surface.c', 'image.c',
		'encode.c', 'vpp.c',
		'context.c', 'picture.c', 'h264.c', 'vp9.c', 'vp8.c', 'av1.c', 'av1_grain.c', 'hevc.c', 'object_heap.c')

va_init = 'VA_DRIVER_INIT_FUNC=__vaDriverInit_@0@_@1@'.format(
	libva_dep.version().split('.')[0], libva_dep.version().split('.')[1])

shared_module('cezanne_drv_video', sources,
	name_prefix: '',
	c_args: ['-D' + va_init, '-fvisibility=hidden'],
	dependencies: [libva_dep, libdrm_dep, dependency('threads')],
	install: true,
	install_dir: join_paths(get_option('libdir'), 'dri'))
