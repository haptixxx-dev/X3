#pragma once

#include "lrpch.h"
#include "Renderer/RenderSettings.h"
#include "Core/GUID.h"
#include "EngineCfg.h"

#include "Renderer/GpuTypes.h"
#include "Renderer/RenderGraph.h"
#include "Renderer/TextureTable.h"
#include "Project/Assets/MaterialDesc.h"

#include "Platform/Vulkan/VulkanBuffer.h"
#include "Platform/Vulkan/VulkanComputePipeline.h"
#include "Platform/Vulkan/VulkanDescriptors.h"
#include "Platform/Vulkan/VulkanGraphicsPipeline.h"
#include "Platform/Vulkan/VulkanImage.h"

// Forward declarations to reduce compilation dependencies
namespace X3 {
    class Scene;
    class AssetManager;
    class Profiler;
    class VulkanContext;
    struct AssetPool;
}

namespace X3
{

	class Renderer {
	private:
		// The three descriptor sets every compute shader in this engine declares.
		// Set 0 is images, set 1 uniforms, set 2 storage buffers, matching
		// res/shaders/Bindings.slang. The TABLE ITSELF is generated from Slang
		// reflection at build time (Renderer/Generated/DescriptorTables.h); only
		// the count is stated here, because it sizes a std::array member.
		static constexpr uint32_t kSetCount = 3;

		struct Cache {
			glm::uvec2 Resolution{0};
			uint32_t AccumulatedFrames = 0;
			LR_GUID prevSkyboxGuid = LR_GUID::INVALID;

			// Asset-pool versions, so the device-local buffers are re-uploaded only
			// when their source actually changed. These were function-local statics
			// in the old SetupGPUResources, which meant two Renderers would have
			// shared them.
			uint32_t triPositionVersion = 0;
			uint32_t nodeBufferVersion  = 0;
			uint32_t bvhPrimIndexVersion = 0;
			uint32_t triRefVersion      = 0;
			uint32_t vertexVersion      = 0;
			uint32_t textureVersion     = 0;
			bool     assetBuffersUploaded = false;

			// Velocity needs the PREVIOUS frame's camera. Held across frames here
			// rather than derived, because there is nothing to derive it from.
			glm::mat4 prevViewProj{ 1.0f };
			bool      havePrevViewProj = false;
		};

		// MeshEntityHandle and LightData used to be declared here, as PRIVATE
		// NESTED types -- which is exactly why no static_assert could ever be
		// written against their layout. They now live in Renderer/GpuTypes.h with
		// the rest of the GPU mirror, as Gpu::MeshEntityHandle and Gpu::LightData.

		// std140 - 416 bytes. Mirrored by CameraUBO in res/shaders/GpuTypes.slang.
		//
		// transform + focalLength are all the path tracer ever needed. The
		// rasterizer needs view and proj, which are not derivable from a focal
		// length: the projection also encodes aspect and the depth range, and the
		// view matrix is transform's inverse, which a vertex shader should not be
		// computing per vertex.
		struct CameraUBOData {
			glm::mat4 transform;
			glm::mat4 view;
			glm::mat4 proj;
			glm::mat4 viewProj;
			// LAST FRAME'S viewProj, for the velocity pass. Kept here rather than
			// in a UBO of its own because it is a camera property and every pass
			// that wants it already binds this one.
			glm::mat4 prevViewProj;
			// THE JITTERED ONE, which every raster pass uses for SV_Position.
			// `viewProj` above stays UNJITTERED and is what the velocity pass
			// projects with -- motion vectors must describe where the surface
			// moved, not where the sub-pixel sample moved, or TAA reprojects by
			// the jitter as well as the motion and the image swims.
			glm::mat4 viewProjJittered;
			glm::vec4 jitter;        // xy this frame, zw last frame, in NDC
			float     focalLength;
			float     nearPlane;
			float     farPlane;
			float     _pad;
		};

		// Asserted for the same reason everything in GpuTypes.h is: a silent
		// layout drift here does not fail loudly, it shades with a transposed or
		// offset matrix and produces a picture that is merely wrong. This struct
		// went from 80 B to 272 B in Phase 7 with nothing checking it.
		//
		// No alignment asserts, matching GpuTypes.h -- this project's glm is
		// packed_highp, so alignof(mat4) is 4 and asserting the std140 alignment
		// would fail on a layout that is nonetheless correct.
		static_assert(sizeof(CameraUBOData) == 416);
		static_assert(offsetof(CameraUBOData, transform)    ==   0);
		static_assert(offsetof(CameraUBOData, view)         ==  64);
		static_assert(offsetof(CameraUBOData, proj)         == 128);
		static_assert(offsetof(CameraUBOData, viewProj)     == 192);
		static_assert(offsetof(CameraUBOData, prevViewProj) == 256);
		static_assert(offsetof(CameraUBOData, viewProjJittered) == 256 + 64);
		static_assert(offsetof(CameraUBOData, jitter)       == 384);
		static_assert(offsetof(CameraUBOData, focalLength)  == 400);
		static_assert(offsetof(CameraUBOData, nearPlane)    == 404);
		static_assert(offsetof(CameraUBOData, farPlane)     == 408);

		// std140 - 304 bytes. Mirrored by ShadowUBO in res/shaders/GpuTypes.slang.
		//
		// glm::vec4 for the per-cascade scalars rather than float[4], because
		// std140 pads every array element to 16 bytes -- a float[4] occupies 64 B
		// with 48 of them padding, and this struct would have to reproduce that
		// padding exactly to stay in step with the shader.
		struct ShadowUBOData {
			std::array<glm::mat4, Gpu::SHADOW_CASCADES> viewProj{};
			glm::vec4 splitDepth{};
			glm::vec4 texelWorldSize{};
			uint32_t  shadowLightIndex = Gpu::NO_SHADOW_LIGHT;
			float     depthBiasScale   = 0.0f;
			float     _pad0 = 0.0f;
			float     _pad1 = 0.0f;
		};
		static_assert(sizeof(ShadowUBOData) == 304);
		static_assert(offsetof(ShadowUBOData, viewProj)         ==   0);
		static_assert(offsetof(ShadowUBOData, splitDepth)       == 256);
		static_assert(offsetof(ShadowUBOData, texelWorldSize)   == 272);
		static_assert(offsetof(ShadowUBOData, shadowLightIndex) == 288);
		static_assert(offsetof(ShadowUBOData, depthBiasScale)   == 292);

		// std140 - 112 bytes. Mirrored by DdgiUBO in res/shaders/GpuTypes.slang.
		struct DdgiUBOData {
			glm::vec4 gridOrigin{};
			glm::vec4 gridSpacing{};
			glm::vec4 rayRotation{ 0.0f, 0.0f, 0.0f, 1.0f };
			glm::vec4 params0{};    // x hysteresis, y depthSharpness, z normalBias, w viewBias
			glm::vec4 params1{};    // x maxRayDistance, y energyPreservation, z volumeFade, w pad
			glm::ivec4 scroll{ 0 };
			glm::uvec4 counts{ 0 };
		};
		static_assert(sizeof(DdgiUBOData) == 112);
		static_assert(offsetof(DdgiUBOData, gridOrigin)  ==  0);
		static_assert(offsetof(DdgiUBOData, gridSpacing) == 16);
		static_assert(offsetof(DdgiUBOData, rayRotation) == 32);
		static_assert(offsetof(DdgiUBOData, params0)     == 48);
		static_assert(offsetof(DdgiUBOData, params1)     == 64);
		static_assert(offsetof(DdgiUBOData, scroll)      == 80);
		static_assert(offsetof(DdgiUBOData, counts)      == 96);

		// std140 - 32 bytes. Mirrored by SettingsUBO in res/shaders/GpuTypes.slang.
		struct SettingsUBOData {
			uint32_t raysPerPixel;
			uint32_t bouncesPerRay;
			uint32_t accumulatedFrames;
			uint32_t entityCount;
			uint32_t debugMode;
			uint32_t aabbHeatmapCutoff;
			uint32_t triangleHeatmapCutoff;
			uint32_t lightCount;
			uint32_t taaHistoryValid;
			uint32_t _pad0;
			uint32_t _pad1;
		};

		struct ParsedScene {
			std::vector<Gpu::MeshEntityHandle> MeshEntityLookupTable; // only renderable entities in the scene

			/// ONE DRAW PER SUBMESH, not per entity, and that is what keeps the
			/// rasterizer off SV_PrimitiveID.
			///
			/// Materials are per-submesh, so the fragment shader has to learn
			/// which one it is shading. Reading TriRefBuffer[root + SV_PrimitiveID]
			/// is the obvious way and it costs the SPIR-V `Geometry` capability --
			/// Vulkan gates gl_PrimitiveID in a fragment shader behind the
			/// geometryShader FEATURE, which MoltenVK does not have. Splitting the
			/// draw instead makes the material a push constant and needs nothing.
			///
			/// SubmeshInfo has existed since Phase 2 with a comment saying it was
			/// for exactly this.
			struct DrawRange {
				uint32_t entityIndex;    ///< index into MeshEntityLookupTable
				uint32_t firstTriIdx;    ///< GLOBAL, into TriRefBuffer
				uint32_t triCount;
				uint32_t materialSlot;   ///< mesh-local; the shader adds materialBase
				float    sortDepth;      ///< world-space distance from the camera
			};

			/// Opaque draws. These go through the depth prepass and shade with an
			/// EQUAL depth test.
			std::vector<DrawRange> DrawList;

			/// Draws whose material alpha is below 1, SORTED BACK TO FRONT.
			///
			/// SEPARATE FROM DrawList BECAUSE OF THE PREPASS. A transparent
			/// surface must not write depth there -- if it did, everything behind
			/// it would fail the opaque pass's EQUAL test and simply vanish, which
			/// looks like the transparent object being drawn as an opaque hole.
			///
			/// Sorted per frame by distance from the camera, because alpha
			/// blending is not commutative and the rasterizer has no other way to
			/// get the order right. The reference does not need this at all: alpha
			/// as coverage is order-independent, which is what makes it able to
			/// say whether this sort is correct.
			std::vector<DrawRange> TransparentDrawList;

			// TriPositionBuffer, TriRefBuffer, VertexBuffer, NodeBuffer and
			// BvhPrimIndexBuffer are stored in the AssetPool.
			//
			// The material list is FLATTENED AND VARIABLE-STRIDE: entity i's
			// materials occupy [materialBase, materialBase + materialSlotCount),
			// so it is no longer one entry per entity.
			//
			// AUTHORING form, not runtime form. Parse() is const and runs before
			// any frame exists, but resolving a texture GUID to a table index may
			// have to UPLOAD that texture, which needs a FrameContext. So Parse
			// collects MaterialDescs and SetupGPUResources converts them through
			// TextureTable::resolve().
			std::vector<MaterialDesc> MaterialDescs;
			std::vector<glm::mat4> TransformBuffer;
			std::vector<Gpu::LightData> LightBuffer;

			bool hasValidCamera = false;
			float CameraFocalLength = 0;
			glm::mat4 CameraTransform{};

			LR_GUID skyboxGUID = LR_GUID::INVALID;
		};

	public:
		Renderer(std::shared_ptr<Profiler> profiler)
			: m_Profiler(profiler) {
		};
		~Renderer() = default;

		/// Applies new settings, and RESETS ACCUMULATION when they change
		/// anything the accumulated samples depend on.
		///
		/// Half-integrated samples taken at a different sample count, resolution
		/// or shader are not samples of the same image, and blending new ones
		/// into them produces a picture of neither. The editor hits this whenever
		/// a slider moves; the render-test harness hit it between scenarios.
		inline void applySettings(RenderSettings renderSettings) {
			const RenderSettings& a = m_RenderSettings;
			const RenderSettings& b = renderSettings;
			const bool invalidated = a.resolution    != b.resolution
			                      || a.taaEnabled    != b.taaEnabled
			                      || a.bloomEnabled  != b.bloomEnabled
			                      || a.raysPerPixel  != b.raysPerPixel
			                      || a.bouncesPerRay != b.bouncesPerRay
			                      || a.shaderType    != b.shaderType
			                      || a.debugMode     != b.debugMode
			                      || a.accumulate    != b.accumulate;
			m_RenderSettings = renderSettings;
			if (invalidated) {
				m_Cache.AccumulatedFrames = 0;
				// The history describes an image made with the OLD settings.
				m_TaaHistoryValid = false;
				m_JitterIndex = 0;
				m_DdgiFrameIndex = 0;
				m_DdgiNeedsClear = true;
			}
		}
		inline void ResetAccumulation() { m_Cache.AccumulatedFrames = 0; }

		// Creates the compute pipelines and their descriptor set rings. Runs out of
		// frame, from RenderLayer::onAttach.
		void Init();

		// Destroys everything this class owns. MUST run after vkDeviceWaitIdle,
		// which is what RenderLayer::onDetach guarantees -- pipelines and descriptor
		// set layouts are destroyed INLINE rather than deferred (see
		// VulkanComputePipeline.h), so a pending command buffer that bound one is
		// undefined behaviour rather than a caught error.
		void Shutdown();

		// Returns a NON-OWNING pointer to the image this frame was rendered into,
		// or nullptr if there is no camera. The Renderer owns the images for its
		// whole life and recreate() keeps the object address, so the pointer stays
		// valid across frames -- but its CONTENTS belong to the frame it was
		// returned for. Compare generation() before reusing anything derived from
		// it, which is what the editor's ImGui descriptor cache does.
		VulkanImage* Render(const FrameContext& frame,
			const Scene* scene, const AssetPool* resourcePool,
			const glm::mat4* editorCameraTransform = nullptr, float editorCameraFOV = 90.0f);

	private:
		std::shared_ptr<const ParsedScene> Parse(const Scene* scene, const AssetPool* resourcePool,
			const glm::mat4* editorCameraTransform = nullptr, float editorCameraFOV = 90.0f) const;
		bool SetupGPUResources(const FrameContext& frame, std::shared_ptr<const ParsedScene> pScene,
			const Scene* scene, const AssetPool* resourcePool);
		// Takes the parsed scene, not just a count: the depth prepass and every
		// raster pass after it need the per-entity triangle ranges to issue draws.
		void Draw(const FrameContext& frame, std::shared_ptr<const ParsedScene> pScene);

		// Creates the pipeline and its kSetCount descriptor set rings together, and
		// they are destroyed together in Shutdown(). That pairing is load-bearing:
		// every ring holds a raw pointer into its pipeline's layout vector.
		VulkanComputePipeline* GetOrLoadShader(ShaderType type);

		// Index of the image slot this frame writes. Accumulation pins it to 0 --
		// see the comment on the definition.
		uint32_t writeSlot(const FrameContext& frame) const;

		std::shared_ptr<Profiler> m_Profiler;
		VulkanContext* m_Ctx = nullptr;

		std::unordered_map<ShaderType, VulkanComputePipeline> m_Pipelines;
		std::unordered_map<ShaderType, std::array<VulkanDescriptorSetRing, kSetCount>> m_SetRings;
		ShaderType m_CurrentShaderType = ShaderType::PATH_TRACING;

		// One image per frame slot. The resolution is tracked PER SLOT, not once:
		// a slot may only be recreated on the frame that owns it, because the other
		// slot may still be referenced by a command buffer in flight.
		std::array<VulkanImage, FRAMES_IN_FLIGHT> m_Frames;
		std::array<glm::uvec2, FRAMES_IN_FLIGHT>  m_FrameResolutions{};

		VulkanTexture m_SkyboxTexture;

		// THE BSDF ENERGY LUT. Baked once, on the first frame that has a
		// FrameContext, then read by every shading pass for the rest of the
		// process. Content depends only on the BSDF code, so nothing invalidates
		// it at runtime -- a shader edit rebuilds the binary and restarts the
		// engine, which re-bakes.
		//
		// 32x32 is plenty: the functions it tabulates are smooth in both
		// roughness and NdotV, and the bilinear fetch in Bsdf.slang covers the
		// rest. It is a storage image rather than a sampled texture so one
		// binding serves both the bake and the reads.
		static constexpr uint32_t kBsdfLutSize = 64;
		VulkanImage m_BsdfLut;
		bool        m_BsdfLutBaked = false;

		// The material texture array bound at set 0 binding 2, plus the
		// MaterialDesc -> Gpu::Material conversion that fills in its indices.
		TextureTable m_TextureTable;

		// Lives for the Renderer's whole life, not per frame: its transient POOL
		// must survive across frames, or every transient image would be
		// reallocated every frame. Only the DECLARATIONS are rebuilt each frame,
		// by begin().
		RenderGraph m_Graph;
		// This frame's handle for the render target, so the pass body can reach it
		// through RgResources rather than closing over the image directly -- which
		// is what makes a pass body unable to touch a resource it did not declare.
		RgHandle m_TargetHandle = RgHandle::Invalid;
		RgHandle m_BsdfLutHandle = RgHandle::Invalid;

		// Written in full every frame, so they are rings: one slot per frame,
		// written only for the slot whose fence beginFrame() has waited.
		VulkanRingBuffer m_CameraUBO, m_SettingsUBO;
		VulkanRingBuffer m_MeshEntityLookupSSBO, m_MaterialSSBO, m_TransformSSBO, m_LightSSBO;
		// The second-tier material blocks. A ring like the others because it is
		// rebuilt in full every frame alongside m_MaterialSSBO -- and usually
		// empty, since only materials with a coat, sheen or anisotropy get one.
		VulkanRingBuffer m_MaterialExtSSBO;

		// Written only when the asset pool's version changes, so they are plain
		// device-local buffers uploaded through the frame's staging arena. A ring
		// would be wrong here: a growing ring discards every slot, so a buffer
		// written only on change would read garbage on the frame after a growth.
		VulkanBuffer m_TriPositionSSBO, m_NodeBufferSSBO, m_BvhPrimIndexSSBO;
		VulkanBuffer m_TriRefSSBO, m_VertexSSBO;

		// Flat triangle list for the rasterizer: three global vertex indices per
		// triangle, derived from TriRefBuffer at upload time. It carries
		// VK_BUFFER_USAGE_INDEX_BUFFER_BIT as well as storage usage, because
		// vkCmdBindIndexBuffer needs the former and it is still declared in the
		// one descriptor table through the latter.
		//
		// The VERTEX SHADER DOES NOT INDEX IT. Bound as the index buffer, Vulkan's
		// SV_VertexID is already the value fetched from it -- indexing it again is
		// a vertex index used as an index index, which does not blank the frame,
		// it draws a scrambled sliver of the same mesh.
		VulkanBuffer m_MeshIndexSSBO;

		// ---- Clustered Forward+ light culling --------------------------------
		// Sized once from the Gpu::CLUSTER_* constants and never resized: the grid
		// is fixed, and the index list is worst-case allocated (884 KB) so no
		// compaction pass or atomic counter is needed. Device-local plain buffers
		// rather than per-frame rings because the GPU both writes and reads them
		// within a frame and the CPU never touches them.
		VulkanBuffer m_ClusterAABBSSBO, m_ClusterLightGridSSBO, m_ClusterLightIndexSSBO;

		// LAST FRAME'S transforms, for the velocity pass. A ring written in full
		// every frame, exactly like m_TransformSSBO -- the CPU already has the
		// data, so keeping a copy and re-uploading it is simpler and cheaper than
		// a GPU-side copy plus the barrier it would need.
		VulkanRingBuffer m_PrevTransformSSBO;
		std::vector<glm::mat4> m_PrevTransforms;

		// Bound at set 0 binding 0 by any pass that renders INTO the render target
		// instead of writing it as a storage image. See its creation site.
		VulkanImage m_DummyStorageImage;

		// Bloom ping-pong, both HALF the render resolution. RGBA16F rather than
		// 32: bloom is a low-frequency additive term and half floats carry it to
		// well past any visible precision, at half the bandwidth of the four
		// passes that touch them.
		/// ONE SET OF RINGS PER BLOOM PASS, not one shared by all four.
		///
		/// A ring hands out one set per frame slot, so four passes sharing a ring
		/// all get the SAME set and each rewrites it while the previous pass's
		/// dispatch is still recorded against it. That is
		/// VUID-vkCmdPushConstants-commandBuffer-recording, reported as "the
		/// descriptor set was destroyed or updated without UPDATE_AFTER_BIND" --
		/// exactly the hazard the per-frame ring exists to prevent, reintroduced
		/// by reusing one ring across passes rather than across frames.
		std::array<std::array<VulkanDescriptorSetRing, kSetCount>, 4> m_BloomRings;
		VulkanImage m_BloomA, m_BloomB;

		// ---- Phase 11: TAA ---------------------------------------------------
		// The resolved history. RGBA16F, and it holds DISPLAY-REFERRED colour --
		// TAA runs after the tonemap here, because neighbourhood clamping works
		// on perceptual differences and an HDR clamp lets one very bright sample
		// widen the box until it stops rejecting anything.
		std::array<VulkanImage, 2> m_TaaHistory;
		glm::uvec2  m_TaaResolution{ 0 };
		std::array<RgHandle, 2> m_TaaHistoryHandle{ RgHandle::Invalid, RgHandle::Invalid };
		uint32_t    m_TaaWriteSlot = 0;
		/// One set per PASS, for the reason the bloom rings are: two passes
		/// sharing a ring rewrite the same descriptor set while the first
		/// dispatch is still recorded against it.
		std::array<std::array<VulkanDescriptorSetRing, kSetCount>, 2> m_TaaRings;
		bool        m_TaaHistoryValid = false;
		/// Counts frames for the jitter sequence. Reset with accumulation so a
		/// render-test scenario always starts at sample 0 and is reproducible.
		uint32_t    m_JitterIndex = 0;

		// ---- Phase 10: DDGI --------------------------------------------------
		// Probe irradiance and probe depth, both as atlases of per-probe tiles
		// with a one-texel border. See res/shaders/Ddgi.slang.
		VulkanImage m_DdgiIrradiance, m_DdgiDepth;
		RgHandle    m_DdgiIrradianceHandle = RgHandle::Invalid;
		RgHandle    m_DdgiDepthHandle      = RgHandle::Invalid;
		VulkanBuffer m_DdgiRaySSBO;
		VulkanRingBuffer m_DdgiUBO;
		std::array<std::array<VulkanDescriptorSetRing, kSetCount>, 2> m_DdgiRings;
		uint32_t    m_DdgiFrameIndex = 0;
		/// Re-clear the atlases and restart the ray rotation sequence. Set on a
		/// scene change and on any settings change, for the same reason
		/// accumulation resets: probe irradiance integrated over a DIFFERENT
		/// scene is not a starting point, it is contamination. Without it the
		/// render-test suite was order-dependent -- ddgi-lights gave one image
		/// alone and another after other scenarios had run, because the ray
		/// rotation is seeded from a frame counter that never restarted.
		bool        m_DdgiNeedsClear = true;
		// Mirrors the X3_DDGI_* constants in res/shaders/Ddgi.slang.
		static constexpr uint32_t kDdgiProbeX = 8, kDdgiProbeY = 4, kDdgiProbeZ = 8;
		static constexpr uint32_t kDdgiProbeCount = kDdgiProbeX * kDdgiProbeY * kDdgiProbeZ;
		static constexpr uint32_t kDdgiRaysPerProbe = 64;
		static constexpr uint32_t kDdgiIrradianceTile = 8, kDdgiDepthTile = 16;
		static constexpr uint32_t kDdgiIrradianceAtlasW = kDdgiProbeX * kDdgiProbeZ * kDdgiIrradianceTile;
		static constexpr uint32_t kDdgiIrradianceAtlasH = kDdgiProbeY * kDdgiIrradianceTile;
		static constexpr uint32_t kDdgiDepthAtlasW = kDdgiProbeX * kDdgiProbeZ * kDdgiDepthTile;
		static constexpr uint32_t kDdgiDepthAtlasH = kDdgiProbeY * kDdgiDepthTile;
		glm::vec2   m_PrevJitter{ 0.0f };
		glm::uvec2  m_BloomResolution{ 0 };
		RgHandle    m_BloomAHandle = RgHandle::Invalid;
		RgHandle    m_BloomBHandle = RgHandle::Invalid;
		static constexpr VkFormat kBloomFormat = VK_FORMAT_R16G16B16A16_SFLOAT;

		// ---- Phase 7: the rasterizer -----------------------------------------
		// The engine had no graphics pipeline of any kind before this.
		static constexpr VkFormat kDepthFormat = VK_FORMAT_D32_SFLOAT;
		VulkanGraphicsPipeline m_DepthPrepassPipeline;
		std::array<VulkanDescriptorSetRing, kSetCount> m_DepthPrepassRings;
		VulkanGraphicsPipeline m_ForwardOpaquePipeline;
		std::array<VulkanDescriptorSetRing, kSetCount> m_ForwardOpaqueRings;
		// ---- Phase 8: cascaded shadow maps -----------------------------------
		// ONE WIDE 2D ATLAS, cascades side by side. A texture array is the
		// textbook form and is not available: VulkanImage hardcodes
		// arrayLayers = 1, ImageDesc has no field for it, RenderGraph hardcodes
		// layerCount = 1, and the graph tracks one layout per whole image so a
		// per-layer read/write hazard cannot be expressed. An atlas needs none of
		// that -- each cascade is a scissored sub-rect.
		VulkanGraphicsPipeline m_ShadowDepthPipeline;
		std::array<VulkanDescriptorSetRing, kSetCount> m_ShadowDepthRings;
		VulkanImage m_ShadowAtlas;
		RgHandle    m_ShadowAtlasHandle = RgHandle::Invalid;
		VulkanRingBuffer m_ShadowUBO;
		/// How far cascades reach. MUCH closer than the camera's 1000-unit far
		/// plane: four cascades stretched across that would put the near
		/// cascade's texels metres apart and blur every contact shadow. Geometry
		/// beyond this is unshadowed by the map, which is why the traced path
		/// stays the reference rather than being replaced.
		static constexpr float kShadowFarPlane = 60.0f;
		/// Normal-offset bias in TEXELS, multiplied by each cascade's world texel
		/// size in the shader. Unitless here on purpose -- a world-space constant
		/// cannot serve four cascades whose texels differ in size by two orders
		/// of magnitude.
		static constexpr float kShadowNormalOffsetTexels = 1.5f;
		/// Whether this frame found a directional light to build cascades for.
		bool m_ShadowCastersPresent = false;

		/// Renders every opaque caster into one cascade's sub-rect of the atlas.
		void DrawShadowCascade(const FrameContext& frame,
		                       std::span<const VkDescriptorSet> sets,
		                       const ParsedScene& pScene, uint32_t cascade);

		VulkanGraphicsPipeline m_ForwardTransparentPipeline;
		std::array<VulkanDescriptorSetRing, kSetCount> m_ForwardTransparentRings;
		VulkanGraphicsPipeline m_VelocityPipeline;
		std::array<VulkanDescriptorSetRing, kSetCount> m_VelocityRings;
		// RG16F: two signed components, and 16-bit float carries a UV-space
		// motion vector to well under a pixel at any resolution this renders at.
		static constexpr VkFormat kVelocityFormat = VK_FORMAT_R16G16_SFLOAT;
		VulkanImage m_VelocityImage;
		glm::uvec2  m_VelocityResolution{ 0 };
		RgHandle    m_VelocityHandle = RgHandle::Invalid;
		// The render target's format, repeated here because the graphics pipeline
		// must declare its colour attachment formats at creation and the target is
		// not allocated until the first frame.
		static constexpr VkFormat kColorFormat = VK_FORMAT_R32G32B32A32_SFLOAT;
		RgHandle m_DepthHandle = RgHandle::Invalid;
		VulkanImage m_DepthImage;
		glm::uvec2  m_DepthResolution{ 0 };

		/// Creates the raster pipelines. Separate from GetOrLoadShader because
		/// those are keyed by ShaderType, which is a user-facing choice of
		/// renderer; these are not selectable.
		bool EnsureRasterPipelines();

		/// One draw per entity into the currently-open rendering block.
		void DrawGeometry(const FrameContext& frame, const VulkanGraphicsPipeline& pipeline,
		                  std::span<const VkDescriptorSet> sets,
		                  std::span<const ParsedScene::DrawRange> draws, VkExtent2D extent);

		/// Writes all three descriptor sets and returns them ready to bind.
		///
		/// ONE TABLE SERVES EVERY PIPELINE in this engine, and DescriptorWriter::
		/// flush() asserts every binding was written exactly once -- so every pass,
		/// compute or raster, writes the identical set of bindings whether or not
		/// its shader reads them. That made this same block appear verbatim in
		/// every pass body, and each new binding had to be added to all of them:
		/// binding 9 was once added to two of three, and the miss surfaced as
		/// flush()'s completeness assert rather than as anything naming the pass.
		///
		/// Passes still have to DECLARE what they bind to the render graph --
		/// see the .write(target)/.read(lut) calls at each addPass -- because the
		/// graph derives barriers from declarations, not from descriptor writes.
		/// This function only removes the copy, not that obligation.
		///
		/// `targetIsAttachment` swaps the dummy storage image in at set 0 binding
		/// 0. Pass true from any raster pass that RENDERS INTO the target: it is
		/// then in COLOR_ATTACHMENT_OPTIMAL and binding it as a GENERAL storage
		/// image in the same pass is not a thing that can be done.
		/// `velocityIsAttachment` does the same for set 0 binding 5, substituting
		/// the context's 1x1 dummy texture. The velocity pass is the one that
		/// renders into it.
		std::array<VkDescriptorSet, kSetCount> WriteCommonSets(
			const FrameContext& frame, const RgResources& res,
			std::array<VulkanDescriptorSetRing, kSetCount>& rings,
			bool targetIsAttachment = false,
			bool velocityIsAttachment = false,
			bool shadowIsAttachment = false);

		Cache m_Cache;
		/// Identity only, for detecting a scene change. Never dereferenced.
		const Scene* m_LastScene = nullptr;
		RenderSettings m_RenderSettings;
		std::unordered_map<ShaderType, std::filesystem::path> m_ShaderPaths = {
			{ShaderType::PATH_TRACING, EngineCfg::RESOURCES_PATH / "shaders" / "PathTracing.slang"},
			{ShaderType::PHONG, EngineCfg::RESOURCES_PATH / "shaders" / "Phong.slang"},
			{ShaderType::PBR, EngineCfg::RESOURCES_PATH / "shaders" / "PBR.slang"},
			{ShaderType::FURNACE_TEST, EngineCfg::RESOURCES_PATH / "shaders" / "FurnaceTest.slang"},
			{ShaderType::BSDF_LUT_BAKE, EngineCfg::RESOURCES_PATH / "shaders" / "BsdfLutBake.slang"},
			{ShaderType::CLUSTER_BUILD, EngineCfg::RESOURCES_PATH / "shaders" / "ClusterBuild.slang"},
			{ShaderType::LIGHT_CULL, EngineCfg::RESOURCES_PATH / "shaders" / "LightCull.slang"},
			{ShaderType::SKYBOX_FILL, EngineCfg::RESOURCES_PATH / "shaders" / "SkyboxFill.slang"},
			{ShaderType::TONEMAP, EngineCfg::RESOURCES_PATH / "shaders" / "Tonemap.slang"},
			{ShaderType::BLOOM, EngineCfg::RESOURCES_PATH / "shaders" / "Bloom.slang"},
			{ShaderType::TAA, EngineCfg::RESOURCES_PATH / "shaders" / "Taa.slang"},
			{ShaderType::DDGI_TRACE, EngineCfg::RESOURCES_PATH / "shaders" / "DdgiProbeTrace.slang"},
			{ShaderType::DDGI_BLEND, EngineCfg::RESOURCES_PATH / "shaders" / "DdgiProbeBlend.slang"}
		};
	};
}
