#version 450
// Device-agnostic blit: destination and UV arrive in clip space, so pixel
// origin, axis direction and target extent stay a renderer policy.
layout(location = 0) in vec2 corner;
layout(location = 0) out vec2 uv;
layout(push_constant) uniform Quad {
    vec2 origin;
    vec2 size;
    vec2 uv0;
    vec2 uv1;
    vec4 color;
} q;
void main() {
    gl_Position = vec4(q.origin + corner * q.size, 0.0, 1.0);
    uv = mix(q.uv0, q.uv1, corner);
}
