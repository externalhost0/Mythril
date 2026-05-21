//
// Created by Hayden Rivas on 10/11/25.
//

#include "mythril/CTX.h"
#include "CommandBuffer.h"
#include "mythril/vkenums.h"

#include "mythril/RenderGraphBuilder.h"

#include <array>
#include <cstdlib>
#include <fstream>
#include <queue>

#include "RenderGraphInternal.h"

#include "GraphicsPipelineBuilder.h"
#include "Logger.h"
#include "vkstring.h"

namespace mythril {
	RenderGraph::RenderGraph() = default;
	RenderGraph::~RenderGraph() = default;
	RenderGraph::RenderGraph(RenderGraph&&) noexcept = default;
	RenderGraph& RenderGraph::operator=(RenderGraph&&) noexcept = default;

	BasePassBuilder::BasePassBuilder(RenderGraph& rGraph, const char* pName, const PassDesc::Type type)
		: _rGraph(rGraph), _passSource(pName, type) {
		ASSERT(!_passSource.name.empty());
		// set default queue solely based on type, will change during compile
		switch (type) {
			case PassDesc::Type::Graphics:     _passSource.queue = QueueAffinity::Graphics;      break;
			case PassDesc::Type::Compute:      _passSource.queue = QueueAffinity::Graphics;      break;
			case PassDesc::Type::Intermediate: _passSource.queue = QueueAffinity::AsyncTransfer; break;
		}
	}

#ifdef DEBUG
	GraphicsPassBuilder::~GraphicsPassBuilder() {
		if (!this->base._passSource.executeCallback)
			LOG_SYSTEM_NOSOURCE(LogType::Warning, "GraphicsPass '{}' has no execution callback!", this->base._passSource.name);
	}
	ComputePassBuilder::~ComputePassBuilder() {
		if (!this->base._passSource.executeCallback)
			LOG_SYSTEM_NOSOURCE(LogType::Warning, "ComputePass '{}' has no execution callback!", this->base._passSource.name);
	}
#endif

	IntermediateBuilder& IntermediateBuilder::blit(TextureDesc src, TextureDesc dst) {
		TextureHandle srcHandle = src.texture.handle();
		TextureHandle dstHandle = dst.texture.handle();

		add(base._passSource, src, Layout::TRANSFER_SRC);
		add(base._passSource, dst, Layout::TRANSFER_DST);

		auto oldCallback = base._passSource.executeCallback;
		if (dst.texture->isSwapchainImage()) {
			base._passSource.executeCallback = [oldCallback, srcHandle](CommandBuffer& cmd) {
				if (oldCallback)
					oldCallback(cmd);
				cmd.cmdBlitImageToSwapchain(srcHandle);
			};
		} else {
			base._passSource.executeCallback = [oldCallback, srcHandle, dstHandle](CommandBuffer& cmd) {
				if (oldCallback)
					oldCallback(cmd);
				cmd.cmdBlitImage(srcHandle, dstHandle);
			};
		}
		return *this;
	}
	IntermediateBuilder& IntermediateBuilder::copy(TextureDesc src, TextureDesc dst) {
		TextureHandle srcHandle = src.texture.handle();
		TextureHandle dstHandle = dst.texture.handle();

		add(base._passSource, src, Layout::TRANSFER_SRC);
		add(base._passSource, dst, Layout::TRANSFER_DST);

		auto oldCallback = base._passSource.executeCallback;
		if (dst.texture->isSwapchainImage()) {
			base._passSource.executeCallback = [oldCallback, srcHandle](CommandBuffer& cmd) {
				if (oldCallback)
					oldCallback(cmd);
				cmd.cmdCopyImageToSwapchain(srcHandle);
			};
		} else {
			base._passSource.executeCallback = [oldCallback, srcHandle, dstHandle](CommandBuffer& cmd) {
				if (oldCallback)
					oldCallback(cmd);
				cmd.cmdCopyImage(srcHandle, dstHandle);
			};
		}
		return *this;
	}

	// todo: i have no clue how this just works immediately besides the fact we dont actually touch the base image + restore its original layout
	IntermediateBuilder& IntermediateBuilder::generateMipmaps(const Texture& texture) {
		TextureHandle handle = texture.handle();
		auto oldCallback = base._passSource.executeCallback;
		base._passSource.executeCallback = [oldCallback, handle](CommandBuffer& cmd) {
			if (oldCallback)
				oldCallback(cmd);
			cmd.cmdGenerateMipmap(handle);
		};
		return *this;
	}
	IntermediateBuilder& IntermediateBuilder::dependency(Buffer& buffer, BufferAccess access) {
		add(base._passSource, buffer, access);
		return *this;
	}
	IntermediateBuilder& IntermediateBuilder::update(Buffer& buffer, std::function<UploadData()> dataCb, size_t dstOffset) {
		BufferHandle handle = buffer.handle();
		add(base._passSource, buffer, BufferAccess::TransferWrite);

		auto oldCallback = base._passSource.executeCallback;
		base._passSource.executeCallback = [oldCallback, handle, dataCb = std::move(dataCb), dstOffset](CommandBuffer& cmd) {
			if (oldCallback)
				oldCallback(cmd);
			if (cmd.isDrying())
				return;

			const UploadData data = dataCb();
			ASSERT_MSG(data.data, "IntermediateBuilder::update called with null data.");
			ASSERT_MSG(data.size > 0 && data.size <= 65536, "IntermediateBuilder::update requires 0 < size <= 65'536 (64KB).");
			ASSERT_MSG(data.size % 4 == 0, "IntermediateBuilder::update size must be a multiple of 4 bytes.");
			ASSERT_MSG(dstOffset % 4 == 0, "IntermediateBuilder::update offset must be a multiple of 4 bytes.");

			AllocatedBuffer& buf = cmd._ctx->access(handle);
			ASSERT_MSG(dstOffset + data.size <= buf._bufferSize, "IntermediateBuilder::update: offset + size exceeds buffer '{}' size.", buf._debugName);
			ASSERT_MSG(buf._vkUsageFlags & VK_BUFFER_USAGE_TRANSFER_DST_BIT, "IntermediateBuilder::update: buffer '{}' was not created with TRANSFER_DST usage.", buf._debugName);

			vkCmdUpdateBuffer(cmd._wrapper->_cmdBuf, buf._vkBuffer, dstOffset, data.size, data.data);
		};
		return *this;
	}
	IntermediateBuilder& IntermediateBuilder::upload(Buffer& buffer, std::function<UploadData()> dataCb, size_t dstOffset) {
		BufferHandle handle = buffer.handle();
		add(base._passSource, buffer, BufferAccess::TransferWrite);

		auto oldCallback = base._passSource.executeCallback;
		base._passSource.executeCallback = [oldCallback, handle, dataCb = std::move(dataCb), dstOffset](CommandBuffer& cmd) {
			if (oldCallback)
				oldCallback(cmd);
			if (cmd.isDrying())
				return;

			const UploadData data = dataCb();
			ASSERT_MSG(data.data, "IntermediateBuilder::upload called with null data.");
			ASSERT_MSG(data.size > 0, "IntermediateBuilder::upload size must be greater than 0.");

			AllocatedBuffer& buf = cmd._ctx->access(handle);
			ASSERT_MSG(dstOffset + data.size <= buf._bufferSize, "IntermediateBuilder::upload: offset + size exceeds buffer '{}' size.", buf._debugName);
			ASSERT_MSG(
			        buf.isMapped() || (buf._vkUsageFlags & VK_BUFFER_USAGE_TRANSFER_DST_BIT), "IntermediateBuilder::upload: device buffer '{}' was not created with TRANSFER_DST usage.", buf._debugName
			);
			if (buf.isMapped()) {
				buf.bufferSubData(*cmd._ctx, dstOffset, data.size, data.data);
				return;
			}

			auto copies = cmd._ctx->_staging->stageBufferCopy(buf, data.data, data.size, dstOffset);
			cmd._ctx->_staging->recordBufferCopies(cmd._wrapper->_cmdBuf, copies);
		};
		return *this;
	}

	void IntermediateBuilder::finish() const {
		this->base._rGraph._passDescriptions.push_back(base._passSource);
		this->base._rGraph._hasCompiled = false;
	}

	void BasePassBuilder::setExecuteCallback(const std::function<void(CommandBuffer& cmd)>& callback) {
		_passSource.executeCallback = callback;
		_rGraph._passDescriptions.push_back(_passSource);
		_rGraph._hasCompiled = false;
	}

	static constexpr bool IsAttachmentImageLayout(VkImageLayout layout) {
		switch (layout) {
			case VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL:
			case VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL:
			case VK_IMAGE_LAYOUT_DEPTH_READ_ONLY_STENCIL_ATTACHMENT_OPTIMAL:
			case VK_IMAGE_LAYOUT_DEPTH_ATTACHMENT_STENCIL_READ_ONLY_OPTIMAL:
			case VK_IMAGE_LAYOUT_DEPTH_ATTACHMENT_OPTIMAL:
			case VK_IMAGE_LAYOUT_STENCIL_ATTACHMENT_OPTIMAL:
			case VK_IMAGE_LAYOUT_ATTACHMENT_OPTIMAL:
				return true;
			default:
				return false;
		}
	}
	static constexpr VkImageSubresourceRange MakeVkRange(VkFormat fmt, SubresourceRange range) {
		return {vkutil::AspectMaskFromFormat(fmt), range.baseMip, range.numMips, range.baseLayer, range.numLayers};
	}
	static SubresourceRange ResolveSubresourceRange(const TextureDesc& texDesc, const AllocatedTexture& texture, std::string_view passName) {
		const uint32_t baseMip = texDesc.baseLevel.value_or(0);
		const uint32_t numMips = texDesc.numLevels.value_or(texDesc.baseLevel.has_value() ? 1 : texture.getNumMips());
		const uint32_t baseLayer = texDesc.baseLayer.value_or(0);
		const uint32_t numLayers = texDesc.numLayers.value_or(texDesc.baseLayer.has_value() ? 1 : texture.getNumLayers());

		ASSERT_MSG(baseMip < texture.getNumMips(), "Pass '{}': Texture '{}' base mip {} is outside the texture's {} mip levels", passName, texture.getDebugName(), baseMip, texture.getNumMips());
		ASSERT_MSG(
		        numMips > 0 && baseMip + numMips <= texture.getNumMips(),
		        "Pass '{}': Texture '{}' mip range [{}, {}) is outside the texture's {} mip levels",
		        passName,
		        texture.getDebugName(),
		        baseMip,
		        baseMip + numMips,
		        texture.getNumMips()
		);
		ASSERT_MSG(baseLayer < texture.getNumLayers(), "Pass '{}': Texture '{}' base layer {} is outside the texture's {} layers", passName, texture.getDebugName(), baseLayer, texture.getNumLayers());
		ASSERT_MSG(
		        numLayers > 0 && baseLayer + numLayers <= texture.getNumLayers(),
		        "Pass '{}': Texture '{}' layer range [{}, {}) is outside the texture's {} layers",
		        passName,
		        texture.getDebugName(),
		        baseLayer,
		        baseLayer + numLayers,
		        texture.getNumLayers()
		);

		return {baseMip, numMips, baseLayer, numLayers};
	}

	static VkImageView ResolveImageView(const TextureDesc& texDesc, const AllocatedTexture& allocatedTexture, std::string_view passName) {
		const bool needsCustomView = texDesc.baseLayer.has_value() || texDesc.baseLevel.has_value() || texDesc.numLayers.has_value() || texDesc.numLevels.has_value();
		if (!needsCustomView) {
			return allocatedTexture.getImageView();
		}

		const SubresourceRange range = ResolveSubresourceRange(texDesc, allocatedTexture, passName);
		auto& mutableTexture = const_cast<Texture&>(texDesc.texture);
		VkImageViewType viewType = allocatedTexture.getViewType();
		if (range.numLayers == 1 && viewType == VK_IMAGE_VIEW_TYPE_CUBE) {
			viewType = VK_IMAGE_VIEW_TYPE_2D;
		}
		const Texture::ViewKey viewKey = mutableTexture.createView({
		    .type = viewType,
		    .mipLevel = range.baseMip,
		    .numMipLevels = range.numMips,
		    .layer = range.baseLayer,
		    .numLayers = range.numLayers,
		});
		return mutableTexture.getImageViewForKey(viewKey);
	}

	static constexpr bool NeedsBarrier(const SubresourceState& currentState, VkImageLayout desiredLayout, const vkutil::StageAccess& desiredStageAccess) {
		if (currentState.layout != desiredLayout)
			return true;

		constexpr VkAccessFlags2 write_flags2 =
		        VK_ACCESS_2_SHADER_WRITE_BIT | VK_ACCESS_2_MEMORY_WRITE_BIT | VK_ACCESS_2_COLOR_ATTACHMENT_WRITE_BIT | VK_ACCESS_2_DEPTH_STENCIL_ATTACHMENT_WRITE_BIT | VK_ACCESS_2_TRANSFER_WRITE_BIT;

		bool currentStateWrites = (currentState.mask.access & write_flags2) > 0;
		// if (currentStateWrites || IsAttachmentImageLayout(desiredLayout))
		// 	return true;
		if (currentStateWrites && desiredStageAccess.stage != currentState.mask.stage)
			return true;

		if (currentState.mask.stage != desiredStageAccess.stage) {
			return true;
		}
		return false;
	}
	static constexpr bool IsBufferWriteAccess(VkAccessFlags2 access) {
		constexpr VkAccessFlags2 writeFlags = VK_ACCESS_2_SHADER_STORAGE_WRITE_BIT | VK_ACCESS_2_TRANSFER_WRITE_BIT | VK_ACCESS_2_MEMORY_WRITE_BIT | VK_ACCESS_2_HOST_WRITE_BIT;
		return (access & writeFlags) != 0;
	}
	static constexpr bool NeedsBufferBarrier(const vkutil::StageAccess& currentState, const vkutil::StageAccess& desiredStageAccess) {
		if (currentState.stage == 0 && currentState.access == 0)
			return false;

		const bool currentWrites = IsBufferWriteAccess(currentState.access);
		const bool desiredWrites = IsBufferWriteAccess(desiredStageAccess.access);
		if (!currentWrites && !desiredWrites)
			return false;

		return true;
	}
	static vkutil::StageAccess GetBufferStageAccess(BufferAccess access, PassDesc::Type passType) {
		const auto shaderStages = [passType] {
			switch (passType) {
				case PassDesc::Type::Graphics:
					return VK_PIPELINE_STAGE_2_VERTEX_SHADER_BIT | VK_PIPELINE_STAGE_2_FRAGMENT_SHADER_BIT;
				case PassDesc::Type::Compute:
					return VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT;
				case PassDesc::Type::Intermediate:
					return VK_PIPELINE_STAGE_2_ALL_GRAPHICS_BIT | VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT;
			}
			return VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT;
		}();

		switch (access) {
			case BufferAccess::ShaderRead:
				return {.stage = shaderStages, .access = VK_ACCESS_2_SHADER_STORAGE_READ_BIT};
			case BufferAccess::ShaderWrite:
				return {.stage = shaderStages, .access = VK_ACCESS_2_SHADER_STORAGE_WRITE_BIT};
			case BufferAccess::ShaderReadWrite:
				return {.stage = shaderStages, .access = VK_ACCESS_2_SHADER_STORAGE_READ_BIT | VK_ACCESS_2_SHADER_STORAGE_WRITE_BIT};
			case BufferAccess::IndexRead:
				return {.stage = VK_PIPELINE_STAGE_2_VERTEX_INPUT_BIT, .access = VK_ACCESS_2_INDEX_READ_BIT};
			case BufferAccess::IndirectRead:
				return {.stage = VK_PIPELINE_STAGE_2_DRAW_INDIRECT_BIT, .access = VK_ACCESS_2_INDIRECT_COMMAND_READ_BIT};
			case BufferAccess::TransferRead:
				return {.stage = VK_PIPELINE_STAGE_2_TRANSFER_BIT, .access = VK_ACCESS_2_TRANSFER_READ_BIT};
			case BufferAccess::TransferWrite:
				return {.stage = VK_PIPELINE_STAGE_2_TRANSFER_BIT, .access = VK_ACCESS_2_TRANSFER_WRITE_BIT};
		}
		ASSERT_MSG(false, "Unsupported BufferAccess value.");
	}

	void RenderGraph::processResourceAccess(const TextureDesc& texDesc, VkImageLayout desiredLayout, CompiledPass& outPass, std::string_view passName) {
		const AllocatedTexture& texture = texDesc.texture.view();
		const bool isSwapchain = texture.isSwapchainImage();
		const SubresourceRange range = ResolveSubresourceRange(texDesc, texture, passName);
		const vkutil::StageAccess dstMask = vkutil::GetPipelineStageAccess(desiredLayout);
		outPass.imageBarriers.push_back({.handle = texDesc.texture.handle(), .range = range, .dstLayout = desiredLayout, .dstMask = dstMask, .isSwapchain = isSwapchain});
	}

	void RenderGraph::processPassResources(const PassDesc& passDesc, CompiledPass& outPass) {
		// we expect to use a max of this
		outPass.imageBarriers.reserve(passDesc.textureDependencyOperations.size() + passDesc.attachmentOperations.size());
		outPass.bufferBarriers.reserve(passDesc.bufferDependencyOperations.size());
		for (const TextureDependencyDesc& dependency_desc: passDesc.textureDependencyOperations) {
			const VkImageLayout layout = [](const Layout simpleLayout) {
				switch (simpleLayout) {
					case Layout::GENERAL:
						return VK_IMAGE_LAYOUT_GENERAL;
					case Layout::READ:
						return VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
					case Layout::TRANSFER_SRC:
						return VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL;
					case Layout::TRANSFER_DST:
						return VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
					case Layout::PRESENT:
						return VK_IMAGE_LAYOUT_PRESENT_SRC_KHR;
					default:
						assert(false);
				}
			}(dependency_desc.desiredLayout);
			processResourceAccess(dependency_desc.texDesc, layout, outPass, passDesc.name);
		}
		for (const AttachmentDesc& attachment_desc: passDesc.attachmentOperations) {
			const AllocatedTexture& texture = attachment_desc.texDesc.texture.view();
			const VkImageLayout layout = texture.isDepthAttachment() ? VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL : VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
			processResourceAccess(attachment_desc.texDesc, layout, outPass, passDesc.name);

			if (attachment_desc.resolveTexDesc.has_value()) {
				processResourceAccess(attachment_desc.resolveTexDesc.value(), layout, outPass, passDesc.name);
			}
		}
		for (const BufferDependencyDesc& dependency_desc: passDesc.bufferDependencyOperations) {
			outPass.bufferBarriers.push_back({.handle = dependency_desc.buffer.handle(), .dstMask = GetBufferStageAccess(dependency_desc.access, passDesc.type)});
		}
	}

	void RenderGraph::processAttachments(const CTX& rCtx, const PassDesc& pass_desc, CompiledPass& outPass) {
		if (pass_desc.attachmentOperations.empty())
			return;
		uint32_t max_width = 0, max_height = 0;
		bool hasDepthAttachment = false;
		for (const AttachmentDesc& attachment_desc: pass_desc.attachmentOperations) {
			const TextureDesc& texDesc = attachment_desc.texDesc;
			const AllocatedTexture& allocatedTexture = attachment_desc.texDesc.texture.view();

			const bool isDepthAttachment = vkutil::IsFormatDepth(allocatedTexture.getFormat());
			if (isDepthAttachment) {
				ASSERT_MSG(!hasDepthAttachment, "Pass '{}': Multiple depth attachments not allowed (found '{}' to be a second depth attachment)", outPass.name, allocatedTexture.getDebugName());
				hasDepthAttachment = true;
			}
			VkImageView imageView = ResolveImageView(texDesc, allocatedTexture, pass_desc.name);

			const ClearValue& clear_value = attachment_desc.clearValue;
			AttachmentInfo attachment_info = {
			    .imageFormat = allocatedTexture.getFormat(),
			    .imageView = imageView,
			    .imageLayout = isDepthAttachment ? VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL : VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL,
			    // these fields are "resolved" in the next steps, haha
			    .resolveImageView = VK_NULL_HANDLE,
			    .resolveImageLayout = VK_IMAGE_LAYOUT_UNDEFINED,

			    .loadOp = toVulkan(attachment_desc.loadOp),
			    .storeOp = toVulkan(attachment_desc.storeOp),
			    .clearValue = isDepthAttachment ? clear_value.getDepthStencilValue() : clear_value.getColorValue(),
			};
			if (attachment_desc.resolveTexDesc.has_value()) {
				const TextureDesc& resolve_desc = attachment_desc.resolveTexDesc.value();
				const AllocatedTexture resolve_texture = resolve_desc.texture.view();
				ASSERT_MSG(
				        resolve_texture.getSampleCount() == VK_SAMPLE_COUNT_1_BIT,
				        "Pass '{}': Resolve Texture must have a sample count of 1 (found '{}' to be of a greater sample count)",
				        pass_desc.name,
				        resolve_texture.getDebugName()
				);
				ASSERT_MSG(
				        allocatedTexture.getSampleCount() > VK_SAMPLE_COUNT_1_BIT, "Pass '{}': Resolve operation on non-multisampled texture '{}'!", pass_desc.name, allocatedTexture.getDebugName()
				);
				ASSERT_MSG(
				        resolve_texture.getFormat() == allocatedTexture.getFormat(),
				        "Pass '{}': Resolve texture must have the same format as the texture it resolves from (found texture '{}' to be of format {} while resolve texture '{}' is of format {})",
				        pass_desc.name,
				        allocatedTexture.getDebugName(),
				        vkstring::VulkanFormatToString(allocatedTexture.getFormat()),
				        resolve_texture.getDebugName(),
				        vkstring::VulkanFormatToString(resolve_texture.getFormat())
				);
				if (attachment_desc.storeOp == StoreOp::STORE)
					LOG_SYSTEM_NOSOURCE(
					        LogType::Suggestion,
					        "Pass '{}': Attachment of texture '{}' has StoreOp::STORE and has a resolve attachment, can be replaced with StoreOp::NO_CARE.",
					        pass_desc.name,
					        allocatedTexture.getDebugName(),
					        resolve_texture.getDebugName()
					);

				// set the fields we left empty previously
				attachment_info.resolveImageLayout = attachment_info.imageLayout;
				attachment_info.resolveImageView = ResolveImageView(resolve_desc, resolve_texture, pass_desc.name);
			}
			if (allocatedTexture.isSwapchainImage()) {
				attachment_info.isSwapchainImage = true;
				const auto& swapchain = rCtx._swapchain;
				const uint32_t numImages = swapchain->getNumOfSwapchainImages();
				for (uint32_t i = 0; i < numImages; i++) {
					const TextureHandle handle = swapchain->getSwapchainTextureHandle(i);
					attachment_info.swapchainImageViews[i] = rCtx.view(handle).getImageView();
				}
			}

			// submit AttachmentInfo
			if (isDepthAttachment)
				outPass.depthAttachment.emplace(attachment_info);
			else
				outPass.colorAttachments.emplace_back(attachment_info);

			// calculate the necessary dimensions of the vkCmdBeginRendering call
			// thats all the below code does
			const Dimensions& baseDims = allocatedTexture.getDimensions();
			const uint32_t mipLevel = texDesc.baseLevel.value_or(0);
			const uint32_t width = std::max(1u, baseDims.width >> mipLevel);
			const uint32_t height = std::max(1u, baseDims.height >> mipLevel);

			if ((max_width != width || max_height != height) && (max_width != 0 || max_height != 0))
				LOG_SYSTEM(LogType::Warning, "Pass '{}': You have attachments of different dimensions, this is allowed but experimental.", pass_desc.name);
			max_width = std::max(max_width, width);
			max_height = std::max(max_height, height);
		}
		outPass.renderArea = {{0, 0}, {max_width, max_height}};
	}

	void RenderGraph::performDryRun(CTX& rCtx) {
		// preserve any in-flight command buffer (e.g. when compile() is triggered from inside
		// execute() after acquireCommand() has installed the real cmd buffer).
		const CommandBuffer saved = rCtx._currentCommandBuffer;
		for (const CompiledPass& pass: _compiledPasses) {
			// by default CommandBuffer will have _isDryRun = true
			CommandBuffer dryCmd;
			dryCmd._ctx = &rCtx;
			dryCmd._activePass = pass;
			dryCmd._viewMask = pass.viewMask;
			rCtx._currentCommandBuffer = dryCmd;
			ASSERT_MSG(pass.executeCallback != nullptr, "Pass '{}' doesn't have an execute callback, something went horribly wrong!", pass.name);
			pass.executeCallback(dryCmd);
		}
		rCtx._currentCommandBuffer = saved;
	}

	bool isWrite(Layout layout) {
		return layout == Layout::GENERAL || layout == Layout::TRANSFER_DST;
	}
	bool isWrite(BufferAccess access) {
		return access == BufferAccess::ShaderReadWrite || access == BufferAccess::ShaderWrite || access == BufferAccess::TransferWrite;
	}


	// unweighted, directed
	// fixed vertex amount
	template<size_t Vertices>
	struct AdjacencyMatrix {
		void addEdge(size_t x, size_t y) {
			if (x >= Vertices || y >= Vertices) return;
			size_t bitIndex = (y * Vertices) + x;
			data.set(bitIndex);
		}
		void removeEdge(size_t x, size_t y) {
			if (x >= Vertices || y >= Vertices) return;
			size_t bitIndex = (y * Vertices) + x;
			data.reset(bitIndex);
		}
		bool hasEdge(size_t x, size_t y) {
			if (x >= Vertices || y >= Vertices) return false;
			return data.test((y * Vertices) + x);
		}
		std::bitset<Vertices * Vertices> data;
	};

	struct DirectedGraph {
		size_t nodeCount;
		// indicies represent original pass index
		// predecessors[2] = {0, 1} represents the third pass that must wait until first and second run
		// will always be same size vector as the # of passes
		std::vector<std::unordered_set<uint32_t>> predecessors;
		std::vector<std::vector<uint32_t>> successors;
		std::vector<uint32_t> inDegree;

		explicit DirectedGraph(uint32_t vertexCount) : nodeCount(vertexCount), predecessors(vertexCount), successors(vertexCount), inDegree(vertexCount) {}

		void addEdge(uint32_t from, uint32_t to) {
			if (from == to) return;
			predecessors.at(to).insert(from);
		}

		std::optional<std::vector<uint32_t>> topologicalSort() const {
			std::vector<uint32_t> local_in_degrees = inDegree;
			std::queue<uint32_t> ready;
			std::vector<uint32_t> sorted_order;
			sorted_order.reserve(this->nodeCount);

			// get root nodes, aka a node with no degrees incoming
			for (uint32_t i = 0; i < nodeCount; ++i) {
				if (local_in_degrees[i] == 0)
					ready.push(i);
			}
			// process from root nodes
			while (!ready.empty()) {
				uint32_t node = ready.front(); ready.pop();
				sorted_order.push_back(node);
				for (uint32_t successor : successors[node]) {
					if (--local_in_degrees[successor] == 0)
						ready.push(successor);
				}
			}
			// if size is not the same than cycles exist, thus cant be topo sorted
			if (sorted_order.size() != nodeCount) return std::nullopt;
			return sorted_order;
		}
	};

	DirectedGraph build_directed_graph(const std::vector<PassDesc>& pass_descs) {
		const size_t pass_desc_count = pass_descs.size();

		auto collectTexAccess = [](const PassDesc& desc) {
			std::vector<std::pair<TextureHandle, bool>> result;
			result.reserve(desc.textureDependencyOperations.size() + desc.attachmentOperations.size());
			for (const auto& texDep : desc.textureDependencyOperations) {
				result.emplace_back(texDep.texDesc.texture.handle(), isWrite(texDep.desiredLayout));
			}
			for (const auto& attachDep : desc.attachmentOperations) {
				// an attachment is always written to
				result.emplace_back(attachDep.texDesc.texture.handle(), true);
				if (attachDep.resolveTexDesc.has_value())
					result.emplace_back(attachDep.resolveTexDesc->texture.handle(), true);
			}
			return result;
		};
		auto collectBufAccess = [](const PassDesc& desc) {
			std::vector<std::pair<BufferHandle, bool>> result;
			result.reserve(desc.bufferDependencyOperations.size());
			for (const auto& bufDep : desc.bufferDependencyOperations) {
				result.emplace_back(bufDep.buffer.handle(), isWrite(bufDep.access));
			}
			return result;
		};

		// build our tracking vars
		// where a handle represents a resource, the integar represents its pass_id
		std::unordered_map<TextureHandle, uint32_t> lastTexWriter;
		std::unordered_map<TextureHandle, std::vector<uint32_t>> lastTexReaders;

		std::unordered_map<BufferHandle, uint32_t> lastBufWriter;
		std::unordered_map<BufferHandle, std::vector<uint32_t>> lastBufReaders;

		DirectedGraph result(pass_desc_count);
		// first collect all edges from the graph sequentially
		for (int i = 0; i < pass_desc_count; i++) {
			const PassDesc& desc = pass_descs[i];
			// h = handle
			// go through textures
			for (const auto& [h, isWrite] : collectTexAccess(desc)) {
				// if the resource isnt added already
				if (auto it = lastTexWriter.find(h); it != lastTexWriter.end())
					result.addEdge(it->second, i);

				if (isWrite) {
					// WAW
					for (const uint32_t reader : lastTexReaders[h])
						result.addEdge(reader, i);
					lastTexWriter[h] = i;
					lastTexReaders[h].clear();
				} else {
					// WAR
					lastTexReaders[h].push_back(i);
				}
			}
			// go through buffers
			for (const auto& [h, isWrite] : collectBufAccess(desc)) {
				if (auto it = lastBufWriter.find(h); it != lastBufWriter.end())
					result.addEdge(it->second, i);
				if (isWrite) {
					for (const uint32_t reader : lastBufReaders[h])
						result.addEdge(reader, i);
					lastBufWriter[h] = i;
					lastBufReaders[h].clear();
				} else {
					lastBufReaders[h].push_back(i);
				}
			}
		}
		return result;
	}

	void report_cycle_and_assert(const DirectedGraph& graph, const std::vector<PassDesc>& pass_descs) {
		const size_t node_count = graph.nodeCount;
		// copy this
		std::vector<uint32_t> local_in_degree = graph.inDegree;
		std::queue<uint32_t> ready;
		std::vector<bool> resolved(node_count, false);
		size_t resolved_count = 0;

		for (uint32_t i = 0; i < node_count; ++i) {
			if (local_in_degree[i] == 0) {
				ready.push(i);
			}
		}
		while (!ready.empty()) {
			const uint32_t node = ready.front();
			ready.pop();
			resolved[node] = true;
			resolved_count++;

			for (uint32_t succ : graph.successors[node]) {
				if (--local_in_degree[succ] == 0) {
					ready.push(succ);
				}
			}
		}
		// this should never happen :p
		if (resolved_count == node_count) return;
		// actually perform reporting now
		std::string report_string;
		for (uint32_t i = 0; i < node_count; ++i) {
			if (!resolved[i]) {
				report_string += fmt::format("\n- '{}' is stuck waiting on:", pass_descs[i].name);
				for (uint32_t pred : graph.predecessors[i]) {
					if (!resolved[pred]) {
						report_string += fmt::format(" '{}'", pass_descs[pred].name);
					}
				}
			}
		}
		ASSERT_MSG(false, "RenderGraph has a cycle ({} passes unresolved): {}", (node_count - resolved_count), report_string);
	}

	std::vector<uint32_t> cull_unused_passes(
		const std::vector<uint32_t>& topo_sorted_order,
		const std::vector<std::unordered_set<uint32_t>>& predecessors,
		const std::vector<PassDesc>& pass_descs) {

		const size_t node_count = topo_sorted_order.size();
		// now that sortedOrder is topologically sorted, we can perform culling from the swapchain pass
		// todo: this only walks back from a swapchain image, headless will mean everything is culled
		std::unordered_set<uint32_t> sinkPasses;
		for (uint32_t i = 0; i < node_count; i++) {
			const PassDesc& pass_desc = pass_descs[i];
			for (const auto& attachDesc : pass_desc.attachmentOperations) {
				if (attachDesc.texDesc.texture->isSwapchainImage()) {
					sinkPasses.insert(i);
					break;
				}
			}
			// verify that it is a write to swapchain not a read for some reason
			for (const auto& texDesc : pass_desc.textureDependencyOperations) {
				if (texDesc.texDesc.texture->isSwapchainImage() && isWrite(texDesc.desiredLayout)) {
					sinkPasses.insert(i);
					break;
				}
			}
		}
		// now reverse BFS
		std::unordered_set<uint32_t> reachablePasses;
		std::queue<uint32_t> worklist;
		for (uint32_t sink : sinkPasses) {
			worklist.push(sink);
			reachablePasses.insert(sink);
		}
		while (!worklist.empty()) {
			const uint32_t node = worklist.front(); worklist.pop();
			for (uint32_t pred : predecessors[node]) {
				if (reachablePasses.insert(pred).second)
					worklist.push(pred);
			}
		}
		// filter sortedOrder while preserving order
		std::vector<uint32_t> result;
		result.reserve(reachablePasses.size());
		for (uint32_t passIdx : topo_sorted_order) {
			if (reachablePasses.contains(passIdx))
				result.push_back(passIdx);
		}
		return result;
	}

	ExecutionSchedule calculate_execution_schedule(const std::vector<uint32_t>& culled_order, const std::vector<std::unordered_set<uint32_t>>& predecessors) {
		ExecutionSchedule result;
		uint32_t max_depth = 0;
		// find the depth for every active (culled) pass
		for (const uint32_t pass_idx : culled_order) {
			uint32_t current_depth = 0;
			for (uint32_t pred : predecessors[pass_idx]) {
				// only get predecessors that arent culled
				if (auto it = result.pass_depths.find(pred); it != result.pass_depths.end()) {
					current_depth = std::max(current_depth, it->second + 1);
				}
			}
			result.pass_depths[pass_idx] = current_depth;
			// update max if necessary
			max_depth = std::max(max_depth, current_depth);
		}
		// group passes into layers
		result.layers.resize(max_depth + 1);
		for (const uint32_t pass_idx : culled_order) {
			const uint32_t depth = result.pass_depths[pass_idx];
			result.layers[depth].push_back(pass_idx);
		}
		return result;
	}

	// purely for development purposes
	static void DumpRenderGraphDot(
		const std::vector<CompiledPass>& compiledPasses,
		const std::vector<std::unordered_set<uint32_t>>& predecessors,
		const ExecutionSchedule& schedule
	) {
		const char* path = std::getenv("MYTHRIL_DUMP_RG");
		if (!path || !*path) return;

		std::ofstream out(path);
		if (!out) {
			LOG_SYSTEM(LogType::Warning, "MYTHRIL_DUMP_RG set but failed to open '{}' for writing.", path);
			return;
		}

		auto queueColor = [](QueueAffinity q) {
			switch (q) {
				case QueueAffinity::Graphics:      return "lightblue";
				case QueueAffinity::AsyncCompute:  return "palegreen";
				case QueueAffinity::AsyncTransfer: return "lightyellow";
			}
			return "white";
		};
		auto typeLabel = [](PassDesc::Type t) {
			switch (t) {
				case PassDesc::Type::Graphics:     return "G";
				case PassDesc::Type::Compute:      return "C";
				case PassDesc::Type::Intermediate: return "I";
			}
			return "?";
		};

		std::unordered_map<uint32_t, const CompiledPass*> byPassIdx;
		byPassIdx.reserve(compiledPasses.size());
		for (const CompiledPass& p : compiledPasses)
			byPassIdx[p.passIndex] = &p;

		out << "digraph RenderGraph {\n";
		out << "  rankdir=LR;\n";
		out << "  node [shape=box, style=\"filled,rounded\", fontname=\"Helvetica\"];\n";
		out << "  edge [color=\"#555555\"];\n";

		for (size_t depth = 0; depth < schedule.layers.size(); ++depth) {
			out << "  subgraph cluster_d" << depth << " {\n";
			out << "    label=\"depth " << depth << "\"; style=dashed; color=\"#999999\";\n";
			out << "    { rank=same;\n";
			for (uint32_t passIdx : schedule.layers[depth]) {
				auto it = byPassIdx.find(passIdx);
				if (it == byPassIdx.end()) continue;
				const CompiledPass* p = it->second;
				out << "      p" << passIdx
				    << " [label=\"" << p->name << "\\n[" << typeLabel(p->type) << "]\""
				    << ", fillcolor=" << queueColor(p->queue) << "];\n";
			}
			out << "    }\n";
			out << "  }\n";
		}

		for (const CompiledPass& p : compiledPasses) {
			for (uint32_t pred : predecessors[p.passIndex]) {
				if (byPassIdx.count(pred))
					out << "  p" << pred << " -> p" << p.passIndex << ";\n";
			}
		}
		out << "}\n";
		LOG_SYSTEM(LogType::Info, "RenderGraph DOT written to '{}'.", path);

		// flamegraph-style SVG: queues as rows, depth layers as columns
		const std::string flamePath = std::string(path) + ".flame.svg";
		std::ofstream svg(flamePath);
		if (!svg) {
			LOG_SYSTEM(LogType::Warning, "Failed to open '{}' for writing flame SVG.", flamePath);
			return;
		}

		struct Lane { QueueAffinity q; const char* label; const char* color; };
		const Lane lanes[] = {
			{QueueAffinity::Graphics,      "Graphics",      "#7eb6ff"},
			{QueueAffinity::AsyncCompute,  "AsyncCompute",  "#9be39b"},
			{QueueAffinity::AsyncTransfer, "AsyncTransfer", "#f5e189"},
		};
		constexpr int blockH = 24;       // min block height (~text + padding)
		constexpr int rowGap = 6;
		constexpr int colW = 180;
		constexpr int padL = 140;
		constexpr int padT = 40;
		const int numLayers = static_cast<int>(schedule.layers.size());

		// per-lane height = max passes in any column for that lane
		std::array<int, std::size(lanes)> laneMaxPasses{};
		std::vector<std::array<std::vector<const CompiledPass*>, std::size(lanes)>> grid(numLayers);
		for (int d = 0; d < numLayers; ++d) {
			for (uint32_t passIdx : schedule.layers[d]) {
				auto it = byPassIdx.find(passIdx);
				if (it == byPassIdx.end()) continue;
				for (size_t li = 0; li < std::size(lanes); ++li) {
					if (lanes[li].q == it->second->queue) {
						grid[d][li].push_back(it->second);
						laneMaxPasses[li] = std::max(laneMaxPasses[li], static_cast<int>(grid[d][li].size()));
						break;
					}
				}
			}
		}
		std::array<int, std::size(lanes)> laneH{};
		std::array<int, std::size(lanes)> laneY{};
		int cursor = padT;
		for (size_t li = 0; li < std::size(lanes); ++li) {
			const int n = std::max(1, laneMaxPasses[li]);
			laneH[li] = n * blockH + (n + 1) * rowGap;
			laneY[li] = cursor;
			cursor += laneH[li];
		}
		const int width = padL + colW * std::max(1, numLayers) + 20;
		const int height = cursor + 30;

		svg << "<svg xmlns=\"http://www.w3.org/2000/svg\" width=\"" << width << "\" height=\"" << height
		    << "\" font-family=\"Helvetica, Arial, sans-serif\" font-size=\"12\">\n";
		svg << "  <rect width=\"100%\" height=\"100%\" fill=\"#1e1e22\"/>\n";

		// depth column headers + vertical guides
		for (int d = 0; d < numLayers; ++d) {
			const int x = padL + d * colW;
			svg << "  <text x=\"" << (x + colW / 2) << "\" y=\"" << (padT - 14)
			    << "\" fill=\"#cccccc\" text-anchor=\"middle\">depth " << d << "</text>\n";
			svg << "  <line x1=\"" << x << "\" y1=\"" << padT << "\" x2=\"" << x
			    << "\" y2=\"" << cursor << "\" stroke=\"#333\"/>\n";
		}

		// lane backgrounds + labels
		for (size_t li = 0; li < std::size(lanes); ++li) {
			svg << "  <rect x=\"0\" y=\"" << laneY[li] << "\" width=\"" << width << "\" height=\"" << laneH[li]
			    << "\" fill=\"" << (li % 2 ? "#26262c" : "#222226") << "\"/>\n";
			svg << "  <text x=\"" << (padL - 10) << "\" y=\"" << (laneY[li] + laneH[li] / 2 + 4)
			    << "\" fill=\"#dddddd\" text-anchor=\"end\">" << lanes[li].label << "</text>\n";
		}

		// passes
		for (int d = 0; d < numLayers; ++d) {
			for (size_t li = 0; li < std::size(lanes); ++li) {
				const auto& passes = grid[d][li];
				for (size_t i = 0; i < passes.size(); ++i) {
					const int x = padL + d * colW + 6;
					const int y = laneY[li] + rowGap + static_cast<int>(i) * (blockH + rowGap);
					const int w = colW - 12;
					svg << "    <rect x=\"" << x << "\" y=\"" << y << "\" width=\"" << w << "\" height=\"" << blockH
					    << "\" rx=\"4\" fill=\"" << lanes[li].color << "\" stroke=\"#111\"/>\n";
					svg << "    <text x=\"" << (x + 8) << "\" y=\"" << (y + blockH / 2 + 4)
					    << "\" fill=\"#111\">" << passes[i]->name << "</text>\n";
				}
			}
		}

		svg << "</svg>\n";
		LOG_SYSTEM(LogType::Info, "RenderGraph flame SVG written to '{}'.", flamePath);
	}

	void RenderGraph::compile(CTX& rCtx) {
		MYTH_PROFILER_FUNCTION_COLOR(MYTH_PROFILER_COLOR_RENDERGRAPH);
		this->_compiledPasses.clear();
		this->_resourceTrackers.clear();
		this->_bufferTrackers.clear();

		const DirectedGraph directed_graph = build_directed_graph(this->_passDescriptions);

		// yeah i know this is doing more work then necessary but its way better to understand
		const std::vector<std::unordered_set<uint32_t>>& passPredecessors = directed_graph.predecessors;
		const std::optional<std::vector<uint32_t>>& sorted_order_opt = directed_graph.topologicalSort();
		if (!sorted_order_opt) {
			// who cares about the recalculation overhead
			report_cycle_and_assert(directed_graph, this->_passDescriptions);
		}
		const std::vector<uint32_t>& culled_order = cull_unused_passes(sorted_order_opt.value(), passPredecessors, _passDescriptions);

		const ExecutionSchedule schedule = calculate_execution_schedule(culled_order, passPredecessors);
		this->_schedule = schedule;

		// map from passDescription to _compiledPasses, reversed basically
		std::unordered_map<uint32_t, uint32_t> passToCompiled;
		passToCompiled.reserve(culled_order.size());
		for (uint32_t i = 0; i < static_cast<uint32_t>(culled_order.size()); ++i)
			passToCompiled[culled_order[i]] = i;

		// assign and warn queues for each pass
		const bool canUseAC = rCtx._immAsyncCompute != nullptr;
		const bool canUseAT = rCtx._immAsyncTransfer != nullptr;
		auto sanitizeQueue = [canUseAC, canUseAT](const QueueAffinity requested_queue, const std::string& pass_name) {
			// demote passes that request a queue but we dont have access to
			if (requested_queue == QueueAffinity::AsyncCompute && !canUseAC) {
				LOG_SYSTEM_NOSOURCE(LogType::Warning, "Pass '{}' demoted to graphics: No async compute family available.", pass_name);
				return QueueAffinity::AsyncTransfer;
			}
			if (requested_queue == QueueAffinity::AsyncTransfer && !canUseAT) {
				if (canUseAC) {
					LOG_SYSTEM_NOSOURCE(LogType::Info, "Pass '{}' routed to async compute: No dedicated transfer family.", pass_name);
					return QueueAffinity::AsyncCompute;
				}
				LOG_SYSTEM_NOSOURCE(LogType::Warning, "Pass '{}' demoted to graphics: No async family available.", pass_name);
				return QueueAffinity::Graphics;
			}
			return requested_queue;
		};

		// compile works in this order
		this->_compiledPasses.reserve(culled_order.size());
		for (const uint32_t pass_idx : culled_order) {
			const PassDesc& pass_desc = this->_passDescriptions[pass_idx];
			CompiledPass compiled_pass;
			compiled_pass.name = pass_desc.name;
			compiled_pass.passIndex = pass_idx;
			compiled_pass.queue = sanitizeQueue(pass_desc.queue, pass_desc.name);
			compiled_pass.type = pass_desc.type;
			compiled_pass.executeCallback = pass_desc.executeCallback;
			compiled_pass.layerCount = pass_desc.layerCount;
			compiled_pass.viewMask = pass_desc.viewMask;
			compiled_pass.conditionCallback = pass_desc.conditionCallback;

			// do not worry about the return value,
			// every type of process should run for every pass
			processPassResources(pass_desc, compiled_pass);
			processAttachments(rCtx, pass_desc, compiled_pass);

			this->_compiledPasses.push_back(std::move(compiled_pass));
		}


		// build _layerByQueue from post cleaned _compiledPassses
		// requires that correct queues have already been selected
		this->_layerByQueue.assign(schedule.layers.size(), {});
		for (size_t depth = 0; depth < schedule.layers.size(); ++depth) {
			for (const uint32_t pass_idx : schedule.layers[depth]) {
				const uint32_t compiled_idx = passToCompiled[pass_idx];
				const auto q = static_cast<size_t>(_compiledPasses[compiled_idx].queue);
				this->_layerByQueue[depth][q].push_back(compiled_idx);
			}
		}
		performDryRun(rCtx);
		this->_hasCompiled = true;
		this->_compiledEpoch = rCtx._resourceEpoch;
#ifdef DEBUG
		DumpRenderGraphDot(this->_compiledPasses, passPredecessors, schedule);
#endif
	}

	void RenderGraph::trackWindowSized(Texture& texture) {
		trackWindowSized(texture, [](const Dimensions& dims) { return dims; });
	}

	void RenderGraph::trackWindowSized(Texture& texture, std::function<Dimensions(Dimensions)> scaleFn) {
		ASSERT_MSG(texture.valid(), "RenderGraph::trackWindowSized called with an invalid texture.");
		_windowSizedTextures.push_back({.texture = &texture, .scaleFn = std::move(scaleFn)});
	}

	void RenderGraph::resizeTrackedWindowSized(const Dimensions& swapchainDimensions) const {
		for (const WindowSizedTexture& tracked: _windowSizedTextures) {
			ASSERT(tracked.texture);
			const Dimensions nextDimensions = tracked.scaleFn ? tracked.scaleFn(swapchainDimensions) : swapchainDimensions;
			tracked.texture->resize(nextDimensions);
		}
	}

	void RenderGraph::PerformBarrierTransitions(CommandBuffer& cmd, const CompiledPass& compiledPass) {
		if (compiledPass.imageBarriers.empty() && compiledPass.bufferBarriers.empty())
			return;
		ASSERT(cmd._wrapper->_cmdBuf);

		std::vector<VkImageMemoryBarrier2> vkImageBarriers;
		vkImageBarriers.reserve(compiledPass.imageBarriers.size());
		std::vector<VkBufferMemoryBarrier2> vkBufferBarriers;
		vkBufferBarriers.reserve(compiledPass.bufferBarriers.size());
		// we have to update the image associated if its a swapchain image
		// as it changes every frame
		for (const CompiledImageBarrier& req: compiledPass.imageBarriers) {
			const TextureHandle activeHandle = req.isSwapchain ? cmd._ctx->getCurrentSwapchainTexHandle() : req.handle;
			const AllocatedTexture& activeTexture = cmd._ctx->view(activeHandle);
			if (!_resourceTrackers.contains(activeHandle)) {
				_resourceTrackers.emplace(activeHandle, TextureStateTracker{activeTexture.getNumMips(), activeTexture.getNumLayers()});
			}
			TextureStateTracker& tracker = _resourceTrackers.at(activeHandle);
			const std::vector<TextureStateTracker::SubresourceEntry> currentStates = tracker.getOverlappingStates(req.range);
			for (const auto& current: currentStates) {
				if (!NeedsBarrier(current.state, req.dstLayout, req.dstMask))
					continue;
				vkImageBarriers.push_back(
				        {.sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER_2,
				         .srcStageMask = current.state.mask.stage,
				         .srcAccessMask = current.state.mask.access,
				         .dstStageMask = req.dstMask.stage,
				         .dstAccessMask = req.dstMask.access,
				         .oldLayout = current.state.layout,
				         .newLayout = req.dstLayout,
				         .srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
				         .dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
				         .image = activeTexture.getImage(),
				         .subresourceRange = MakeVkRange(activeTexture.getFormat(), current.range)}
				);
			}
			tracker.setState(req.range, {req.dstLayout, req.dstMask});
			cmd._ctx->access(activeHandle)._vkCurrentImageLayout = req.dstLayout;
		}
		for (const CompiledBufferBarrier& req: compiledPass.bufferBarriers) {
			const AllocatedBuffer& buffer = cmd._ctx->view(req.handle);
			const vkutil::StageAccess currentState = _bufferTrackers.contains(req.handle) ? _bufferTrackers.at(req.handle) : vkutil::StageAccess{};
			if (NeedsBufferBarrier(currentState, req.dstMask)) {
				vkBufferBarriers.push_back({
					.sType = VK_STRUCTURE_TYPE_BUFFER_MEMORY_BARRIER_2,
					.srcStageMask = currentState.stage,
					.srcAccessMask = currentState.access,
					.dstStageMask = req.dstMask.stage,
					.dstAccessMask = req.dstMask.access,
					.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
					.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
					.buffer = buffer._vkBuffer,
					.offset = 0,
					.size = VK_WHOLE_SIZE}
				);
			}
			_bufferTrackers[req.handle] = req.dstMask;
		}
		if (!vkImageBarriers.empty() || !vkBufferBarriers.empty()) {
			const VkDependencyInfo dependencyInfo = {
			    .sType = VK_STRUCTURE_TYPE_DEPENDENCY_INFO,
			    .pNext = nullptr,
			    .bufferMemoryBarrierCount = static_cast<uint32_t>(vkBufferBarriers.size()),
			    .pBufferMemoryBarriers = vkBufferBarriers.data(),
			    .imageMemoryBarrierCount = static_cast<uint32_t>(vkImageBarriers.size()),
			    .pImageMemoryBarriers = vkImageBarriers.data()
			};
			vkCmdPipelineBarrier2(cmd._wrapper->_cmdBuf, &dependencyInfo);
		}
	}

	// will be the only execute but left for now
	void RenderGraph::execute(CTX& rCtx) {
		MYTH_PROFILER_FUNCTION_COLOR(MYTH_PROFILER_COLOR_RENDERGRAPH);
		ASSERT_MSG(_hasCompiled, "A RenderGraph must be compiled before it can be executed!");

		ASSERT_MSG(!rCtx.isHeadless(), "headless is currently not supported");

		// auto-recompile if any resource topology changed since last compile
		// (texture resize, swapchain recreate/destroy).
		if (_compiledEpoch != rCtx._resourceEpoch) {
			LOG_SYSTEM(LogType::Info, "Performing recompile on RenderGraph!");
			compile(rCtx);
		}

		CommandBuffer cmd = rCtx.acquireCommand(CommandBuffer::Type::Graphics);
		this->execute(cmd);
		rCtx.submitCommand(cmd);
	}

	void RenderGraph::execute(CommandBuffer& cmd) {
		MYTH_PROFILER_FUNCTION_COLOR(MYTH_PROFILER_COLOR_RENDERGRAPH);
		ASSERT_MSG(!cmd.isDrying(), "You cannot call RenderGraph::execute inside an execution callback!");
		ASSERT_MSG(_hasCompiled, "RenderGraph must be compiled before it can be executed!");
		// auto-recompile if any resource topology changed since last compile (texture resize, swapchain recreate/destroy).
		// single uint64 compare on the hot path; recompile only runs when actually stale (typically window-resize frames).
		if (_compiledEpoch != cmd._ctx->_resourceEpoch) {
			compile(*cmd._ctx);
		}

		for (CompiledPass& pass: _compiledPasses) {
			if (pass.conditionCallback && !pass.conditionCallback())
				continue;
			const bool isGraphics = pass.type == PassDesc::Type::Graphics;
			// perform batched vkCmdPipelineBarrier
			PerformBarrierTransitions(cmd, pass);
			// switch color attachment imageViews for Swapchain
			if (!cmd._ctx->isHeadless()) {
				const uint32_t swapIdx = cmd._ctx->_swapchain->getCurrentImageIndex();
				for (auto& color: pass.colorAttachments) {
					if (color.isSwapchainImage)
						color.imageView = color.swapchainImageViews[swapIdx];
				}
				if (pass.depthAttachment && pass.depthAttachment->isSwapchainImage)
					pass.depthAttachment->imageView = pass.depthAttachment->swapchainImageViews[swapIdx];
			}
			// reset current states
			cmd._currentPipelineInfo = nullptr;
			cmd._currentPipelineHandle = {};
			cmd._activePass = pass;
			// we already checked if it has an execute callback so it should be guaranteed
			if (isGraphics) cmd.cmdBeginRendering(pass.layerCount, pass.viewMask);
			pass.executeCallback(cmd);
			if (isGraphics) cmd.cmdEndRendering();
		}
		if (!cmd._ctx->isHeadless()) {
			// fixme: make this cleaner im so lazy right now
			cmd.cmdTransitionLayout(cmd._ctx->getCurrentSwapchainTexHandle(), VK_IMAGE_LAYOUT_PRESENT_SRC_KHR);
			const vkutil::StageAccess dstMask = vkutil::GetPipelineStageAccess(VK_IMAGE_LAYOUT_PRESENT_SRC_KHR);
			const TextureHandle currentSwapchainHandle = cmd._ctx->getCurrentSwapchainTexHandle();
			if (_resourceTrackers.contains(currentSwapchainHandle)) {
				TextureStateTracker& tracker = _resourceTrackers.at(currentSwapchainHandle);
				tracker.setState(tracker.wholeResourceRange(), SubresourceState{VK_IMAGE_LAYOUT_PRESENT_SRC_KHR, dstMask});
			}
			cmd._ctx->access(currentSwapchainHandle)._vkCurrentImageLayout = VK_IMAGE_LAYOUT_PRESENT_SRC_KHR;
		}
	}
} // namespace mythril
