//
// Created by Hayden Rivas on 1/10/26.
//
#include "../include/mythril/Objects.h"
#include "CTX.h"
#include "HelperMacros.h"

namespace mythril {

	// individual function implementations
	VkDeviceAddress Buffer::gpuAddress(size_t offset) {
		const VkDeviceAddress addr = this->_pCtx->view(_handle)._vkDeviceAddress;
		ASSERT_MSG(addr, "Buffer doesnt have a valid device address!");
		return addr + offset;
	}

	TextureHandle Texture::getHandle(const TextureView view) const {
		// when view is base, we can evaluate this at compile time
		if (view.isBase()) {
			return _handle;
		}
		uint32_t viewIndex = view._index - 1;  // Convert from 1-based to 0-based
		ASSERT_MSG(viewIndex < _additionalViews.size(),
				   "Invalid TextureView - index {} out of range (have {} views)",
				   viewIndex, _additionalViews.size());
		return _additionalViews[viewIndex];
	}

	uint32_t Texture::index(const TextureView view) const {
		return getHandle(view).index();
	}

	void Texture::resize(const Dimensions newDimensions) {
		_pCtx->resizeTexture(_handle, newDimensions);
	}

	TextureView Texture::createView(const TextureViewSpec& spec) {
		_additionalViews.push_back(_pCtx->createTextureView(_handle, spec));
		return TextureView(static_cast<uint32_t>(_additionalViews.size()));
	}

	// ObjectHolder implementations that needed the defined CTX

    template<typename InternalHandle>
    ObjectHolder<InternalHandle>::~ObjectHolder() {
        if (_pCtx) {
            _pCtx->destroy(_handle);
        }
    }
	template<typename InternalHandle>
	void ObjectHolder<InternalHandle>::reset() {
    	_pCtx->destroy(_handle);
    	_pCtx = nullptr;
    	_handle = InternalHandle{};
    }

	template<typename InternalHandle>
	auto ObjectHolder<InternalHandle>::operator->() -> AllocatedType* {
    	return &_pCtx->access(_handle);
    }
	template<typename InternalHandle>
	auto ObjectHolder<InternalHandle>::operator->() const -> const AllocatedType* {
		return &_pCtx->view(_handle);
	}

	template<typename InternalHandle>
	auto ObjectHolder<InternalHandle>::access() -> AllocatedType& {
		return _pCtx->access(_handle);
	}
	template<typename InternalHandle>
	auto ObjectHolder<InternalHandle>::view() const -> const AllocatedType& {
		return _pCtx->view(_handle);
	}



	// define all types of holders so that the definitions can be pre generated
	template class ObjectHolder<TextureHandle>;
	template class ObjectHolder<BufferHandle>;
	template class ObjectHolder<SamplerHandle>;
	template class ObjectHolder<ShaderHandle>;
	template class ObjectHolder<GraphicsPipelineHandle>;
	template class ObjectHolder<ComputePipelineHandle>;
}
