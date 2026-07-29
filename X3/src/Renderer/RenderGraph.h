#pragma once

// =============================================================================
// RenderGraph.h -- passes declare what they read and write; barriers, transient
// lifetimes and resource reuse are derived from those declarations.
//
// WHY THIS EXISTS BEFORE THE RASTERIZER. Phase 7's Forward+ is a depth prepass,
// cluster assignment, light culling, opaque, transparent and velocity; Phases 8,
// 10 and 11 add shadow cascades, DDGI probe update, GI apply, TAA, bloom and
// tonemap. That is fifteen-plus passes, well past the point where hand-written
// barriers stay tractable, and barrier bugs are the single most common source of
// Vulkan failures that work on one GPU and corrupt on another. Building the
// graph after the passes exist would mean rewriting all of them.
//
// WHAT V1 DELIBERATELY DOES NOT DO. No pass reordering. No async compute. No
// multi-queue scheduling. All three are real wins and all three are where render
// graph complexity explodes; they are added once the pass set is stable, not
// while it is being discovered. Execution order is exactly the order passes were
// added.
//
// WHAT "ALIASING" MEANS HERE, STATED PLAINLY. Transient images are pooled and
// REUSED by (extent, format, usage) once their last reader has run -- the graph
// computes each resource's first and last pass and hands a retired allocation to
// the next compatible request. It is not memory aliasing in the
// bind-two-images-to-one-VkDeviceMemory sense; that needs VMA aliasing support,
// per-resource memory requirements and an allocator that understands overlap,
// and it buys nothing until there are enough large transients for it to matter.
// The declaration surface is the same either way, so upgrading later does not
// touch a single pass.
//
// HOW IT COMPOSES WITH THE PHASE 1 RESOURCE LAYER. It does not reimplement
// barriers. VulkanImage already tracks its own (layout, access, stage) and
// derives a correct barrier in transition()/barrier(); the graph's job is to
// decide WHEN and TO WHAT, from the declarations, and then call those. That
// keeps one implementation of the elision rule rather than two that must agree.
// =============================================================================

#include "Platform/Vulkan/VulkanBuffer.h"
#include "Platform/Vulkan/VulkanImage.h"
#include "Platform/Vulkan/VulkanTypes.h"

#include <cstdint>
#include <functional>
#include <string>
#include <vector>

namespace X3
{

class VulkanContext;

// Opaque handle into a graph's resource table. Only meaningful to the graph that
// produced it, and only until that graph is reset.
enum class RgHandle : uint32_t { Invalid = 0xFFFFFFFFu };

/// What a pass does with a resource. This is the whole vocabulary -- the graph
/// derives access masks, pipeline stages and image layouts from it, so a pass
/// never spells out a VkAccessFlags and cannot get one subtly wrong.
enum class RgUsage {
	ComputeRead,        ///< sampled or read-only storage in a compute shader
	ComputeWrite,       ///< storage image / buffer written by a compute shader
	ComputeReadWrite,   ///< read-modify-write in one dispatch (the accumulator)
	TransferRead,
	TransferWrite,
	FragmentRead,       ///< sampled by a fragment shader (the editor's ImGui pass)
	ColorAttachment,    ///< written by the rasterizer as a colour target
	DepthAttachment,    ///< written by the rasterizer as a depth target
	DepthRead,          ///< sampled/tested against without writing
};

/// How a raster pass treats an attachment it opens.
enum class RgLoadOp {
	Clear,     ///< discard whatever was there and clear to the declared value
	Load,      ///< keep it -- a later pass drawing into the same target
	DontCare,  ///< the pass writes every pixel; the driver may skip the load
};

/// Everything a pass body needs to reach its declared resources.
class RgResources
{
public:
	explicit RgResources(class RenderGraph& graph) : m_Graph(&graph) {}

	/// The image behind a handle. Asserts the handle names an image and that the
	/// currently-executing pass declared it -- reaching a resource a pass did not
	/// declare is exactly the bug this whole design exists to prevent, so it is
	/// caught rather than silently working until a barrier is missing.
	VulkanImage& image(RgHandle handle) const;

	/// As image(), for buffers.
	VulkanBuffer& buffer(RgHandle handle) const;

private:
	class RenderGraph* m_Graph = nullptr;
};

using RgPassBody = std::function<void(const FrameContext&, const RgResources&)>;

// -----------------------------------------------------------------------------
// The graph itself.
//
// LIFETIME. One RenderGraph lives for the life of the Renderer, so its transient
// POOL survives across frames -- rebuilding it every frame would reallocate every
// transient image every frame, which is worse than the hand-written code it
// replaces. The DECLARATIONS are rebuilt each frame (begin() clears them); the
// allocations are not.
//
// USAGE, per frame:
//     graph.begin();
//     RgHandle target = graph.importImage("target", &m_Frames[slot]);
//     graph.addPass("PathTrace")
//          .readWrite(target, RgUsage::ComputeReadWrite)
//          .execute([&](const FrameContext& f, const RgResources& r) { ... });
//     graph.compile();
//     graph.execute(frame);
// -----------------------------------------------------------------------------
class RenderGraph
{
public:
	RenderGraph() = default;
	explicit RenderGraph(VulkanContext& ctx) : m_Ctx(&ctx) {}

	/// Fluent declaration surface for one pass. Returned by addPass and valid
	/// only until the next addPass -- it holds an index, not a reference, so a
	/// stored one is caught by an assert rather than dangling.
	class PassBuilder
	{
	public:
		PassBuilder(RenderGraph& graph, uint32_t passIndex)
			: m_Graph(&graph), m_Pass(passIndex) {}

		PassBuilder& read(RgHandle handle, RgUsage usage);
		PassBuilder& write(RgHandle handle, RgUsage usage);
		/// Read-modify-write in a single pass. Distinct from read() + write()
		/// because it needs a barrier BEFORE the pass even when the layout is
		/// unchanged -- which is precisely the accumulation case that
		/// VulkanImage::transition() would elide.
		PassBuilder& readWrite(RgHandle handle, RgUsage usage);

		/// Declares a colour attachment. Implies write(handle,
		/// RgUsage::ColorAttachment) -- a pass that renders into a target is
		/// writing it, and stating that twice invites the two to disagree.
		///
		/// A pass with ANY attachment is a RASTER pass: execute() opens a dynamic
		/// rendering block around its body and closes it afterwards. A pass with
		/// none is a compute pass and gets no block, which matters because
		/// VulkanComputePipeline::dispatch asserts it is not inside one.
		PassBuilder& colorAttachment(RgHandle handle, RgLoadOp load = RgLoadOp::Clear,
		                             glm::vec4 clearValue = glm::vec4(0.0f));

		/// Declares the depth attachment. Implies write(handle,
		/// RgUsage::DepthAttachment).
		///
		/// clearValue defaults to 0, NOT 1, because the projection is REVERSE-Z:
		/// near maps to 1 and far to 0, so the far plane -- what an empty pixel
		/// should read as -- is zero. Clearing to 1 with a GREATER depth test
		/// rejects every fragment and renders nothing at all.
		PassBuilder& depthAttachment(RgHandle handle, RgLoadOp load = RgLoadOp::Clear,
		                             float clearValue = 0.0f);

		/// The work. Called during execute(), after the pass's barriers have been
		/// recorded into frame.cmd() and, for a raster pass, after its rendering
		/// block has been opened.
		void execute(RgPassBody body);

	private:
		RenderGraph* m_Graph = nullptr;
		uint32_t     m_Pass  = 0;
	};

	/// Clears the frame's declarations. Does NOT free the transient pool.
	void begin();

	/// An image the graph does not own -- the Renderer's per-frame targets, the
	/// swapchain, anything with a lifetime longer than one frame. The graph
	/// records barriers on it but never allocates or frees it.
	RgHandle importImage(const char* name, VulkanImage* image);

	/// A buffer the graph does not own.
	RgHandle importBuffer(const char* name, VulkanBuffer* buffer);

	/// A transient image, valid from its first writer to its last reader. Backed
	/// by the pool: a retired allocation with a matching desc is reused, and a
	/// new one is created only when none is free.
	RgHandle createImage(const char* name, const ImageDesc& desc);

	PassBuilder addPass(const char* name);

	/// Computes per-resource first/last pass, assigns pooled allocations to
	/// transients, and validates the declarations. Must be called before
	/// execute(); calling execute() without it asserts.
	void compile(const FrameContext& frame);

	/// Records every pass in declaration order, inserting the derived barriers
	/// before each.
	void execute(const FrameContext& frame);

	/// Human-readable dump of passes, resources and lifetimes. YOU WILL NEED THIS
	/// CONSTANTLY -- a render graph whose derived barriers cannot be inspected is
	/// harder to debug than the hand-written barriers it replaced, not easier.
	std::string dump() const;

	/// Frees every pooled transient. Must run after vkDeviceWaitIdle.
	void shutdown();

private:
	friend class PassBuilder;
	friend class RgResources;

	enum class ResourceKind { Image, Buffer };

	struct Access {
		RgHandle handle = RgHandle::Invalid;
		RgUsage  usage  = RgUsage::ComputeRead;
		bool     isWrite = false;
		bool     isReadWrite = false;
	};

	struct Attachment {
		RgHandle  handle = RgHandle::Invalid;
		RgLoadOp  load   = RgLoadOp::Clear;
		glm::vec4 clearColor{ 0.0f };
		float     clearDepth = 0.0f;
		bool      isDepth = false;
	};

	struct Pass {
		std::string             name;
		std::vector<Access>     accesses;
		std::vector<Attachment> attachments;
		RgPassBody              body;

		/// A pass with attachments is a raster pass and gets a dynamic rendering
		/// block; one without is a compute pass and must not.
		bool isRaster() const { return !attachments.empty(); }
	};

	struct Resource {
		std::string   name;
		ResourceKind  kind = ResourceKind::Image;

		// Imported: borrowed, never allocated or freed here.
		VulkanImage*  importedImage  = nullptr;
		VulkanBuffer* importedBuffer = nullptr;

		// Transient: desc plus the pool slot compile() assigned.
		bool          transient = false;
		ImageDesc     desc{};
		uint32_t      poolSlot = UINT32_MAX;

		// Filled by compile().
		uint32_t firstPass = UINT32_MAX;
		uint32_t lastPass  = 0;
	};

	struct PooledImage {
		VulkanImage image;
		ImageDesc   desc{};
		/// Pass index after which this allocation is free again, within the
		/// current frame's compile. UINT32_MAX means "in use for the whole frame".
		uint32_t    freeAfterPass = 0;
		bool        claimedThisFrame = false;
	};

	static bool descsCompatible(const ImageDesc& a, const ImageDesc& b);

	/// True when the currently-executing pass declared `handle`. Always true
	/// outside execute(), where there is no pass to check against.
	bool passDeclared(RgHandle handle) const;

	VulkanImage&  resolveImage(RgHandle handle) const;
	VulkanBuffer& resolveBuffer(RgHandle handle) const;

	VulkanContext*           m_Ctx = nullptr;
	std::vector<Resource>    m_Resources;
	std::vector<Pass>        m_Passes;
	std::vector<PooledImage> m_Pool;

	bool     m_Compiled = false;
	uint32_t m_ExecutingPass = UINT32_MAX;   // for RgResources' declaration check
};

}
