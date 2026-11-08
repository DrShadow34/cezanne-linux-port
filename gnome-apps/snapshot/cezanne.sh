# The fork (mainline/camera/README.md, NOTES "Camera app"): libcamera backend
# with the camera settings sheet, RAW / DNG, lens switch, flash, HDR /
# portrait / low light / denoise through the helpers, H.264 recording on
# cezanne-venc. Developed in apps/snapshot (the 51.0 tag + working-tree
# changes); update-patch.sh turns that into 0001-cezanne-fork.patch. The patch
# carries its Cargo.lock, so it goes in before upstream's prepare()
# (cargo fetch --locked).
cz_source 0001-cezanne-fork.patch
cz_before prepare 'patch -d "$srcdir/snapshot" -Np1 -i "$srcdir/0001-cezanne-fork.patch"'
# libcamerasrc (our libcamera), the picture helpers, the patched va /
# v4l2codecs plugins (hardware decoding when the gallery plays a recording;
# recording itself uses v4l2h264enc)
depends+=(gst-plugin-libcamera cezanne-photo gst-plugins-cezanne)
optdepends+=('cezanne-upscale: AI upscaling of saved pictures on the APU (64MP button)'
            'realesrgan-ncnn-vulkan: AI upscaling on the GPU (fallback)')
