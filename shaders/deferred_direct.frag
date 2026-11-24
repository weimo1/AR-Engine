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

#ifdef AR_SHADOW_MAP
layout(set = 0, binding = 7) uniform sampler2DShadow shadowMap;
#endif

layout(std140, set = 0, binding = 8) uniform LightingFrame {
    mat4 inverseProjection;
    mat4 viewToWorld;
    mat4 lightViewProj;
    vec4 lightDirView;
    vec4 lightColor;
    vec4 shadowParams;
} lighting;

layout(location = 0) out vec4 hdrColor;

const float pi = 3.14159265358979323846;

float smithG1(float cosine, float alphaSquared)
{
    return 2.0 * cosine /
           (cosine + sqrt(alphaSquared + (1.0 - alphaSquared) * cosine * cosine));
}

#ifdef AR_SHADOW_MAP
float sampleDirectionalShadow(vec3 worldPos, vec3 worldNormal)
{
    if (lighting.lightColor.w < 0.5)
        return 1.0;

    vec3 biasedPos = worldPos + worldNormal * lighting.shadowParams.x;
    vec4 lightClip = lighting.lightViewProj * vec4(biasedPos, 1.0);
    if (abs(lightClip.w) <= 1e-6)
        return 1.0;

    vec3 lightNdc = lightClip.xyz / lightClip.w;
    vec2 shadowUv = lightNdc.xy * 0.5 + 0.5;
    if (shadowUv.x <= 0.0 || shadowUv.x >= 1.0 || shadowUv.y <= 0.0 || shadowUv.y >= 1.0 ||
        lightNdc.z <= 0.0 || lightNdc.z >= 1.0)
        return 1.0;

    float refDepth = clamp(lightNdc.z - lighting.shadowParams.y, 0.0, 1.0);
    float texelStep = lighting.shadowParams.z * lighting.shadowParams.w;
    float visibility = 0.0;
    for (int dy = -1; dy <= 1; ++dy) {
        for (int dx = -1; dx <= 1; ++dx) {
            vec2 offset = vec2(float(dx), float(dy)) * texelStep;
            visibility += texture(shadowMap, vec3(shadowUv + offset, refDepth));
        }
    }
    return visibility * (1.0 / 9.0);
}
#endif

void main()
{
    ivec2 pixel = ivec2(gl_FragCoord.xy);
    float depth = texelFetch(depthAttachment, pixel, 0).r;
    hdrColor = vec4(0.0, 0.0, 0.0, 1.0);
    if (depth == 0.0)
        return;

    vec2 ndc = 2.0 * gl_FragCoord.xy / vec2(textureSize(depthAttachment, 0)) - 1.0;
    vec4 viewPosition = lighting.inverseProjection * vec4(ndc.x, -ndc.y, depth, 1.0);
    vec3 viewPos = viewPosition.xyz / viewPosition.w;
    vec3 view = normalize(-viewPos);
    vec3 storedNormal = texelFetch(normalAttachment, pixel, 0).xyz;
    float normalLengthSquared = dot(storedNormal, storedNormal);
    if (normalLengthSquared <= 1e-8)
        return;
    vec3 normal = storedNormal * inversesqrt(normalLengthSquared);
    // Keep grazing smooth normals in the front hemisphere so silhouette pixels do not drop to black.
    float rawNv = dot(normal, view);
    if (rawNv < 1e-4) {
        normal = rawNv <= -0.999 ? view : normalize(normal + view * (1e-4 - rawNv));
    }
    vec3 light = lighting.lightDirView.xyz;
    float nl = dot(normal, light);
    float nv = max(dot(normal, view), 1e-4);

    vec3 baseColor = texelFetch(baseColorAttachment, pixel, 0).rgb;
    vec2 material = texelFetch(materialAttachment, pixel, 0).rg;
    vec3 f0 = mix(vec3(0.04), baseColor, material.x);
    vec3 totalLighting = vec3(0.0);
#if defined(AR_ENVIRONMENT_LIGHTING) || defined(AR_SHADOW_MAP)
    mat3 viewToWorldRotation = mat3(lighting.viewToWorld);
    vec3 worldNormal = normalize(viewToWorldRotation * normal);
#endif
#ifdef AR_ENVIRONMENT_LIGHTING
    vec3 worldReflection = viewToWorldRotation * reflect(-view, normal);
    vec2 integrated = texture(environmentBrdf, vec2(nv, material.y)).rg;
    vec3 irradiance = texture(diffuseEnvironment, worldNormal).rgb;
    vec3 prefiltered = textureLod(specularEnvironment, worldReflection, material.y * 4.0).rgb;
    totalLighting = irradiance * (1.0 - material.x) * (1.0 - f0) * baseColor +
                    prefiltered * (f0 * integrated.x + integrated.y);
#endif
    if (nl > 0.0 && lighting.lightDirView.w > 0.0) {
        float shadowFactor = 1.0;
#ifdef AR_SHADOW_MAP
        vec3 worldPos = (lighting.viewToWorld * vec4(viewPos, 1.0)).xyz;
        shadowFactor = sampleDirectionalShadow(worldPos, worldNormal);
#endif
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
        totalLighting += shadowFactor * lighting.lightColor.rgb * lighting.lightDirView.w * nl *
                         (diffuse + specular);
    }
    hdrColor.rgb = min(totalLighting, vec3(65504.0));
}
