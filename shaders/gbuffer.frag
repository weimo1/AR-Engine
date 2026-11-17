#version 450

layout(set = 0, binding = 0) uniform sampler2D baseColor;
layout(set = 0, binding = 1) uniform sampler2D metallicRoughness;
layout(location = 0) in vec2 uv;
layout(location = 1) in vec3 viewNormal;
layout(location = 0) out vec4 preview;
layout(location = 1) out vec4 gBaseColor;
layout(location = 2) out vec4 gNormal;
layout(location = 3) out vec2 gMaterial;

layout(push_constant) uniform Draw {
    mat4 clipFromModel;
    vec4 normalColumn0;
    vec4 normalColumn1;
    vec4 normalColumn2;
    vec4 baseColorFactor;
} draw;

vec3 linearColor(vec3 encoded) {
    return mix(pow((encoded + 0.055) / 1.055, vec3(2.4)), encoded / 12.92,
               lessThanEqual(encoded, vec3(0.04045)));
}

void main() {
    vec4 encoded = texture(baseColor, uv);
    vec4 linear = vec4(linearColor(encoded.rgb), encoded.a) * draw.baseColorFactor;
    vec4 data = texture(metallicRoughness, uv);
    gBaseColor = linear;
    gNormal = vec4(normalize(viewNormal), 0.0);
    gMaterial = data.bg * vec2(draw.normalColumn0.w, draw.normalColumn1.w);
    preview = vec4(linear.rgb, 1.0);
}
