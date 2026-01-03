// common.glsl - Shared constants and data structures
// These structs MUST match their CPU counterparts exactly (std430 layout)

// OpenGL doesn't support Vulkan descriptor sets (set = X)
// Use macros to make shader portable between APIs
#ifdef VULKAN
    #define SET(x) set = x,
#else
    #define SET(x)
#endif

const float PI = 3.1415926535897932;
const float INV_PI = 0.3183098861837907;
const float INV_TWOPI = 0.15915494309189535;
const float INF_T = 1e30;
const float SURFACE_BIAS = 1e-4;
const float GAMMA = 0.8;

// Debug counters (per-invocation)
uint g_AabbIntersectionCount = 0;
uint g_TriIntersectionCount = 0;

// std430 - 32 bytes (CPU: Assets/BVHAccel.h)
struct BVHNode {
    vec3 min;
    uint leftChild_Or_FirstTri;
    vec3 max;
    uint triCount;
    // if triCount == 0: leftChild_Or_FirstTri is left child index
    // if triCount > 0: leftChild_Or_FirstTri is first triangle index
};

// std430 - 48 bytes (CPU: Assets/AssetTypes.h)
struct Triangle {
    vec4 v0, v1, v2;
};

// std430 - 24 bytes (CPU: Renderer/Renderer.h)
struct EntityHandle {
    uint rootTriIdx;
    uint triCount;
    uint rootNodeIdx;
    uint nodeCount;
    uint transformIdx;
    uint materialIdx;
};

// std430 - 96 bytes (CPU: Assets/AssetTypes.h)
struct Material {
    vec4 emission;   // .xyz = color, .w = strength
    vec4 color;      // .xyz = albedo, .w = padding
    vec4 pbrParams;  // .x = metallic, .y = roughness, .z = ao, .w = padding

    // Texture indices (-1 = use scalar value)
    int albedoTexIdx;
    int normalTexIdx;
    int metallicTexIdx;
    int roughnessTexIdx;
    int aoTexIdx;
    int emissionTexIdx;
    int _pad0;
    int _pad1;
};

// std430 - 64 bytes (CPU: Renderer/Renderer.h)
struct LightData {
    vec4 position;   // .xyz = position (point/spot), .w = type (0=dir, 1=point, 2=spot)
    vec4 direction;  // .xyz = direction (dir/spot), .w = intensity
    vec4 color;      // .xyz = color, .w = range
    vec4 params;     // .x = attenuation, .y = innerConeAngle, .z = outerConeAngle, .w = padding
};

// Shader-only struct (no CPU counterpart)
struct Ray {
    vec3 origin;
    float t;
    vec3 dir;
    uint materialIdx;
    vec3 normal;
    vec2 uv;
    uint hitTriIdx;
};

// Light type constants
const uint LIGHT_TYPE_DIRECTIONAL = 0u;
const uint LIGHT_TYPE_POINT = 1u;
const uint LIGHT_TYPE_SPOT = 2u;
