#!/bin/sh
# build.sh <outdir>: vkoverlap + its shaders (glslangValidator)
out=${1:-.}
mkdir -p "$out"
for s in fs.vert slow.frag vsread.vert pass.frag passdbg.frag zero.frag heavy.vert readx.comp writet.comp ssbo.frag vattr.vert; do
	glslangValidator -V --target-env vulkan1.3 -o "$out/$s.spv" "$s" >/dev/null || exit 1
done
cc -O2 -Wall -o "$out/vkoverlap" vkoverlap.c -lvulkan
