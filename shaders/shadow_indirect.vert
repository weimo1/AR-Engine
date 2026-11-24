#version 450
#extension GL_GOOGLE_include_directive : require
#include "static_indirect_scene.glsl"

layout(location = 0) in vec3 position;

layout(std430, set = 0, binding = 0) readonly buffer Instances {
    StaticInstance instances[];
};

layout(push_constant) uniform ShadowIndirectPush {
    mat4 lightViewProj;
} pc;

void main() {
    StaticInstance instance = instances[gl_InstanceIndex];
    gl_Position = pc.lightViewProj * (instance.modelMatrix * vec4(position, 1.0));
}
