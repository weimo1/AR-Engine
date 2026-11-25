#version 460

layout(set = 0, binding = 0) uniform sampler2D fontTexture;

layout(push_constant) uniform OverlayPush {
    vec2 scale;
    vec2 translate;
    uint decodeSrgb;
} pc;

layout(location = 0) in vec2 inUv;
layout(location = 1) in vec4 inColor;

layout(location = 0) out vec4 outColor;

vec3 srgbToLinear(vec3 c) {
    vec3 low = c / 12.92;
    vec3 high = pow((c + 0.055) / 1.055, vec3(2.4));
    return mix(high, low, lessThanEqual(c, vec3(0.04045)));
}

void main() {
    vec4 texel = texture(fontTexture, inUv);
    vec4 color = inColor * texel;
    if (pc.decodeSrgb != 0u) {
        color.rgb = srgbToLinear(clamp(color.rgb, 0.0, 1.0));
    }
    outColor = color;
}
