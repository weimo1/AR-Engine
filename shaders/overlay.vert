#version 460

layout(location = 0) in vec2 inPosition;
layout(location = 1) in vec2 inUv;
layout(location = 2) in vec4 inColor;

layout(push_constant) uniform OverlayPush {
    vec2 scale;
    vec2 translate;
    uint decodeSrgb;
} pc;

layout(location = 0) out vec2 outUv;
layout(location = 1) out vec4 outColor;

void main() {
    outUv = inUv;
    outColor = inColor;
    gl_Position = vec4(inPosition * pc.scale + pc.translate, 0.0, 1.0);
}
