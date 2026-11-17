#version 450

layout(location = 0) in vec3 position;
layout(location = 1) in vec2 uv;
layout(location = 2) in vec3 normal;
layout(location = 0) out vec2 outUv;
layout(location = 1) out vec3 viewNormal;

layout(push_constant) uniform Draw {
    mat4 clipFromModel;
    vec4 normalColumn0;
    vec4 normalColumn1;
    vec4 normalColumn2;
    vec4 baseColorFactor;
} draw;

void main() {
    gl_Position = draw.clipFromModel * vec4(position, 1.0);
    outUv = uv;
    viewNormal = mat3(draw.normalColumn0.xyz, draw.normalColumn1.xyz, draw.normalColumn2.xyz) * normal;
}
