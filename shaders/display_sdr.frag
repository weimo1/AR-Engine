#version 460

layout(set = 0, binding = 0) uniform sampler2D hdrAttachment;
layout(push_constant) uniform DisplayPush {
    float exposureEV;
    uint encodeSrgb;
} pc;
layout(location = 0) out vec4 displayColor;

// Rows of the color transforms are passed as GLSL columns for row-vector multiplication.
const mat3 rec709ToAP1 = mat3(
    vec3(0.613097, 0.339523, 0.047379),
    vec3(0.070194, 0.916354, 0.013452),
    vec3(0.020616, 0.109570, 0.869815)
);
const mat3 ap1ToRec709 = mat3(
    vec3(1.705051, -0.621792, -0.083259),
    vec3(-0.130257, 1.140805, -0.010548),
    vec3(-0.024003, -0.128969, 1.152972)
);
const mat3 ap1ToAP0 = mat3(
    vec3(0.6954522414, 0.1406786965, 0.1638690622),
    vec3(0.0447945634, 0.8596711185, 0.0955343182),
    vec3(-0.0055258826, 0.0040252103, 1.0015006723)
);
const mat3 ap0ToAP1 = mat3(
    vec3(1.4514393161, -0.2365107469, -0.2149285693),
    vec3(-0.0765537734, 1.1762296998, -0.0996759264),
    vec3(0.0083161484, -0.0060324498, 0.9977163014)
);
const vec3 ap1Luminance = vec3(0.2722287168, 0.6740817658, 0.0536895174);

vec3 adjustFilmInput(vec3 ap0)
{
    float maximum = max(max(ap0.r, ap0.g), ap0.b);
    float minimum = min(min(ap0.r, ap0.g), ap0.b);
    float saturation = (max(maximum, 1e-10) - max(minimum, 1e-10)) / max(maximum, 1e-2);
    vec3 difference = ap0 - ap0.gbr;
    float chroma = sqrt(dot(difference, difference) * 0.5);
    float luminanceProxy = (ap0.r + ap0.g + ap0.b + 1.75 * chroma) / 3.0;
    float sigmoidInput = (saturation - 0.4) / 0.2;
    float remaining = max(1.0 - abs(0.5 * sigmoidInput), 0.0);
    float gain = 0.025 * (1.0 + sign(sigmoidInput) * (1.0 - remaining * remaining));
    float glow = luminanceProxy <= (2.0 / 3.0) * 0.08
        ? gain
        : max(0.0, gain * (0.08 / luminanceProxy - 0.5));
    ap0 *= 1.0 + glow;

    if (saturation > 1e-6) {
        float hue = degrees(atan(2.0 * ap0.r - ap0.g - ap0.b, sqrt(3.0) * (ap0.g - ap0.b)));
        float weight = smoothstep(0.0, 1.0, 1.0 - abs(2.0 * hue / 135.0));
        ap0.r += weight * weight * saturation * (0.03 - ap0.r) * 0.18;
    }
    return ap0;
}

vec3 filmAces(vec3 rec709)
{
    vec3 ap0 = adjustFilmInput(rec709 * rec709ToAP1 * ap1ToAP0);
    vec3 ap1 = max(vec3(0.0), ap0 * ap0ToAP1);
    ap1 = mix(vec3(dot(ap1, ap1Luminance)), ap1, 0.96);

    const float slope = 0.91;
    const float toe = 0.55;
    const float shoulder = 0.26;
    const float whiteClip = 0.04;
    const float toeScale = 1.0 - toe;
    const float shoulderScale = 1.0 + whiteClip - shoulder;
    const float toeRatio = 0.18 / toeScale - 1.0;
    const float toeMatch = log(0.18) / log(10.0) -
        0.5 * log((1.0 + toeRatio) / (1.0 - toeRatio)) * toeScale / slope;
    const float straightMatch = (1.0 - toe) / slope - toeMatch;
    const float shoulderMatch = shoulder / slope - straightMatch;

    vec3 logColor = log(max(ap1, vec3(1e-12))) / log(10.0);
    vec3 straight = slope * (logColor + straightMatch);
    vec3 low = 2.0 * toeScale / (1.0 + exp((-2.0 * slope / toeScale) * (logColor - toeMatch)));
    vec3 high = 1.0 + whiteClip -
        2.0 * shoulderScale / (1.0 + exp((2.0 * slope / shoulderScale) * (logColor - shoulderMatch)));
    low = mix(straight, low, lessThan(logColor, vec3(toeMatch)));
    high = mix(straight, high, greaterThan(logColor, vec3(shoulderMatch)));
    vec3 weight = smoothstep(min(toeMatch, shoulderMatch), max(toeMatch, shoulderMatch), logColor);
    vec3 tone = mix(low, high, weight);
    tone = mix(vec3(dot(tone, ap1Luminance)), tone, 0.93);
    return clamp(max(tone, vec3(0.0)) * ap1ToRec709, 0.0, 1.0);
}

void main()
{
    vec3 linearScene = texelFetch(hdrAttachment, ivec2(gl_FragCoord.xy), 0).rgb;
    displayColor = vec4(0.0, 0.0, 0.0, 1.0);
    if (all(equal(linearScene, vec3(0.0))))
        return;

    vec3 rgb = filmAces(linearScene * exp2(pc.exposureEV));
    if (pc.encodeSrgb != 0) {
        rgb = mix(1.055 * pow(rgb, vec3(1.0 / 2.4)) - 0.055, 12.92 * rgb,
                  lessThanEqual(rgb, vec3(0.0031308)));
    }
    displayColor.rgb = rgb;
}
