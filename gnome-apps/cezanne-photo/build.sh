#!/bin/sh
# build.sh [OUTDIR]: builds the picture helpers into OUTDIR (default: here).
# Packaged as cezanne-photo (PKGBUILD here, /usr/bin); a quick dev build runs
# from OUTDIR, never install it over the package.
#   cezanne-mono   mono sensor stream + exposure brackets (for HDR)
#   cezanne-photo  denoise / portrait / HDR fusion / AI upscaling of saved pictures
# Needs opencv (OpenCV 5: link the modules, pkg-config also pulls viz/hdf
# whose vtk/hdf5 are not installed), exiv2, and for --upscale
# cezanne-upscale (the APU, mainline/apu/upscaler) or realesrgan-ncnn-vulkan
# (the GPU, package apps/realesrgan-ncnn-vulkan) with the models in
# /usr/share/realesrgan/models (upscayl-lite-4x from Upscayl's
# resources/models).
set -e
cd "$(dirname "$0")"
OUT=${1:-.}
gcc ${CFLAGS:--O2} -Wall -o "$OUT/cezanne-mono" cezanne-mono.c -lm $LDFLAGS
g++ ${CXXFLAGS:--O2} -std=c++20 -o "$OUT/cezanne-photo" cezanne-photo.cpp \
	-I/usr/include/opencv5 -lopencv_core -lopencv_imgproc -lopencv_imgcodecs \
	-lopencv_features -lopencv_geometry -lopencv_flann -lopencv_photo \
	-lopencv_calib -lopencv_stereo -lexiv2 $LDFLAGS
