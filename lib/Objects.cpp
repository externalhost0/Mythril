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

	Texture::~Texture() {
		if (_pCtx) {
			for (const auto& view: _additionalViews) {
				_pCtx->destroy(view.second);
			}
			_pCtx->destroy(_handle);
		}
		_additionalViews.clear();
	}

	Texture& Texture::operator=(Texture&& other) noexcept {
		if (this != &other) {
			if (_pCtx) {
				for (const auto& view : _additionalViews) {
					_pCtx->destroy(view.second);
				}
			}
			_additionalViews.clear();
			ObjectHolder::operator=(std::move(other));
			_additionalViews = std::move(other._additionalViews);
		}
		return *this;
	}

	uint32_t Texture::index(ViewKey key) const {
        auto it = _additionalViews.find(key);
        ASSERT_MSG(it != _additionalViews.end(), "ViewKey not found in additional views!");
        return it->second.index();
	}
	TextureHandle Texture::handle(ViewKey key) const {
		auto it = _additionalViews.find(key);
		ASSERT_MSG(it != _additionalViews.end(), "ViewKey not found in additional views!");
		return it->second;
	}

	void Texture::resize(const Dimensions newDimensions) {
		_pCtx->resizeTexture(_handle, newDimensions);
	}

	Texture::ViewKey Texture::createView(const TextureViewSpec& spec) {
		const uint32_t baseMip = spec.mipLevel;
		const uint32_t numMips = spec.numMipLevels;
		const uint32_t baseLayer = spec.layer;
		const uint32_t numLayers = spec.numLayers;

		ASSERT(_pCtx);
		const uint64_t key = packViewKey(baseMip, numMips, baseLayer, numLayers);
		auto it = _additionalViews.find(key);
		if (it != _additionalViews.end()) {
			return key;
		}

		// if not make a new texture for the view
		const AllocatedTexture& texture = view();
		VkImageViewType viewType = spec.type != VK_IMAGE_VIEW_TYPE_MAX_ENUM ? spec.type : texture.getViewType();
		if (numLayers == 1 && viewType == VK_IMAGE_VIEW_TYPE_CUBE) {
			viewType = VK_IMAGE_VIEW_TYPE_2D;
		}

		char data[kMaxDebugNameLength];
		snprintf(data, sizeof(data), "%s - View (Mip: %d, NumMips: %d, Layer: %d, NumLayers: %d)",
			texture.getDebugName().data(),
			baseMip,
			numMips,
			baseLayer,
			numLayers
			);
		const TextureHandle newView = _pCtx->createTextureView(_handle, {
			.type = viewType,
			.layer = baseLayer,
			.numLayers = numLayers,
			.mipLevel = baseMip,
			.numMipLevels = numMips,
			.debugName = data
		});
		_additionalViews.emplace(key, newView);
		return key;
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
