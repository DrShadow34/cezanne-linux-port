#version 450
layout(location = 0) flat in uvec2 v;
layout(location = 0) out uvec2 o;

void main()
{
   o = v;
}
