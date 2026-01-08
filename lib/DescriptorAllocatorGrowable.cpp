//
// Created by Hayden Rivas on 1/3/26.
//

#include "DescriptorAllocatorGrowable.h"

#include "HelperMacros.h"

namespace mythril {
	VkDescriptorPool DescriptorAllocatorGrowable::createPoolImpl(VkDevice device, uint32_t setCount, std::span<PoolSizeRatio> poolRatios) {
		std::vector<VkDescriptorPoolSize> pool_sizes;
		pool_sizes.reserve(this->_ratios.size());
		for (const PoolSizeRatio& ratio : poolRatios) {
			pool_sizes.push_back(VkDescriptorPoolSize{
					.type = ratio.type,
					.descriptorCount = static_cast<uint32_t>(ratio.ratio * (float)setCount)
			});
		}
		VkDescriptorPoolCreateInfo dsp_ci = {};
		dsp_ci.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO;
		dsp_ci.flags = 0;
		dsp_ci.maxSets = setCount;
		dsp_ci.poolSizeCount = static_cast<uint32_t>(pool_sizes.size());
		dsp_ci.pPoolSizes = pool_sizes.data();

		VkDescriptorPool descriptor_pool = VK_NULL_HANDLE;
		vkCreateDescriptorPool(device, &dsp_ci, nullptr, &descriptor_pool);
		return descriptor_pool;
	}

	VkDescriptorPool DescriptorAllocatorGrowable::getPoolImpl(VkDevice device) {
		VkDescriptorPool new_pool = VK_NULL_HANDLE;
		if (!this->_readyPools.empty()) {
			new_pool = this->_readyPools.back();
			_readyPools.pop_back();
		} else {
			new_pool = this->createPoolImpl(device, this->_setsPerPool, this->_ratios);
			this->_setsPerPool *= 2;

			if (this->_setsPerPool > 4092) {
				this->_setsPerPool = 4092;
			}
		}
		ASSERT_MSG(new_pool != VK_NULL_HANDLE, "Cant get a null pool!");
		return new_pool;
	}
	void DescriptorAllocatorGrowable::initialize(VkDevice device, uint32_t initialSets, std::span<PoolSizeRatio> poolRatios) {
		this->_ratios.clear();
		for (auto& ratio : poolRatios) {
			this->_ratios.push_back(ratio);
		}
		VkDescriptorPool new_pool = this->createPoolImpl(device, initialSets, poolRatios);
		this->_setsPerPool = initialSets * 2;
		this->_readyPools.push_back(new_pool);
	}
	void DescriptorAllocatorGrowable::clearPools(VkDevice device) {
		for (auto ready_pool : this->_readyPools) {
			vkResetDescriptorPool(device, ready_pool, 0);
		}
		for (auto full_pool : this->_fullPools) {
			vkResetDescriptorPool(device, full_pool, 0);
			_readyPools.push_back(full_pool);
		}
		this->_fullPools.clear();
	}
	void DescriptorAllocatorGrowable::destroyPools(VkDevice device) {
		for (auto ready_pool : this->_readyPools) {
			vkDestroyDescriptorPool(device, ready_pool, nullptr);
		}
		this->_readyPools.clear();
		for (auto full_pool : this->_fullPools) {
			vkDestroyDescriptorPool(device, full_pool, nullptr);
		}
		this->_fullPools.clear();
	}
	VkDescriptorSet DescriptorAllocatorGrowable::allocateSet(VkDevice device, VkDescriptorSetLayout layout, void* pNext) {
		VkDescriptorPool pool_to_use = this->getPoolImpl(device);

		VkDescriptorSetAllocateInfo ds_ai = {
				.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO,
				.pNext = pNext,
				.descriptorPool = pool_to_use,
				.descriptorSetCount = 1,
				.pSetLayouts = &layout
		};
		VkDescriptorSet descriptor_set = VK_NULL_HANDLE;
		VkResult result = vkAllocateDescriptorSets(device, &ds_ai, &descriptor_set);
		if (result == VK_ERROR_OUT_OF_POOL_MEMORY || result == VK_ERROR_FRAGMENTED_POOL) {
			_fullPools.push_back(pool_to_use);
			pool_to_use = this->getPoolImpl(device);
			ds_ai.descriptorPool = pool_to_use;
			VK_CHECK(vkAllocateDescriptorSets(device, &ds_ai, &descriptor_set));
		}
		this->_readyPools.push_back(pool_to_use);
		return descriptor_set;
	}
}