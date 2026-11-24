#version 450

layout(location = 0) in vec3 position;

layout(push_constant) uniform ShadowDirectPush {
    mat4 lightClipFromModel;
} pc;

void main() {
    gl_Position = pc.lightClipFromModel * vec4(position, 1.0);
}
