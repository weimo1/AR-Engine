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
layout(std140, set = 1, binding = 4) uniform FrameView {
    mat4 viewProj;
    mat4 view;
} frameView;

void main() {
    StaticInstance instance = instances[gl_InstanceIndex];
    mat4 clipFromModel = frameView.viewProj * instance.modelMatrix;
    gl_Position = clipFromModel * vec4(position, 1.0);
    outUv = uv;
    mat3 worldNormal = mat3(instance.normalColumn0.xyz, instance.normalColumn1.xyz,
                            instance.normalColumn2.xyz);
    viewNormal = (mat3(frameView.view) * worldNormal) * normal;
    materialIndex = instance.materialIndex;
}
