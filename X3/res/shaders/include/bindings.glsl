// bindings.glsl - All shader buffer and image bindings

// Image outputs
<<<<<<< HEAD
layout (rgba32f, SET(0) binding = 0) uniform image2D rayTracingTexture;

// Texture samplers
layout (SET(0) binding = 1) uniform sampler2D skyboxTexture;

// Camera UBO (std140, binding 0)
layout (std140, SET(1) binding = 0) uniform CameraUBO {
=======
layout (rgba32f, binding = 0) uniform image2D rayTracingTexture;

// Texture samplers
layout (binding = 1) uniform sampler2D skyboxTexture;

// Camera UBO (std140, binding 0)
layout (std140, binding = 0) uniform CameraUBO {
>>>>>>> 8275053554e488a45f912f16258d004ad274a8a3
    mat4 u_CameraTransform;
    float u_FocalLength;
};

// Settings UBO (std140, binding 1)
<<<<<<< HEAD
layout (std140, SET(1) binding = 1) uniform SettingsUBO {
=======
layout (std140, binding = 1) uniform SettingsUBO {
>>>>>>> 8275053554e488a45f912f16258d004ad274a8a3
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
<<<<<<< HEAD
layout (std430, SET(2) binding = 0) readonly buffer EntityLookupSSBO {
=======
layout (std430, binding = 0) readonly buffer EntityLookupSSBO {
>>>>>>> 8275053554e488a45f912f16258d004ad274a8a3
    EntityHandle EntityLookupTable[];
};

// Transform matrices
<<<<<<< HEAD
layout (std430, SET(2) binding = 1) readonly buffer TransformSSBO {
=======
layout (std430, binding = 1) readonly buffer TransformSSBO {
>>>>>>> 8275053554e488a45f912f16258d004ad274a8a3
    mat4 TransformBuffer[];
};

// Materials
<<<<<<< HEAD
layout (std430, SET(2) binding = 2) readonly buffer MaterialSSBO {
=======
layout (std430, binding = 2) readonly buffer MaterialSSBO {
>>>>>>> 8275053554e488a45f912f16258d004ad274a8a3
    Material MaterialBuffer[];
};

// Triangle mesh data
<<<<<<< HEAD
layout (std430, SET(2) binding = 3) readonly buffer MeshBufferSSBO {
=======
layout (std430, binding = 3) readonly buffer MeshBufferSSBO {
>>>>>>> 8275053554e488a45f912f16258d004ad274a8a3
    Triangle MeshBuffer[];
};

// BVH nodes
<<<<<<< HEAD
layout (std430, SET(2) binding = 4) readonly buffer NodeBufferSSBO {
=======
layout (std430, binding = 4) readonly buffer NodeBufferSSBO {
>>>>>>> 8275053554e488a45f912f16258d004ad274a8a3
    BVHNode NodeBuffer[];
};

// Triangle index buffer (for BVH)
<<<<<<< HEAD
layout (std430, SET(2) binding = 5) readonly buffer IndexBufferSSBO {
=======
layout (std430, binding = 5) readonly buffer IndexBufferSSBO {
>>>>>>> 8275053554e488a45f912f16258d004ad274a8a3
    uint IndexBuffer[];
};

// Lights
<<<<<<< HEAD
layout (std430, SET(2) binding = 6) readonly buffer LightBufferSSBO {
=======
layout (std430, binding = 6) readonly buffer LightBufferSSBO {
>>>>>>> 8275053554e488a45f912f16258d004ad274a8a3
    LightData LightBuffer[];
};

// UV coordinates (3 per triangle)
<<<<<<< HEAD
layout (std430, SET(2) binding = 7) readonly buffer UVBufferSSBO {
=======
layout (std430, binding = 7) readonly buffer UVBufferSSBO {
>>>>>>> 8275053554e488a45f912f16258d004ad274a8a3
    vec2 UVBuffer[];
};

// Skybox sampling helper
vec3 GetSkyboxLight(vec3 dir) {
    float u = 0.5 + atan(dir.z, dir.x) * INV_TWOPI;
    float v = 0.5 + asin(clamp(dir.y, -1.0, 1.0)) * INV_PI;
    return texture(skyboxTexture, vec2(u, v)).rgb;
}
