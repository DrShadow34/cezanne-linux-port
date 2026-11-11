#version 450
/* vertex work only: a loop per vertex, the three vertices of a triangle are
 * equal (zero area, culled after position shading) */
layout(push_constant) uniform PC { uint frame; uint iters; } pc;

void main()
{
   float a = float(gl_VertexIndex / 3) * 1e-6;
   for (uint i = 0u; i < pc.iters; i++)
      a = fract(a * 1.0001 + 0.37);
   gl_Position = vec4(a * 0.5, a * 0.25, 0.5, 1.0);
}
