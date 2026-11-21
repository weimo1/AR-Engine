#version 450

#ifdef AR_BINDLESS_MATERIALS
#extension GL_EXT_nonuniform_qualifier : require
layout(set = 0, binding = 0) uniform texture2D images[];
layout(set = 0, binding = 1) uniform sampler nearestRepeat;
#else
layout(set = 0, binding = 0) uniform sampler2D baseColor;
layout(set = 0, binding = 1) uniform sampler2D metallicRoughness;
#endif
#ifdef AR_STATIC_INDIRECT
#extension GL_GOOGLE_include_directive : require
#include "static_indirect_scene.glsl"
layout(location = 2) flat in uint materialIndex;
layout(std430, set = 1, binding = 1) readonly buffer Materials {
    StaticMaterial materials[];
};
#endif
layout(location = 0) in vec2 uv;
layout(location = 1) in vec3 viewNormal;
layout(location = 0) out vec4 preview;
layout(location = 1) out vec4 gBaseColor;
layout(location = 2) out vec4 gNormal;
layout(location = 3) out vec2 gMaterial;

#ifndef AR_STATIC_INDIRECT
layout(push_constant) uniform Draw {
    mat4 clipFromModel;
    vec4 normalColumn0;
    vec4 normalColumn1;
    vec4 normalColumn2;
    vec4 baseColorFactor;
} draw;
#endif

vec3 linearColor(vec3 encoded) {
    return mix(pow((encoded + 0.055) / 1.055, vec3(2.4)), encoded / 12.92,
               lessThanEqual(encoded, vec3(0.04045)));
}

void main() {
#if defined(AR_STATIC_INDIRECT)
    StaticMaterial material = materials[materialIndex];
    vec4 encoded = texture(sampler2D(images[nonuniformEXT(material.albedoSlot)], nearestRepeat), uv);
    vec4 data = texture(sampler2D(images[nonuniformEXT(material.metalRoughSlot)], nearestRepeat), uv);
    vec4 baseFactor = material.baseColor;
    vec2 dataFactor = vec2(material.metallic, material.roughness);
#else
#ifdef AR_BINDLESS_MATERIALS
    uint slots = uint(draw.normalColumn2.w);
    vec4 encoded = texture(sampler2D(images[nonuniformEXT(slots & 255u)], nearestRepeat), uv);
    vec4 data = texture(sampler2D(images[nonuniformEXT(slots >> 8u)], nearestRepeat), uv);
#else
    vec4 encoded = texture(baseColor, uv);
    vec4 data = texture(metallicRoughness, uv);
#endif
    vec4 baseFactor = draw.baseColorFactor;
    vec2 dataFactor = vec2(draw.normalColumn0.w, draw.normalColumn1.w);
#endif
    vec4 linear = vec4(linearColor(encoded.rgb), encoded.a) * baseFactor;
    gBaseColor = linear;
    gNormal = vec4(normalize(viewNormal), 0.0);
    gMaterial = data.bg * dataFactor;
    preview = vec4(linear.rgb, 1.0);
}
