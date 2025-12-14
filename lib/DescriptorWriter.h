//
// Created by Hayden Rivas on 12/9/25.
//

#pragma once

#include "ObjectHandles.h"
#include "GraphicsPipeline.h"

#include <volk.h>
#include <deque>
#include <vector>

namespace mythril {
	class CTX;

	class DWriter {
	public:
		void writeBuffer(VkDescriptorSet set, unsigned int binding, VkBuffer buffer, size_t size, size_t offset, VkDescriptorType type);
		void updateSets(VkDevice device);
		void clear() {
			this->_writes.clear();
			this->_bufferInfos.clear();
		};
	private:
		std::deque<VkDescriptorBufferInfo> _bufferInfos;
		std::vector<VkWriteDescriptorSet> _writes;
	};

	class DescriptorSetWriter {
	public:
		void updateBinding(InternalBufferHandle bufHandle, const char* name);
		void updateBinding(InternalBufferHandle bufHandle, int set, int binding);
	private:
		explicit DescriptorSetWriter(CTX& ctx) : _ctx(&ctx) {};

		DWriter writer = {};
		PipelineCommon* currentPipelineCommon = nullptr;
		const char* currentPipelineDebugName = nullptr;
		CTX* _ctx = nullptr;

		friend class CTX;
	};
}
