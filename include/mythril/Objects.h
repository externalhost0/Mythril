//
// Created by Hayden Rivas on 12/31/25.
//

#pragma once

#include "../../lib/ObjectHandles.h"
#include "../../lib/CTX.h"

namespace mythril {

	// concept defined in CTX.h
	// basically a lvk Holder
	template<typenameInternalHandle InternalHandle>
	class ObjectHolder {
	public:
		ObjectHolder() = default;
		ObjectHolder(CTX* ctx, InternalHandle handle) : _handle(handle), _pCtx(ctx) {}
		~ObjectHolder() {
			_pCtx->destroy(_handle);
		}
		ObjectHolder(const ObjectHolder&) = delete;
		ObjectHolder(ObjectHolder&& other) noexcept : _handle(other._handle), _pCtx(other._pCtx) {
			other._pCtx = nullptr;
			other._handle = InternalHandle{};
		}
		ObjectHolder& operator=(std::nullptr_t) {
			this->reset();
			return *this;
		}
	public:
		// i dont want this to be explicit yeah
		operator InternalHandle() const {
			return _handle;
		}

		bool valid() const {
			return _handle.valid();
		}

		bool empty() const {
			return _handle.empty();
		}

		void reset() {
			// destroy function priv
			_pCtx->destroy(_handle);
			_pCtx = nullptr;
			_handle = InternalHandle{};
		}

		InternalHandle release() {
			_pCtx = nullptr;
			return std::exchange(_handle, InternalHandle{});
		}

		uint32_t gen() const {
			return _handle.gen();
		}
		uint32_t index() const {
			return _handle.index();
		}
		void* indexAsVoid() const {
			return _handle.indexAsVoid();
		}
		void* handleAsVoid() const {
			return _handle.handleAsVoid();
		}
	protected:
		InternalHandle _handle = {};
		CTX* _pCtx = nullptr;
	};


	class Sampler : ObjectHolder<InternalSamplerHandle> {
	public:
		[[nodiscard]] std::string_view getDebugName() const { return _pCtx->viewSampler(_handle).getDebugName(); }
	};

	class Texture : public ObjectHolder<InternalTextureHandle> {
	public:
		[[nodiscard]] InternalTextureHandle mip(uint8_t level) const { return mips[level]; }
		[[nodiscard]] uint64_t index() const { return _handle.index(); }

		[[nodiscard]] Dimensions getDimensions() const { return _pCtx->viewTexture(_handle).getDimensions(); }
		[[nodiscard]] VkFormat getFormat() const { return _pCtx->viewTexture(_handle).getFormat(); }
		[[nodiscard]] VkImageType getType() const { return _pCtx->viewTexture(_handle).getType(); }
		[[nodiscard]] bool hasMipmaps() const { return _pCtx->viewTexture(_handle).hasMipmaps(); }
		[[nodiscard]] std::string_view getDebugName() const { return _pCtx->viewTexture(_handle).getDebugName(); }
	private:
		std::vector<InternalTextureHandle> mips;
	};

	class Buffer : ObjectHolder<InternalBufferHandle> {
	public:
		[[nodiscard]] bool isMapped() const { return _pCtx->viewBuffer(_handle).isMapped(); }
		[[nodiscard]] std::string_view getDebugName() const { return _pCtx->viewBuffer(_handle).getDebugName(); }
	};

	// todo: remove shader being its own object, its redundant and can be merged into the pipeline object
	class Shader : ObjectHolder<InternalShaderHandle> {
	public:
		[[nodiscard]] std::string_view getDebugName() const { return _pCtx->viewShader(_handle).getDebugName(); }
	};

	class GraphicsPipeline : ObjectHolder<InternalGraphicsPipelineHandle> {
	public:
		[[nodiscard]] std::string_view getDebugName() const { return _pCtx->viewGraphicsPipeline(_handle).getDebugName(); }
	};
	class ComputePipeline : ObjectHolder<InternalComputePipelineHandle> {
	public:
		[[nodiscard]] std::string_view getDebugName() const { return _pCtx->viewComputePipeline(_handle).getDebugName(); }
	};

}