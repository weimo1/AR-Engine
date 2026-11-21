#version 450
#extension GL_GOOGLE_include_directive : require
#include "static_indirect_scene.glsl"

layout(location = 0) in vec3 position;
layout(location = 1) in vec2 uv;
layout(location = 2) in vec3 normal;
layout(location = 0) out vec2 outUv;
layout(location = 1) out vec3 viewNormal;
layout(location = 2) flat out uint materialIndex;

layout(std430, set = 1, binding = 0) readonly buffer Instances {
    StaticInstance instances[];
};
layout(push_constant) uniform Frame { float inverseAspect; } frame;

void main() {
    StaticInstance instance = instances[gl_InstanceIndex];
    gl_Position = instance.clipFromModel * vec4(position, 1.0);
    gl_Position.x *= frame.inverseAspect;
    outUv = uv;
    viewNormal = mat3(instance.normalColumn0.xyz, instance.normalColumn1.xyz,
                      instance.normalColumn2.xyz) * normal;
    materialIndex = instance.materialIndex;
}
