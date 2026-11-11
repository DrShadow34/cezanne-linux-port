#version 450
/* a long loop, then one pixel writes the frame number to V[0..2] (the vertex
 * buffer of the vin test); writes no colour that matters */
layout(push_constant) uniform PC { uint frame; uint iters; } pc;
layout(set = 0, binding = 4) buffer V { uint v[]; };
layout(location = 0) out uint o;

void main()
{
   float a = fract(gl_FragCoord.x * 0.0013 + gl_FragCoord.y * 0.0007);
   for (uint i = 0u; i < pc.iters; i++)
      a = fract(a * 1.0001 + 0.37);
   if (ivec2(gl_FragCoord.xy) == ivec2(1023, 1023)) {
      uint f = pc.frame + (a > 2.0 ? 1u : 0u);
      v[0] = f;
      v[1] = f;
      v[2] = f;
   }
   o = 0u;
}
