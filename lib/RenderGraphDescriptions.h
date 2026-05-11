//
// Created by Hayden Rivas on 1/20/26.
//

#pragma once

#include "../include/mythril/Objects.h"

#include <functional>
#include <optional>
#include <vector>
#include <volk.h>

namespace mythril {
	struct BasePassBuilder;
	class CommandBuffer;

	struct ClearColor {
		float r, g, b, a;
		ClearColor(float r, float g, float b, float a) :
		    r(r),
		    g(g),
		    b(b),
		    a(a) {}
		VkClearColorValue getAsVkClearColorValue() const { return {{r, g, b, a}}; }
	};

	struct ClearDepthStencil {
		float depth;
		uint32_t stencil;
		ClearDepthStencil(float depth, uint32_t stencil) :
		    depth(depth),
		    stencil(stencil) {}
		VkClearDepthStencilValue getAsVkClearDepthStencilValue() const { return {depth, stencil}; }
	};

	union ClearValue {
		ClearColor clearColor;
		ClearDepthStencil clearDepthStencil;

		// if the user sets no clear (have no intention of clearing) then do nothing ig
		ClearValue() {}
		static ClearValue color(float r, float g, float b, float a) {
			ClearValue v;
			v.clearColor = {r, g, b, a};
			return v;
		}
		static ClearValue depth(float depth, uint32_t stencil = 0) {
			ClearValue v;
			v.clearDepthStencil = {depth, stencil};
			return v;
		}
		VkClearValue getDepthStencilValue() const { return {.depthStencil = clearDepthStencil.getAsVkClearDepthStencilValue()}; }
		VkClearValue getColorValue() const { return {.color = clearColor.getAsVkClearColorValue()}; }
	};


	struct TextureDesc {
		TextureDesc() = delete;
		TextureDesc(Texture& tex) :
		    texture(tex) {}
		TextureDesc(const Texture& tex) :
		    texture(const_cast<Texture&>(tex)) {}
		Texture& texture;
		std::optional<uint32_t> baseLevel;
		std::optional<uint32_t> numLevels;
		std::optional<uint32_t> baseLayer;
		std::optional<uint32_t> numLayers;
		std::optional<TextureType> type;
	};
	struct AttachmentDesc {
		TextureDesc texDesc;
		ClearValue clearValue;
		LoadOp loadOp = LoadOp::DONT_CARE;
		StoreOp storeOp = StoreOp::DONT_CARE;
		std::optional<TextureDesc> resolveTexDesc;
	};
	enum class Layout : uint8_t {
		GENERAL,
		READ,
		TRANSFER_SRC,
		TRANSFER_DST,
		PRESENT
	};
	enum class BufferAccess : uint8_t {
		ShaderRead,
		ShaderWrite,
		ShaderReadWrite,
		IndexRead,
		IndirectRead,
		TransferRead,
		TransferWrite,
	};
	// not actually passed directly to dependency calls right now
	struct DependencyDesc {
		TextureDesc texDesc;
		Layout desiredLayout = Layout::READ;
	};
	struct BufferDependencyDesc {
		Buffer& buffer;
		BufferAccess access = BufferAccess::ShaderRead;
	};

	// user defined information from addPass and RenderPassBuilder
	struct PassDesc {
		enum class Type {
			Graphics,
			Compute,
			Intermediate,
			Presentation
		};
		// these first three members are sent straight over to PassCompiled
		std::string name;
		Type type;
		std::function<void(CommandBuffer&)> executeCallback{};
		std::vector<AttachmentDesc> attachmentOperations;
		std::vector<DependencyDesc> dependencyOperations;
		std::vector<BufferDependencyDesc> bufferDependencyOperations;
	};
} // namespace mythril
