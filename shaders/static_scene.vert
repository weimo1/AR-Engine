#version 450
layout(location = 0) in vec3 position;
layout(location = 1) in vec2 uv;
layout(location = 0) out vec2 outUv;
layout(push_constant) uniform Draw {
    mat4 clipFromModel;
} draw;

void main() {
    gl_Position = draw.clipFromModel * vec4(position, 1.0);
    outUv = uv;
}
