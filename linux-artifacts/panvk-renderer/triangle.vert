#version 450
layout(location=0) in vec2 position;
layout(location=1) in vec3 color;
layout(location=0) out vec3 rgb;
layout(push_constant) uniform Frame { float angle; } frame;
void main() {
    float cx=cos(frame.angle*0.7), sx=sin(frame.angle*0.7);
    float cy=cos(frame.angle), sy=sin(frame.angle);
    vec3 p=vec3(position.x,position.y*cx,position.y*sx);
    p=vec3(cy*p.x+sy*p.z,p.y,-sy*p.x+cy*p.z);
    float d=2.5-p.z;
    // Convert the GLES diagnostic's clip depth [-w,w] to Vulkan [0,w].
    gl_Position=vec4(2.5*p.xy,(1.020202*d-0.2020202+d)*0.5,d);
    rgb=color;
}
