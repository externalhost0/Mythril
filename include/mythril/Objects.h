//
// Created by Hayden Rivas on 12/31/25.
//

#pragma once


#include "../../lib/Specs.h"
#include "../../lib/ObjectHandles.h"

#include <volk.h>

#include <utility>
#include <unordered_map>

namespace mythril {
	struct TextureViewSpec;
	class CTX;

	class AllocatedTexture;
	class AllocatedBuffer;
	class AllocatedSampler;
	class AllocatedGraphicsPipeline;
	class AllocatedComputePipeline;
	class AllocatedShader;

	template<typename HandleType> struct HandleToAllocated;
	template<> struct HandleToAllocated<TextureHandle> { using type = AllocatedTexture; };
	template<> struct HandleToAllocated<BufferHandle> { using type = AllocatedBuffer; };
	template<> struct HandleToAllocated<SamplerHandle> { using type = AllocatedSampler; };
	template<> struct HandleToAllocated<ShaderHandle> { using type = AllocatedShader; };
	template<> struct HandleToAllocated<GraphicsPipelineHandle> { using type = AllocatedGraphicsPipeline; };
	template<> struct HandleToAllocated<ComputePipelineHandle> { using type = AllocatedComputePipeline; };

	template<typename HandleType>
	using HandleToAllocated_t = typename HandleToAllocated<HandleType>::type;

	// concept defined in CTX.h
	// basically a lvk Holder
	template<typename InternalHandle>
	class ObjectHolder {
		using AllocatedType = HandleToAllocated_t<InternalHandle>;
		friend class CTX;
		// basically for swapchain only
		void updateHandle(CTX* ctx, InternalHandle handle) {
			_pCtx = ctx;
			_handle = handle;
		}
	public:
		ObjectHolder() = default;
		ObjectHolder(CTX* ctx, InternalHandle handle) : _handle(handle), _pCtx(ctx) {}
		virtual ~ObjectHolder();
		ObjectHolder(const ObjectHolder&) = delete;
		ObjectHolder(ObjectHolder&& other) noexcept : _handle(other._handle), _pCtx(other._pCtx) {
			other._pCtx = nullptr;
			other._handle = InternalHandle{};
		}
		ObjectHolder& operator=(const ObjectHolder&) = delete;
		ObjectHolder& operator=(ObjectHolder&& other) noexcept {
			std::swap(_pCtx, other._pCtx);
			std::swap(_handle, other._handle);
			return *this;
		}
		ObjectHolder& operator=(std::nullptr_t) {
			this->reset();
			return *this;
		}
	public:
		InternalHandle handle() const { return _handle; }

		AllocatedType* operator->();
		const AllocatedType* operator->() const;
		// explicit versions
		AllocatedType& access();
		const AllocatedType& view() const;

		// functionality from handle
		bool valid() const { return _handle.valid(); }
		bool empty() const { return _handle.empty(); }
		uint32_t gen() const { return _handle.gen(); }
		uint32_t index() const { return _handle.index(); }

		// wrapper functionality
		void reset();
		// releases handle from being automatically cleaned
		InternalHandle release() {
			_pCtx = nullptr;
			return std::exchange(_handle, InternalHandle{});
		}
	protected:
		InternalHandle _handle = {};
		CTX* _pCtx = nullptr;
	};

	// ref: AllocatedSampler
	class Sampler : public ObjectHolder<SamplerHandle> {
		using ObjectHolder::ObjectHolder;
	public:

	};
	// ref: AllocatedBuffer
	class Buffer : public ObjectHolder<BufferHandle> {
		using ObjectHolder::ObjectHolder;
	public:
		VkDeviceAddress gpuAddress(size_t offset = 0);

	};

	// ref: AllocatedTexture
	class Texture : public ObjectHolder<TextureHandle> {
		using ObjectHolder::ObjectHolder;
	public:
		// opaque object used to index into _additionalViews
		using ViewKey = uint64_t;
		// we need some custom constructors and operators because of our additional data
		~Texture() override;
		Texture(Texture&& other) noexcept
			: ObjectHolder(std::move(other)),
			  _additionalViews(std::move(other._additionalViews)) {}
		Texture& operator=(const Texture&) = delete;
		Texture& operator=(Texture&& other) noexcept;

		// default index and index of specific view
		using ObjectHolder::index;
		using ObjectHolder::handle;
		TextureHandle handle(ViewKey key) const;
		uint32_t index(ViewKey key) const;

		ViewKey createView(const TextureViewSpec& spec);
		ViewKey getView(uint32_t baseMip, uint32_t baseLayer) {
			return packViewKey(baseMip, 1, baseLayer, 1);
		}
		void resize(const Dimensions& newDimensions);
	private:
		static constexpr uint64_t packViewKey(uint32_t baseMip, uint32_t numMips, uint32_t baseLayer, uint32_t numLayers) {
			return (static_cast<uint64_t>(baseMip & 0xFFFF) << 48) |
				   (static_cast<uint64_t>(numMips & 0xFFFF) << 32) |
				   (static_cast<uint64_t>(baseLayer & 0xFFFF) << 16) |
				   (static_cast<uint64_t>(numLayers & 0xFFFF));
		}
		std::unordered_map<uint64_t, TextureHandle> _additionalViews;
		friend class RenderGraph;
	};

	// todo: remove shader being its own object, its redundant and can be merged into the pipeline object
	// ref: AllocatedShader
	class Shader : public ObjectHolder<ShaderHandle> {
		using ObjectHolder::ObjectHolder;
	public:

	};
	// ref: AllocatedGraphicsPipeline
	class GraphicsPipeline : public ObjectHolder<GraphicsPipelineHandle> {
		using ObjectHolder::ObjectHolder;
	public:

	};
	// ref: AllocatedComputePipeline
	class ComputePipeline : public ObjectHolder<ComputePipelineHandle> {
		using ObjectHolder::ObjectHolder;
	public:

	};

}
