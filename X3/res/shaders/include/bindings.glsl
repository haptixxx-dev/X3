// bindings.glsl - All shader buffer and image bindings

// Image outputs
layout (rgba32f, binding = 0) uniform image2D rayTracingTexture;

// Texture samplers
layout (binding = 1) uniform sampler2D skyboxTexture;

// Camera UBO (std140, binding 0)
layout (std140, binding = 0) uniform CameraUBO {
    mat4 u_CameraTransform;
    float u_FocalLength;
};

// Settings UBO (std140, binding 1)
layout (std140, binding = 1) uniform SettingsUBO {
    uint u_RaysPerPixel;
    uint u_BouncesPerRay;
    uint u_numAccumulatedFrames;
    uint u_EntityCount;
    uint u_DebugMode;
    uint u_AabbHeatmapCutoff;
    uint u_TriHeatmapCutoff;
    uint u_LightCount;
};

// Entity lookup table
layout (std430, binding = 0) readonly buffer EntityLookupSSBO {
    EntityHandle EntityLookupTable[];
};

// Transform matrices
layout (std430, binding = 1) readonly buffer TransformSSBO {
    mat4 TransformBuffer[];
};

// Materials
layout (std430, binding = 2) readonly buffer MaterialSSBO {
    Material MaterialBuffer[];
};

// Triangle mesh data
layout (std430, binding = 3) readonly buffer MeshBufferSSBO {
    Triangle MeshBuffer[];
};

// BVH nodes
layout (std430, binding = 4) readonly buffer NodeBufferSSBO {
    BVHNode NodeBuffer[];
};

// Triangle index buffer (for BVH)
layout (std430, binding = 5) readonly buffer IndexBufferSSBO {
    uint IndexBuffer[];
};

// Lights
layout (std430, binding = 6) readonly buffer LightBufferSSBO {
    LightData LightBuffer[];
};

// UV coordinates (3 per triangle)
layout (std430, binding = 7) readonly buffer UVBufferSSBO {
    vec2 UVBuffer[];
};

// Skybox sampling helper
vec3 GetSkyboxLight(vec3 dir) {
    float u = 0.5 + atan(dir.z, dir.x) * INV_TWOPI;
    float v = 0.5 + asin(clamp(dir.y, -1.0, 1.0)) * INV_PI;
    return texture(skyboxTexture, vec2(u, v)).rgb;
}
