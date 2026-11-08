#version 450
layout(location = 0) in vec3 vcol;
layout(location = 0) out vec4 o;
void main() { o = vec4(vcol, 1.0); }
