#version 450
/* DBG=1: the vertex shader's min and the frame number (did the pass run?) */
layout(push_constant) uniform PC { uint frame; uint iters; } pc;
layout(location = 0) flat in uvec2 v;
layout(location = 0) out uvec2 o;

void main()
{
   o = uvec2(v.x, pc.frame);
}
