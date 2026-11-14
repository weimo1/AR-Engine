#version 450

layout(location = 0) out vec2 screenUv;

void main() {
    const vec2 corners[] = vec2[](vec2(-1.0, -1.0), vec2(3.0, -1.0), vec2(-1.0, 3.0));
    vec2 position = corners[gl_VertexIndex];
    gl_Position = vec4(position, 0.0, 1.0);
    screenUv = position * 0.5 + 0.5;
}
