//
// Created by Hayden Rivas on 12/31/25.
//

#pragma once


#include "Specs.h"
#include "ObjectHandles.h"

#include <volk.h>

#include <utility>
#include <unordered_map>

namespace mythril {
	struct TextureViewSpec;
	class CTX;

	// cached texture subresource view key
	using TextureViewKey = uint64_t;

	namespace detail {
		// pack a subresource range into a view key
		constexpr TextureViewKey packTextureViewKey(uint32_t baseMip, uint32_t numMips, uint32_t baseLayer, uint32_t numLayers) {
			return (static_cast<uint64_t>(baseMip & 0xFFFF) << 48) |
			       (static_cast<uint64_t>(numMips & 0xFFFF) << 32) |
			       (static_cast<uint64_t>(baseLayer & 0xFFFF) << 16) |
			       (static_cast<uint64_t>(numLayers & 0xFFFF));
		}

		class AllocatedTexture;
		class AllocatedBuffer;
		class AllocatedSampler;
		class AllocatedShader;
		class AllocatedGraphicsPipeline;
		class AllocatedComputePipeline;
		class AllocatedRaytracingPipeline;
		class AllocatedAccelerationStructure;


		template<typename InternalHandle>
		class Holder {
			friend class ::mythril::CTX;
			// basically for swapchain only
			void updateHandle(CTX& ctx, InternalHandle handle) {
				_pCtx = &ctx;
				_handle = handle;
			}
		public:
			Holder() = default;
			Holder(CTX* ctx, InternalHandle handle) : _handle(handle), _pCtx(ctx) {}
			~Holder();
			Holder(const Holder&) = delete;
			Holder(Holder&& other) noexcept : _handle(other._handle), _pCtx(other._pCtx) {
				other._pCtx = nullptr;
				other._handle = InternalHandle{};
			}
			Holder& operator=(const Holder&) = delete;
			Holder& operator=(Holder&& other) noexcept {
				std::swap(_pCtx, other._pCtx);
				std::swap(_handle, other._handle);
				return *this;
			}
			Holder& operator=(std::nullptr_t) {
				this->reset();
				return *this;
			}
		public:
			InternalHandle handle() const { return _handle; }
			// resolve the handle without threading a CTX* for every call
			CTX* getContext() const { return _pCtx; }

			// functionality from handle
			bool valid() const { return _handle.valid(); }
			bool empty() const { return _handle.empty(); }
			uint32_t gen() const { return _handle.gen(); }
			uint32_t index() const { return _handle.index(); }

			// wrapper functionality
			void reset();
			// releases handle from being automatically cleaned, allows manual management
			InternalHandle release() {
				_pCtx = nullptr;
				return std::exchange(_handle, InternalHandle{});
			}
		protected:
			InternalHandle _handle = {};
			CTX* _pCtx = nullptr;
		};
	}

	// public resource wrappers
	// ref: detail::Allocated*
	using Sampler = detail::Holder<SamplerHandle>;
	using Buffer = detail::Holder<BufferHandle>;
	using Texture = detail::Holder<TextureHandle>;
	using Shader = detail::Holder<ShaderHandle>;
	using GraphicsPipeline = detail::Holder<GraphicsPipelineHandle>;
	using ComputePipeline = detail::Holder<ComputePipelineHandle>;
	using RaytracingPipeline = detail::Holder<RaytracingPipelineHandle>;
	using AccelerationStructure = detail::Holder<AccelerationStructureHandle>;
}
