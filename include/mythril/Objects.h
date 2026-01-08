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
		ObjectHolder(CTX* ctx, InternalHandle handle) : _pCtx(ctx), _handle(handle) {}
		~ObjectHolder() {
			_pCtx->destroy(_handle);
		}
		ObjectHolder(const ObjectHolder&) = delete;
		ObjectHolder(ObjectHolder&& other) noexcept : _pCtx(other._pCtx), _handle(other._handle) {
			other._pCtx = nullptr;
			other._handle = InternalHandle{};
		}
		ObjectHolder& operator=(ObjectHolder&& other) noexcept {
			std::swap(_pCtx, other.ctx_);
			std::swap(_handle, other.handle_);
			return *this;
		}
		ObjectHolder& operator=(std::nullptr_t) {
			this->reset();
			return *this;
		}
	public:
		// i dont want this to be explicit yeah
		inline operator InternalHandle() const {
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
	private:
		InternalHandle _handle = {};
		CTX* _pCtx = nullptr;
	};

	using Buffer = ObjectHolder<InternalBufferHandle>;
	using Texture = ObjectHolder<InternalTextureHandle>;
	using Sampler = ObjectHolder<InternalSamplerHandle>;
	using Shader = ObjectHolder<InternalShaderHandle>;
	using GraphicsPipeline = ObjectHolder<InternalGraphicsPipelineHandle>;
	using ComputePipeline = ObjectHolder<InternalComputePipelineHandle>;
}