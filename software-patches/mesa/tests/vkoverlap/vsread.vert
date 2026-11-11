#version 450
/* reads X's corners in the vertex shader: min / max as a flat varying */
layout(set = 0, binding = 0) uniform usampler2D X;
layout(location = 0) flat out uvec2 v;

void main()
{
   ivec2 s = textureSize(X, 0) - 1;
   uint a = texelFetch(X, ivec2(0, 0), 0).r, b = texelFetch(X, s, 0).r;
   uint c = texelFetch(X, ivec2(s.x, 0), 0).r, d = texelFetch(X, ivec2(0, s.y), 0).r;
   v = uvec2(min(min(a, b), min(c, d)), max(max(a, b), max(c, d)));
   vec2 p = vec2((gl_VertexIndex << 1) & 2, gl_VertexIndex & 2);
   gl_Position = vec4(p * 2.0 - 1.0, 0.0, 1.0);
}
