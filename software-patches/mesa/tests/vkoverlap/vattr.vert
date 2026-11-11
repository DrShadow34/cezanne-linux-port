#version 450
/* only a vertex attribute (no descriptors): passes it on flat */
layout(location = 0) in uint a;
layout(location = 0) flat out uvec2 v;

void main()
{
   v = uvec2(a, a);
   vec2 p = vec2((gl_VertexIndex << 1) & 2, gl_VertexIndex & 2);
   gl_Position = vec4(p * 2.0 - 1.0, 0.0, 1.0);
}
