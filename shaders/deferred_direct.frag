#version 460

layout(set = 0, binding = 0) uniform sampler2D baseColorAttachment;
layout(set = 0, binding = 1) uniform sampler2D normalAttachment;
layout(set = 0, binding = 2) uniform sampler2D materialAttachment;
layout(set = 0, binding = 3) uniform sampler2D depthAttachment;

#ifdef AR_ENVIRONMENT_LIGHTING
layout(set = 0, binding = 4) uniform samplerCube diffuseEnvironment;
layout(set = 0, binding = 5) uniform samplerCube specularEnvironment;
layout(set = 0, binding = 6) uniform sampler2D environmentBrdf;
#endif

layout(push_constant) uniform LightingPush {
    mat4 inverseProjection;
#ifdef AR_ENVIRONMENT_LIGHTING
    mat3 viewToWorld;
#endif
} pc;

layout(location = 0) out vec4 hdrColor;

const float pi = 3.14159265358979323846;

float smithG1(float cosine, float alphaSquared)
{
    return 2.0 * cosine /
           (cosine + sqrt(alphaSquared + (1.0 - alphaSquared) * cosine * cosine));
}

void main()
{
    ivec2 pixel = ivec2(gl_FragCoord.xy);
    float depth = texelFetch(depthAttachment, pixel, 0).r;
    hdrColor = vec4(0.0, 0.0, 0.0, 1.0);
    if (depth == 0.0)
        return;

    vec2 ndc = 2.0 * gl_FragCoord.xy / vec2(textureSize(depthAttachment, 0)) - 1.0;
    vec4 viewPosition = pc.inverseProjection * vec4(ndc.x, -ndc.y, depth, 1.0);
    vec3 view = normalize(-viewPosition.xyz / viewPosition.w);
    vec3 storedNormal = texelFetch(normalAttachment, pixel, 0).xyz;
    float normalLengthSquared = dot(storedNormal, storedNormal);
    if (normalLengthSquared <= 1e-8)
        return;
    vec3 normal = storedNormal * inversesqrt(normalLengthSquared);
    const vec3 light = vec3(0.0, 0.0, 1.0);
    float nl = dot(normal, light);
    float nv = dot(normal, view);
    if (nv <= 0.0)
        return;

    vec3 baseColor = texelFetch(baseColorAttachment, pixel, 0).rgb;
    vec2 material = texelFetch(materialAttachment, pixel, 0).rg;
    vec3 f0 = mix(vec3(0.04), baseColor, material.x);
    vec3 lighting = vec3(0.0);
#ifdef AR_ENVIRONMENT_LIGHTING
    vec3 worldNormal = pc.viewToWorld * normal;
    vec3 worldReflection = pc.viewToWorld * reflect(-view, normal);
    vec2 integrated = texture(environmentBrdf, vec2(nv, material.y)).rg;
    vec3 irradiance = texture(diffuseEnvironment, worldNormal).rgb;
    vec3 prefiltered = textureLod(specularEnvironment, worldReflection, material.y * 4.0).rgb;
    lighting = irradiance * (1.0 - material.x) * (1.0 - f0) * baseColor +
               prefiltered * (f0 * integrated.x + integrated.y);
#endif
    if (nl > 0.0) {
        vec3 halfVector = normalize(view + light);
        float nh = clamp(dot(normal, halfVector), 0.0, 1.0);
        float vh = clamp(dot(view, halfVector), 0.0, 1.0);
        float alpha = max(material.y * material.y, 0.002);
        float alphaSquared = alpha * alpha;
        float denominator = (1.0 - nh * nh) + nh * nh * alphaSquared;
        float distribution = alphaSquared / (pi * denominator * denominator);
        float masking = smithG1(nl, alphaSquared) * smithG1(nv, alphaSquared);
        vec3 fresnel = f0 + (1.0 - f0) * pow(1.0 - vh, 5.0);
        vec3 diffuse = (1.0 - material.x) * (1.0 - fresnel) * baseColor / pi;
        vec3 specular = distribution * masking * fresnel / max(4.0 * nl * nv, 1e-5);
        lighting += 6.0 * nl * (diffuse + specular);
    }
    hdrColor.rgb = min(lighting, vec3(65504.0));
}
