#version 450
layout(location = 0) in vec2 pos;
layout(location = 1) in vec3 col;
layout(location = 0) out vec3 vcol;
// real vertices: col sums to 1; a lane without a vertex reads 0 and spins
void main() {
   gl_Position = vec4(pos, 0.0, 1.0);
   int one = int(col.r + col.g + col.b + 0.5);
   int i = 0;
   vec3 c = col;
   do {
      do { c = c.yzx; } while (i >= 3 * one);
   } while (++i < 3 * one);
   vcol = c;
}
