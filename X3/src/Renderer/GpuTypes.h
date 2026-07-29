#pragma once

// =============================================================================
// GpuTypes.h -- THE single C++ home for every struct mirrored into GLSL.
//
// Before Phase 2 these lived in three places: `Triangle`/`Material` in
// Project/Assets/AssetTypes.h, `MeshEntityHandle`/`LightData` as PRIVATE NESTED
// types of class Renderer, and `BVHNode` hand-copied into all three .comp files.
// Being nested inside Renderer is why no static_assert could ever be written
// against them -- offsetof on a private nested type is not reachable from
// namespace scope. Moving them here is what makes the assert block at the bottom
// of this file possible, and it is the obvious target for Phase 3's Slang
// reflection codegen to overwrite.
//
// THE MIRROR. X3/res/shaders/GpuTypes.glsl declares the same structs for the
// shader side, and is #include-d by all three .comp files. An edit to either
// file MUST be matched in the other. The static_asserts below verify the C++
// half only -- nothing here can see the GLSL. That is the honest limit of the
// interim measure, and Phase 3 (reflection-generated tables) is what removes it.
//
// LAYOUT RULE. Every struct here is built from vec4/uvec4/uint so that std430,
// std140 and scalar block layouts all agree on it. Do not add a bare vec3 or a
// float to any of them; a vec3 followed by a float packs in std430 and scalar
// but NOT in std140, and the resulting mismatch is silent.
// =============================================================================

#include "lrpch.h"          // glm
#include <cstddef>          // offsetof
#include <cstdint>
#include <type_traits>

namespace X3::Gpu
{

	// -------------------------------------------------------------------------
	// 48 B. BVH-ONLY. Byte-identical to the pre-Phase-2 X3::Triangle.
	//
	// De-referenced positions: the three corners written out in full rather than
	// indexed. That redundancy is deliberate and is the single most important
	// layout decision of Phase 2. BVH traversal tests hundreds of triangles per
	// ray and needs ONLY positions; indexing them would cost three scattered
	// index loads and then three dependent scattered position loads per test.
	// This costs one contiguous 48-byte load. Attributes are indexed instead
	// (see TriRef/Vertex) because they are fetched once per RAY, not per test.
	// -------------------------------------------------------------------------
	struct TrianglePositions {
		glm::vec4 v0{}, v1{}, v2{};
	};

	// -------------------------------------------------------------------------
	// 16 B. The attribute half of a triangle, in lockstep with TrianglePositions
	// -- both buffers are appended together and are always the same length.
	//
	// i0/i1/i2 are GLOBAL indices into AssetPool::VertexBuffer. The importer adds
	// the mesh's firstVertexIdx when writing them, which costs one add at import
	// and saves one add per hit in the shader.
	//
	// materialSlot is MESH-LOCAL (dense, 0..materialSlotCount-1). It rides in the
	// fourth lane that alignment would otherwise waste, so per-submesh materials
	// cost zero extra loads: the fetch is already happening for the indices.
	//
	// Phase 7 note: a tightly-packed uint32 index buffer for vkCmdBindIndexBuffer
	// is trivially extracted from i0/i1/i2 when the rasterizer needs one. Do not
	// build it before then.
	// -------------------------------------------------------------------------
	struct TriRef {
		uint32_t i0 = 0, i1 = 0, i2 = 0;
		uint32_t materialSlot = 0;
	};

	// -------------------------------------------------------------------------
	// 48 B. De-duplicated vertex (assimp's JoinIdenticalVertices does the dedup).
	//
	// UVs live in the .w lanes that three vec4s would otherwise pad out, so this
	// layout wastes NOTHING despite being unpacked. No octahedral normals, no
	// half-float UVs, no quantization -- deliberately. Phase 2 introduces normals
	// to this engine for the first time and its risk is correctness; an
	// encode/decode layer on the same commit makes every wrong-looking pixel
	// ambiguous between "my interpolation is wrong" and "my decode is wrong".
	// Phase 9's cook step re-lays-out this data anyway (meshoptimizer), which is
	// where quantization belongs.
	//
	// tangent.w is the handedness sign: B = cross(N, T) * tangent.w. The
	// bitangent is not stored (matches glTF, saves 12 B/vertex).
	// tangent.w == 0.0 is THE invalid-tangent sentinel -- the mesh had no UVs, so
	// assimp's CalcTangentSpace produced nothing and normal mapping must be
	// skipped in-shader. Test the sentinel, never a zero-length tangent vector.
	// -------------------------------------------------------------------------
	struct Vertex {
		glm::vec4 positionU{};   // xyz position, w uv.x
		glm::vec4 normalV{};     // xyz normal,   w uv.y
		glm::vec4 tangent{};     // xyz tangent,  w handedness (+1/-1); 0 == none
	};

	// Sentinel written into Material::textures for "no texture bound". Mirrored
	// as X3_INVALID_TEXTURE in GpuTypes.slang.
	static constexpr uint32_t INVALID_TEXTURE = 0xFFFFFFFFu;

	// Material::flags.y when the material uses no extended lobes and therefore
	// has no MaterialExt entry. Mirrored as X3_NO_MATERIAL_EXT.
	static constexpr uint32_t NO_MATERIAL_EXT = 0xFFFFFFFFu;

	// Bits in Material::flags.x. Mirrored as MATERIAL_FEATURE_* in Bsdf.slang,
	// which is what selects which BSDF a shader instantiates.
	enum MaterialFeature : uint32_t {
		MATERIAL_FEATURE_CLEARCOAT  = 1u << 0,
		MATERIAL_FEATURE_SHEEN      = 1u << 1,
		MATERIAL_FEATURE_ANISOTROPY = 1u << 2,
	};

	// -------------------------------------------------------------------------
	// 64 B. The first 48 B are byte-identical to the pre-Phase-2 X3::Material, so
	// emission / color / pbrParams.xyz keep their meanings and every shader read
	// of them is unchanged.
	//
	// pbrParams.w was dead padding; it now carries normalScale.
	// textures holds indices into the bound material texture table (see
	// TextureTable.h), NOT GUIDs -- the GUID -> index resolve happens in
	// Renderer::Parse. INVALID_TEXTURE means "use the factor only".
	// -------------------------------------------------------------------------
	struct Material {
		glm::vec4  emission  = { 0.0f, 1.0f, 0.0f, 1.0f }; // xyz colour, w strength
		glm::vec4  color     = { 0.0f, 0.0f, 0.0f, 1.0f }; // xyz baseColor factor, w alpha
		glm::vec4  pbrParams = { 0.0f, 0.5f, 1.0f, 1.0f }; // x metallic, y roughness, z ao, w normalScale
		glm::uvec4 textures  = { INVALID_TEXTURE, INVALID_TEXTURE,
		                         INVALID_TEXTURE, INVALID_TEXTURE };
		                         // x baseColor, y normal, z metalRough, w emissive
		glm::uvec4 flags     = { 0u, NO_MATERIAL_EXT, 0u, 0u };
		                         // x feature bits, y MaterialExt index
	};

	// -------------------------------------------------------------------------
	// 64 B. THE SECOND TIER of the material model.
	//
	// Only materials whose feature bits name an extended lobe get one of these,
	// so a scene of plain metallic-roughness materials uploads an empty buffer
	// and every shader instantiated on MetalRoughBsdf has the whole thing
	// eliminated at link time. That is precisely why these are not just more
	// fields on Material: the plan's tiering is only meaningful if the tier you
	// do not use costs nothing, and a wider struct would cost bandwidth on every
	// material in the scene.
	// -------------------------------------------------------------------------
	struct MaterialExt {
		glm::vec4 clearcoat = { 0.0f, 0.1f, 1.5f, 0.0f };  // weight, roughness, ior, pad
		glm::vec4 sheen     = { 0.0f, 0.0f, 0.0f, 0.3f };  // colour, roughness
		glm::vec4 aniso     = { 0.0f, 0.0f, 0.0f, 1.5f };  // anisotropy, rotation, thinFilm thickness/ior
		glm::vec4 specular  = { 0.5f, 1.0f, 1.0f, 1.0f };  // level, tint
	};

	// -------------------------------------------------------------------------
	// 32 B (was 24). One renderable entity's slice of every asset buffer.
	//
	// The single materialIdx became materialBase + materialSlotCount because
	// materials moved from per-entity to per-submesh: entity i's materials occupy
	// MaterialBuffer[materialBase, materialBase + materialSlotCount). The BVH is
	// still ONE per mesh asset spanning all submeshes -- splitting it per submesh
	// would multiply the linear entity loop in CheckRayCollision by the submesh
	// count for no gain, since the SAH build handles the concatenation fine.
	// -------------------------------------------------------------------------
	struct MeshEntityHandle {
		uint32_t firstTriIdx  = 0;
		uint32_t triCount     = 0;
		uint32_t firstNodeIdx = 0;
		uint32_t nodeCount    = 0;
		uint32_t transformIdx = 0;
		uint32_t materialBase = 0;   // index of this entity's slot 0 in MaterialBuffer
		uint32_t materialSlotCount = 0;
		uint32_t firstVertexIdx = 0; // unused by the tracer (TriRef indices are global); Phase 7 binds from here
	};

	// -------------------------------------------------------------------------
	// 64 B. Moved verbatim out of Renderer.h.
	// -------------------------------------------------------------------------
	struct LightData {
		glm::vec4 position{};    // xyz position, w type (0=directional, 1=point, 2=spot)
		glm::vec4 direction{};   // xyz direction, w intensity
		glm::vec4 color{};       // xyz colour, w range
		glm::vec4 params{};      // x attenuation, y innerCone, z outerCone, w softness (radians)
	};

	// -------------------------------------------------------------------------
	// CLUSTERED FORWARD+ -- the light-culling grid. Mirrors the X3_CLUSTER_*
	// constants and ClusterAABB in res/shaders/GpuTypes.slang; the shader's own
	// comments carry the reasoning for the grid shape and the exponential depth
	// slicing.
	// -------------------------------------------------------------------------
	inline constexpr uint32_t CLUSTER_X = 16;
	inline constexpr uint32_t CLUSTER_Y = 9;
	inline constexpr uint32_t CLUSTER_Z = 24;
	inline constexpr uint32_t CLUSTER_COUNT = CLUSTER_X * CLUSTER_Y * CLUSTER_Z;
	inline constexpr uint32_t MAX_LIGHTS_PER_CLUSTER = 64;

	// 32 B. View space -- the grid is fixed to the camera, so it changes when the
	// PROJECTION does rather than when the camera moves.
	struct ClusterAABB {
		glm::vec4 minPoint{};
		glm::vec4 maxPoint{};
	};

	// -------------------------------------------------------------------------
	// CASCADED SHADOW MAPS. Mirrors X3_SHADOW_* in res/shaders/GpuTypes.slang.
	// -------------------------------------------------------------------------
	inline constexpr uint32_t SHADOW_CASCADES        = 4;
	inline constexpr uint32_t SHADOW_MAP_RESOLUTION  = 1024;
	inline constexpr uint32_t NO_SHADOW_LIGHT        = 0xFFFFFFFFu;

	// =========================================================================
	// LAYOUT ASSERTS -- the interim measure, per ENGINE_PLAN.md Phase 2.
	//
	// THESE VERIFY THE C++ SIDE ONLY. They catch ABI and padding surprises (a
	// different compiler, a glm built with SIMD alignment, a member reordered by
	// accident). They CANNOT see GpuTypes.glsl, so they cannot catch a shader
	// struct that drifted. Collapsing the GLSL to one included file is what
	// reduces that risk today; Phase 3's Slang reflection is what removes it.
	// =========================================================================

	// glm must be tightly packed for any of the offsets below to hold.
	//
	// SIZE is asserted; ALIGNMENT deliberately is not. This project builds glm in
	// its default `packed_highp` configuration, where alignof(glm::vec4) is 4 and
	// not 16 (GLM_FORCE_DEFAULT_ALIGNED_GENTYPES is not defined). That is
	// harmless for every struct in this file: each one begins with a vec4 and has
	// a size that is a multiple of 16, so the ARRAY STRIDE the GPU sees is
	// identical to std430 either way, and the offsetof asserts below pin the
	// interior. Asserting alignment == 16 would fail here while proving nothing
	// the offsets do not already prove.
	static_assert(sizeof(glm::vec4)  == 16);
	static_assert(sizeof(glm::uvec4) == 16);
	static_assert(sizeof(glm::vec3)  == 12);
	static_assert(sizeof(glm::mat4)  == 64);

	// offsetof is only well-defined on standard-layout types.
	static_assert(std::is_standard_layout_v<TrianglePositions>);
	static_assert(std::is_standard_layout_v<TriRef>);
	static_assert(std::is_standard_layout_v<Vertex>);
	static_assert(std::is_standard_layout_v<Material>);
	static_assert(std::is_standard_layout_v<MaterialExt>);
	static_assert(std::is_standard_layout_v<MeshEntityHandle>);
	static_assert(std::is_standard_layout_v<LightData>);

	// --- TrianglePositions : std430 48 B ---
	static_assert(sizeof(TrianglePositions) == 48);
	static_assert(offsetof(TrianglePositions, v0) ==  0);
	static_assert(offsetof(TrianglePositions, v1) == 16);
	static_assert(offsetof(TrianglePositions, v2) == 32);

	// --- TriRef : std430 16 B ---
	static_assert(sizeof(TriRef) == 16);
	static_assert(offsetof(TriRef, i0)           ==  0);
	static_assert(offsetof(TriRef, i1)           ==  4);
	static_assert(offsetof(TriRef, i2)           ==  8);
	static_assert(offsetof(TriRef, materialSlot) == 12);

	// --- Vertex : std430 48 B ---
	static_assert(sizeof(Vertex) == 48);
	static_assert(offsetof(Vertex, positionU) ==  0);
	static_assert(offsetof(Vertex, normalV)   == 16);
	static_assert(offsetof(Vertex, tangent)   == 32);

	// --- Material : std430 80 B ---
	static_assert(sizeof(Material) == 80);
	static_assert(offsetof(Material, emission)  ==  0);
	static_assert(offsetof(Material, color)     == 16);
	static_assert(offsetof(Material, pbrParams) == 32);
	static_assert(offsetof(Material, textures)  == 48);
	static_assert(offsetof(Material, flags)     == 64);

	// --- MaterialExt : std430 64 B ---
	static_assert(std::is_standard_layout_v<MaterialExt>);
	static_assert(sizeof(MaterialExt) == 64);
	static_assert(offsetof(MaterialExt, clearcoat) ==  0);
	static_assert(offsetof(MaterialExt, sheen)     == 16);
	static_assert(offsetof(MaterialExt, aniso)     == 32);
	static_assert(offsetof(MaterialExt, specular)  == 48);

	// --- MeshEntityHandle : std430 32 B ---
	static_assert(sizeof(MeshEntityHandle) == 32);
	static_assert(offsetof(MeshEntityHandle, firstTriIdx)       ==  0);
	static_assert(offsetof(MeshEntityHandle, triCount)          ==  4);
	static_assert(offsetof(MeshEntityHandle, firstNodeIdx)      ==  8);
	static_assert(offsetof(MeshEntityHandle, nodeCount)         == 12);
	static_assert(offsetof(MeshEntityHandle, transformIdx)      == 16);
	static_assert(offsetof(MeshEntityHandle, materialBase)      == 20);
	static_assert(offsetof(MeshEntityHandle, materialSlotCount) == 24);
	static_assert(offsetof(MeshEntityHandle, firstVertexIdx)    == 28);

	// --- LightData : std430 64 B ---
	static_assert(sizeof(LightData) == 64);
	static_assert(offsetof(LightData, position)  ==  0);
	static_assert(offsetof(LightData, direction) == 16);
	static_assert(offsetof(LightData, color)     == 32);
	static_assert(offsetof(LightData, params)    == 48);

	// --- ClusterAABB : std430 32 B ---
	static_assert(sizeof(ClusterAABB) == 32);
	static_assert(offsetof(ClusterAABB, minPoint) ==  0);
	static_assert(offsetof(ClusterAABB, maxPoint) == 16);

}
