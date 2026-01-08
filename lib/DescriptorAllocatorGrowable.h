//
// Created by Hayden Rivas on 1/3/26.
//

#pragma once

#include <vector>
#include <span>

#include <volk.h>

namespace mythril {
	class DescriptorAllocatorGrowable {
	public:
		struct PoolSizeRatio {
			VkDescriptorType type;
			float ratio;
		};
		void initialize(VkDevice device, uint32_t initialSets, std::span<PoolSizeRatio> poolRatios);
		void clearPools(VkDevice device);
		void destroyPools(VkDevice device);

		VkDescriptorSet allocateSet(VkDevice device, VkDescriptorSetLayout layout, void* pNext = nullptr);
	private:
		VkDescriptorPool getPoolImpl(VkDevice device);
		VkDescriptorPool createPoolImpl(VkDevice device, uint32_t setCount, std::span<PoolSizeRatio> poolRatios);

		std::vector<PoolSizeRatio> _ratios;
		std::vector<VkDescriptorPool> _fullPools;
		std::vector<VkDescriptorPool> _readyPools;
		uint32_t _setsPerPool;
	};

}
