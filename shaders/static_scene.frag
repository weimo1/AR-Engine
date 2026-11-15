#version 450
layout(set = 0, binding = 0) uniform sampler2D baseColor;
layout(location = 0) in vec2 uv;
layout(location = 0) out vec4 color;

void main() {
    vec4 texel = texture(baseColor, uv);
    vec3 linearLow = texel.rgb / 12.92;
    vec3 linearHigh = pow((texel.rgb + 0.055) / 1.055, vec3(2.4));
    color = vec4(mix(linearHigh, linearLow, lessThanEqual(texel.rgb, vec3(0.04045))), texel.a);
}
