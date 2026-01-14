//
// Created by Hayden Rivas on 12/31/25.
//

#pragma once

#include <unordered_map>

#include "../../lib/Specs.h"
#include "../../lib/ObjectHandles.h"

#include <volk.h>


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
		// i dont want this to be explicit yeah
		InternalHandle handle() const { return _handle; }

		AllocatedType* operator->();
		const AllocatedType* operator->() const;

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

	// opaque object used to index into _additionalViews
	class TextureView {
	public:
		TextureView() = default;
		constexpr bool isBase() const { return _index == 0; }
	private:
		uint32_t _index = 0;  // 0 = base view, 1+ are indices into _additionalViews vector
		explicit constexpr TextureView(uint32_t index) : _index(index) {}
		friend class Texture;
	};
	// ref: AllocatedTexture
	class Texture : public ObjectHolder<TextureHandle> {
		using ObjectHolder::ObjectHolder;
	public:
		using ObjectHolder::index;
		uint32_t index(TextureView view) const;

		TextureView createView(const TextureViewSpec& spec);
		void resize(Dimensions newDimensions);

		TextureHandle getHandle(TextureView view = TextureView()) const;
	private:
		std::vector<TextureHandle> _additionalViews;
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
