#version 450
#ifdef VERTEX_INPUT
layout(location=0) in vec2 position;
#endif
void main() {
    const vec2 positions[3]=vec2[3](vec2(-1,-1),vec2(3,-1),vec2(-1,3));
#ifdef VERTEX_INPUT
    gl_Position=vec4(position,0,1);
#else
    gl_Position=vec4(positions[gl_VertexIndex],0,1);
#endif
}
