struct StaticInstance {
    mat4 clipFromModel;
    vec4 normalColumn0;
    vec4 normalColumn1;
    vec4 normalColumn2;
    vec4 boundsMin;
    vec4 boundsMax;
    uint firstIndex;
    uint indexCount;
    int vertexOffset;
    uint materialIndex;
};

struct StaticMaterial {
    vec4 baseColor;
    float metallic;
    float roughness;
    uint albedoSlot;
    uint metalRoughSlot;
};
