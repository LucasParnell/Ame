#version 450
layout(location = 0) out vec4 color;
layout(push_constant) uniform Solid { vec4 rgba; } solid;
void main() { color = solid.rgba; }
