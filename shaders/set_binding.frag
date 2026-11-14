#version 450
#extension GL_EXT_nonuniform_qualifier : require

layout(set = 0, binding = 0) uniform texture2D textures[AR_SET_IMAGE_COUNT];
layout(set = 0, binding = 1) uniform sampler samplers[AR_SET_SAMPLER_COUNT];
layout(location = 0) in vec2 screenUv;
layout(location = 0) out vec4 color;

void main() {
    const uint imageSlots[] = uint[](0, 1, 2, 1);
    const uint samplerSlots[] = uint[](0, 0, 0, 1);
    uint material = uint(clamp(floor(screenUv.x * 4.0), 0.0, 3.0));
    color = texture(sampler2D(textures[nonuniformEXT(imageSlots[material])],
                              samplers[nonuniformEXT(samplerSlots[material])]),
                    vec2(material == 3 ? 0.5 : 0.49, 0.5));
}
