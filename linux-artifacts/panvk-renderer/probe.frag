#version 450
#ifdef INTEGER_SOURCE
layout(push_constant) uniform Params {uvec4 color;} pc;
layout(location=0) out uvec4 value;
#else
layout(push_constant) uniform Params {vec4 color;} pc;
layout(location=0) out vec4 value;
#endif
void main() {value=pc.color;}
