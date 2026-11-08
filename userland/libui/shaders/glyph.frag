#version 450
// Single-channel coverage times a straight tint; output is premultiplied.
layout(location = 0) in vec2 uv;
layout(location = 0) out vec4 color;
layout(binding = 0) uniform sampler2D coverage;
layout(push_constant) uniform Quad {
    vec2 origin;
    vec2 size;
    vec2 uv0;
    vec2 uv1;
    vec4 color;
} q;
void main() {
    float a = texture(coverage, uv).r * q.color.a;
    color = vec4(q.color.rgb * a, a);
}
