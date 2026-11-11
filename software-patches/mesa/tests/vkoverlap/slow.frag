#version 450
/* a long loop, then the frame number (MODE 0) or T's texel read at the end
 * of it (MODE 1, write-after-read test) */
layout(push_constant) uniform PC { uint frame; uint iters; } pc;
layout(constant_id = 0) const int MODE = 0;
layout(set = 0, binding = 3) uniform usampler2D T;
layout(location = 0) out uint o;

void main()
{
   float a = fract(gl_FragCoord.x * 0.0013 + gl_FragCoord.y * 0.0007);
   for (uint i = 0u; i < pc.iters; i++)
      a = fract(a * 1.0001 + 0.37);
   uint v = pc.frame;
   if (MODE == 1)
      v = texelFetch(T, ivec2(a > 2.0 ? 1 : 0, 0), 0).r;
   o = v + (a > 2.0 ? 1u : 0u);
}
