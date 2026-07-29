#include "Renderer/RenderGraph.h"

#include "Platform/Vulkan/VulkanContext.h"

#include <algorithm>
#include <cassert>
#include <sstream>

namespace X3
{

	namespace {
		// THE WHOLE TRANSLATION FROM INTENT TO VULKAN, in one place.
		//
		// A pass says ComputeWrite; this decides that means SHADER_WRITE at
		// COMPUTE_SHADER in layout GENERAL. Having exactly one table is the point:
		// the pre-graph code spelled these out at each call site, and two call
		// sites that disagreed about a stage produced a hazard no validation layer
		// reports.
		struct UsageInfo {
			VkAccessFlags        access;
			VkPipelineStageFlags stage;
			VkImageLayout        layout;
		};

		UsageInfo infoFor(RgUsage usage) {
			switch (usage) {
			case RgUsage::ComputeRead:
				return { VK_ACCESS_SHADER_READ_BIT, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT,
				         VK_IMAGE_LAYOUT_GENERAL };
			case RgUsage::ComputeWrite:
				return { VK_ACCESS_SHADER_WRITE_BIT, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT,
				         VK_IMAGE_LAYOUT_GENERAL };
			case RgUsage::ComputeReadWrite:
				return { VK_ACCESS_SHADER_READ_BIT | VK_ACCESS_SHADER_WRITE_BIT,
				         VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, VK_IMAGE_LAYOUT_GENERAL };
			case RgUsage::TransferRead:
				return { VK_ACCESS_TRANSFER_READ_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT,
				         VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL };
			case RgUsage::TransferWrite:
				return { VK_ACCESS_TRANSFER_WRITE_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT,
				         VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL };
			case RgUsage::FragmentRead:
				return { VK_ACCESS_SHADER_READ_BIT, VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT,
				         VK_IMAGE_LAYOUT_GENERAL };
			case RgUsage::ColorAttachment:
				return { VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT | VK_ACCESS_COLOR_ATTACHMENT_READ_BIT,
				         VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT,
				         VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL };
			case RgUsage::DepthAttachment:
				// BOTH depth stages. A depth attachment is tested at
				// EARLY_FRAGMENT_TESTS and written at LATE_FRAGMENT_TESTS, and a
				// barrier naming only one of them leaves the other unordered --
				// which is a hazard that shows up as flickering geometry on some
				// drivers and nothing at all on others.
				return { VK_ACCESS_DEPTH_STENCIL_ATTACHMENT_WRITE_BIT
				       | VK_ACCESS_DEPTH_STENCIL_ATTACHMENT_READ_BIT,
				         VK_PIPELINE_STAGE_EARLY_FRAGMENT_TESTS_BIT
				       | VK_PIPELINE_STAGE_LATE_FRAGMENT_TESTS_BIT,
				         VK_IMAGE_LAYOUT_DEPTH_ATTACHMENT_OPTIMAL };
			case RgUsage::DepthRead:
				return { VK_ACCESS_DEPTH_STENCIL_ATTACHMENT_READ_BIT,
				         VK_PIPELINE_STAGE_EARLY_FRAGMENT_TESTS_BIT
				       | VK_PIPELINE_STAGE_LATE_FRAGMENT_TESTS_BIT,
				         VK_IMAGE_LAYOUT_DEPTH_READ_ONLY_OPTIMAL };
			}
			assert(false && "unhandled RgUsage");
			return { 0, VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT, VK_IMAGE_LAYOUT_GENERAL };
		}

		const char* usageName(RgUsage usage) {
			switch (usage) {
			case RgUsage::ComputeRead:      return "ComputeRead";
			case RgUsage::ComputeWrite:     return "ComputeWrite";
			case RgUsage::ComputeReadWrite: return "ComputeReadWrite";
			case RgUsage::TransferRead:     return "TransferRead";
			case RgUsage::TransferWrite:    return "TransferWrite";
			case RgUsage::FragmentRead:     return "FragmentRead";
			case RgUsage::ColorAttachment:  return "ColorAttachment";
			case RgUsage::DepthAttachment:  return "DepthAttachment";
			case RgUsage::DepthRead:        return "DepthRead";
			}
			return "?";
		}

		inline uint32_t index(RgHandle h) { return static_cast<uint32_t>(h); }
	}

	// ---- RgResources ---------------------------------------------------------

	VulkanImage& RgResources::image(RgHandle handle) const {
		return m_Graph->resolveImage(handle);
	}

	VulkanBuffer& RgResources::buffer(RgHandle handle) const {
		return m_Graph->resolveBuffer(handle);
	}

	// ---- PassBuilder ---------------------------------------------------------

	RenderGraph::PassBuilder& RenderGraph::PassBuilder::read(RgHandle handle, RgUsage usage) {
		m_Graph->m_Passes[m_Pass].accesses.push_back({ handle, usage, false, false });
		return *this;
	}

	RenderGraph::PassBuilder& RenderGraph::PassBuilder::write(RgHandle handle, RgUsage usage) {
		m_Graph->m_Passes[m_Pass].accesses.push_back({ handle, usage, true, false });
		return *this;
	}

	RenderGraph::PassBuilder& RenderGraph::PassBuilder::readWrite(RgHandle handle, RgUsage usage) {
		m_Graph->m_Passes[m_Pass].accesses.push_back({ handle, usage, true, true });
		return *this;
	}

	RenderGraph::PassBuilder& RenderGraph::PassBuilder::colorAttachment(
		RgHandle handle, RgLoadOp load, glm::vec4 clearValue) {
		Attachment a;
		a.handle     = handle;
		a.load       = load;
		a.clearColor = clearValue;
		a.isDepth    = false;
		m_Graph->m_Passes[m_Pass].attachments.push_back(a);
		// The declaration and the attachment are the same fact stated once.
		return write(handle, RgUsage::ColorAttachment);
	}

	RenderGraph::PassBuilder& RenderGraph::PassBuilder::depthAttachment(
		RgHandle handle, RgLoadOp load, float clearValue) {
		Attachment a;
		a.handle     = handle;
		a.load       = load;
		a.clearDepth = clearValue;
		a.isDepth    = true;
		m_Graph->m_Passes[m_Pass].attachments.push_back(a);
		return write(handle, RgUsage::DepthAttachment);
	}

	void RenderGraph::PassBuilder::execute(RgPassBody body) {
		m_Graph->m_Passes[m_Pass].body = std::move(body);
	}

	// ---- RenderGraph ---------------------------------------------------------

	void RenderGraph::begin() {
		m_Resources.clear();
		m_Passes.clear();
		m_Compiled = false;
		m_ExecutingPass = UINT32_MAX;
		for (PooledImage& p : m_Pool)
			p.claimedThisFrame = false;
	}

	RgHandle RenderGraph::importImage(const char* name, VulkanImage* image) {
		assert(image && "importImage with a null image");
		Resource r;
		r.name = name;
		r.kind = ResourceKind::Image;
		r.importedImage = image;
		m_Resources.push_back(std::move(r));
		return static_cast<RgHandle>(m_Resources.size() - 1);
	}

	RgHandle RenderGraph::importBuffer(const char* name, VulkanBuffer* buffer) {
		assert(buffer && "importBuffer with a null buffer");
		Resource r;
		r.name = name;
		r.kind = ResourceKind::Buffer;
		r.importedBuffer = buffer;
		m_Resources.push_back(std::move(r));
		return static_cast<RgHandle>(m_Resources.size() - 1);
	}

	RgHandle RenderGraph::createImage(const char* name, const ImageDesc& desc) {
		Resource r;
		r.name = name;
		r.kind = ResourceKind::Image;
		r.transient = true;
		r.desc = desc;
		r.desc.debugName = name;
		m_Resources.push_back(std::move(r));
		return static_cast<RgHandle>(m_Resources.size() - 1);
	}

	RenderGraph::PassBuilder RenderGraph::addPass(const char* name) {
		Pass p;
		p.name = name;
		m_Passes.push_back(std::move(p));
		return PassBuilder(*this, static_cast<uint32_t>(m_Passes.size() - 1));
	}

	bool RenderGraph::descsCompatible(const ImageDesc& a, const ImageDesc& b) {
		// Usage must MATCH, not merely be a superset: a pooled image created
		// without TRANSFER_SRC cannot serve a request that needs it, and one
		// created with extra usage is a different allocation the driver may lay
		// out differently. Exact match keeps the pool predictable.
		return a.width == b.width && a.height == b.height && a.format == b.format
		    && a.usage == b.usage && a.mipLevels == b.mipLevels;
	}

	void RenderGraph::compile(const FrameContext& frame) {
		// --- lifetimes ---------------------------------------------------------
		for (uint32_t p = 0; p < m_Passes.size(); ++p) {
			for (const Access& a : m_Passes[p].accesses) {
				assert(index(a.handle) < m_Resources.size() && "pass declared an unknown handle");
				Resource& r = m_Resources[index(a.handle)];
				r.firstPass = std::min(r.firstPass, p);
				r.lastPass  = std::max(r.lastPass, p);
			}
		}

		// A transient nobody ever writes is a declaration bug: it would be read
		// before anything filled it. Caught here rather than showing up as noise
		// in the image.
		for (const Resource& r : m_Resources) {
			if (!r.transient) continue;
			assert(r.firstPass != UINT32_MAX &&
			       "transient resource declared but never used by any pass");
		}

		// --- transient assignment ---------------------------------------------
		// Walk resources in first-use order and hand each one a pooled allocation
		// whose previous tenant has already retired. This is where "aliasing"
		// happens, at allocation granularity -- see the header for exactly what
		// that does and does not mean.
		std::vector<uint32_t> order;
		for (uint32_t i = 0; i < m_Resources.size(); ++i)
			if (m_Resources[i].transient && m_Resources[i].firstPass != UINT32_MAX)
				order.push_back(i);
		std::sort(order.begin(), order.end(), [this](uint32_t a, uint32_t b) {
			return m_Resources[a].firstPass < m_Resources[b].firstPass;
		});

		for (uint32_t ri : order) {
			Resource& r = m_Resources[ri];

			uint32_t chosen = UINT32_MAX;
			for (uint32_t s = 0; s < m_Pool.size(); ++s) {
				PooledImage& slot = m_Pool[s];
				if (!descsCompatible(slot.desc, r.desc)) continue;
				// Free means: either untouched this frame, or its tenant's last
				// reader ran strictly before this resource's first writer.
				if (slot.claimedThisFrame && slot.freeAfterPass >= r.firstPass) continue;
				chosen = s;
				break;
			}

			if (chosen == UINT32_MAX) {
				PooledImage slot;
				slot.desc = r.desc;
				m_Pool.push_back(std::move(slot));
				chosen = static_cast<uint32_t>(m_Pool.size() - 1);
			}

			PooledImage& slot = m_Pool[chosen];
			if (!slot.image.valid())
				slot.image.recreate(frame, r.desc);

			slot.claimedThisFrame = true;
			slot.freeAfterPass = r.lastPass;
			r.poolSlot = chosen;
		}

		m_Compiled = true;
	}

	void RenderGraph::execute(const FrameContext& frame) {
		assert(m_Compiled && "RenderGraph::execute without compile()");

		for (uint32_t p = 0; p < m_Passes.size(); ++p) {
			Pass& pass = m_Passes[p];

			// BARRIERS FIRST, before the body runs -- ONE PER RESOURCE, not one
			// per declared access.
			//
			// THE COALESCING IS LOAD-BEARING, not an optimisation. A pass that
			// declares two readers of the same image (the editor samples it from a
			// fragment shader while the runtime blits it, and the renderer cannot
			// know which) must emit ONE barrier covering both. Two chained
			// barriers, where the second's source is the first's DESTINATION
			// access, order the stages but do NOT make the original compute write
			// visible to the second consumer. That is a real hazard that no
			// validation layer reports.
			//
			// The graph decides WHEN and TO WHAT; VulkanImage decides HOW, from
			// the (layout, access, stage) it already tracks. That split keeps one
			// implementation of the elision rule instead of two that must agree.
			struct Merged {
				RgHandle             handle = RgHandle::Invalid;
				VkAccessFlags        access = 0;
				VkPipelineStageFlags stage  = 0;
				VkImageLayout        layout = VK_IMAGE_LAYOUT_UNDEFINED;
				bool                 layoutAgreed = true;
				bool                 isReadWrite = false;
			};
			std::vector<Merged> merged;

			for (const Access& a : pass.accesses) {
				const Resource& r = m_Resources[index(a.handle)];
				if (r.kind != ResourceKind::Image) continue;   // buffers: see below

				const UsageInfo info = infoFor(a.usage);

				auto it = std::find_if(merged.begin(), merged.end(),
					[&](const Merged& m) { return m.handle == a.handle; });

				if (it == merged.end()) {
					merged.push_back({ a.handle, info.access, info.stage, info.layout,
					                   true, a.isReadWrite });
					continue;
				}

				it->access |= info.access;
				it->stage  |= info.stage;
				it->isReadWrite = it->isReadWrite || a.isReadWrite;
				// USAGES THAT DISAGREE ABOUT LAYOUT leave the resource in the
				// layout it already has, and the barrier becomes a pure
				// visibility barrier. That is deliberate and it is what the
				// hand-written code did: a compute write consumed by both an ImGui
				// sample (GENERAL) and a swapchain blit (TRANSFER_SRC_OPTIMAL)
				// cannot be in two layouts at once, and blitImageToSwapchain does
				// its own GENERAL -> TRANSFER_SRC -> GENERAL round trip through
				// transition(), so the tracked layout stays true either way.
				if (it->layout != info.layout)
					it->layoutAgreed = false;
			}

			for (const Merged& m : merged) {
				VulkanImage& img = resolveImage(m.handle);

				const bool sameLayout = !m.layoutAgreed || img.layout() == m.layout;
				if (sameLayout) {
					// barrier(), not transition(). transition() elides when the
					// layout is unchanged and the access is already covered, and
					// eliding here is exactly the hazard: the accumulator's read
					// must be ordered after the previous frame's write, and the
					// consumers' reads after this frame's. barrier() is the
					// explicit spelling of "I need this even though nothing looks
					// different".
					img.barrier(frame, m.access, m.stage);
				} else {
					img.transition(frame, m.layout, m.access, m.stage);
				}
			}

			// Buffers carry no layout, and every buffer this engine binds is
			// written through a per-frame ring or a staging upload that already
			// records its own barrier. Declaring them still buys the lifetime
			// tracking and the dump; a buffer barrier pass belongs here when a
			// compute pass first writes a buffer another pass reads, which no pass
			// does yet. Deliberately not speculated on.

			// ---- DYNAMIC RENDERING BLOCK, for raster passes only ----------
			// No VkRenderPass and no VkFramebuffer anywhere: attachments are named
			// here, at draw time, which is what locked decision 12 bought. A
			// COMPUTE pass gets no block, and that is load-bearing rather than an
			// optimisation -- VulkanComputePipeline::dispatch asserts it is not
			// inside one, because vkCmdDispatch within a rendering block is
			// VUID-vkCmdDispatch-renderpass.
			std::vector<VkRenderingAttachmentInfo> colorInfos;
			VkRenderingAttachmentInfo depthInfo{};
			bool haveDepth = false;
			VkExtent2D renderExtent{};

			if (pass.isRaster()) {
				for (const Attachment& a : pass.attachments) {
					VulkanImage& img = resolveImage(a.handle);
					renderExtent = img.extent();

					VkRenderingAttachmentInfo info{};
					info.sType       = VK_STRUCTURE_TYPE_RENDERING_ATTACHMENT_INFO;
					info.imageView   = img.view();
					info.imageLayout = a.isDepth ? VK_IMAGE_LAYOUT_DEPTH_ATTACHMENT_OPTIMAL
					                             : VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
					info.storeOp     = VK_ATTACHMENT_STORE_OP_STORE;
					switch (a.load) {
						case RgLoadOp::Clear:    info.loadOp = VK_ATTACHMENT_LOAD_OP_CLEAR;     break;
						case RgLoadOp::Load:     info.loadOp = VK_ATTACHMENT_LOAD_OP_LOAD;      break;
						case RgLoadOp::DontCare: info.loadOp = VK_ATTACHMENT_LOAD_OP_DONT_CARE; break;
					}

					if (a.isDepth) {
						info.clearValue.depthStencil = { a.clearDepth, 0 };
						depthInfo = info;
						haveDepth = true;
					} else {
						info.clearValue.color = { { a.clearColor.r, a.clearColor.g,
						                            a.clearColor.b, a.clearColor.a } };
						colorInfos.push_back(info);
					}
				}

				VkRenderingInfo ri{};
				ri.sType                = VK_STRUCTURE_TYPE_RENDERING_INFO;
				ri.renderArea.offset    = { 0, 0 };
				ri.renderArea.extent    = renderExtent;
				ri.layerCount           = 1;
				ri.colorAttachmentCount = static_cast<uint32_t>(colorInfos.size());
				ri.pColorAttachments    = colorInfos.empty() ? nullptr : colorInfos.data();
				ri.pDepthAttachment     = haveDepth ? &depthInfo : nullptr;
				vkCmdBeginRendering(frame.cmd(), &ri);
			}

			m_ExecutingPass = p;
			if (pass.body)
				pass.body(frame, RgResources(*this));
			m_ExecutingPass = UINT32_MAX;

			if (pass.isRaster())
				vkCmdEndRendering(frame.cmd());
		}
	}

	bool RenderGraph::passDeclared(RgHandle handle) const {
		// Outside execute() there is no pass to check against -- compile() and
		// the dump legitimately resolve resources with no pass executing.
		if (m_ExecutingPass == UINT32_MAX) return true;
		for (const Access& a : m_Passes[m_ExecutingPass].accesses)
			if (a.handle == handle) return true;
		return false;
	}

	VulkanImage& RenderGraph::resolveImage(RgHandle handle) const {
		assert(index(handle) < m_Resources.size() && "unknown RgHandle");
		const Resource& r = m_Resources[index(handle)];
		assert(r.kind == ResourceKind::Image && "handle does not name an image");
		// THE INVARIANT THIS WHOLE DESIGN EXISTS FOR. A pass that reaches a
		// resource it did not declare gets no barrier for it, and the failure is
		// a race that depends on GPU timing rather than anything reproducible.
		// This was documented from the start and NOT enforced, and the very first
		// pass added after the graph landed violated it.
		assert(passDeclared(handle) &&
		       "pass reached an image it did not declare -- add a read()/write() "
		       "for it, or the graph cannot barrier it");

		if (r.transient) {
			assert(r.poolSlot != UINT32_MAX && "transient image used before compile()");
			return const_cast<RenderGraph*>(this)->m_Pool[r.poolSlot].image;
		}
		assert(r.importedImage && "imported image handle with no image");
		return *r.importedImage;
	}

	VulkanBuffer& RenderGraph::resolveBuffer(RgHandle handle) const {
		assert(index(handle) < m_Resources.size() && "unknown RgHandle");
		const Resource& r = m_Resources[index(handle)];
		assert(r.kind == ResourceKind::Buffer && "handle does not name a buffer");
		assert(passDeclared(handle) &&
		       "pass reached a buffer it did not declare");
		assert(r.importedBuffer && "imported buffer handle with no buffer");
		return *r.importedBuffer;
	}

	std::string RenderGraph::dump() const {
		std::ostringstream out;
		out << "RenderGraph: " << m_Passes.size() << " passes, "
		    << m_Resources.size() << " resources, pool " << m_Pool.size() << " images\n";

		out << "  resources:\n";
		for (uint32_t i = 0; i < m_Resources.size(); ++i) {
			const Resource& r = m_Resources[i];
			out << "    [" << i << "] " << r.name
			    << (r.kind == ResourceKind::Image ? " image" : " buffer")
			    << (r.transient ? " transient" : " imported");
			if (r.firstPass == UINT32_MAX) {
				out << "  (unused)";
			} else {
				out << "  live [" << r.firstPass << ".." << r.lastPass << "]";
				if (r.transient) out << "  poolSlot=" << r.poolSlot;
			}
			out << "\n";
		}

		out << "  passes:\n";
		for (uint32_t p = 0; p < m_Passes.size(); ++p) {
			const Pass& pass = m_Passes[p];
			out << "    [" << p << "] " << pass.name
			    << (pass.isRaster() ? "  (raster)" : "  (compute)") << "\n";
			for (const Access& a : pass.accesses) {
				const Resource& r = m_Resources[index(a.handle)];
				out << "        "
				    << (a.isReadWrite ? "rw " : (a.isWrite ? "w  " : "r  "))
				    << r.name << "  " << usageName(a.usage) << "\n";
			}
		}
		return out.str();
	}

	void RenderGraph::shutdown() {
		// Runs after vkDeviceWaitIdle. The pooled images route their handles
		// through the context's deferred-destroy queue like every other
		// VulkanImage, so the order here does not matter -- but the wait does.
		m_Pool.clear();
		m_Resources.clear();
		m_Passes.clear();
		m_Compiled = false;
	}

}
