//
// Created by Hayden Rivas on 12/9/25.
//

#pragma once

#include "ObjectHandles.h"
#include "Pipelines.h"

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
		void updateBinding(const Buffer& buffer, const char* name) { updateBinding(buffer.handle(), name); }
		void updateBinding(const Buffer& buffer, int set, int binding) { updateBinding(buffer.handle(), set, binding); }

	private:
		void updateBinding(BufferHandle bufHandle, const char *name);
		void updateBinding(BufferHandle bufHandle, int set, int binding);
		explicit DescriptorSetWriter(CTX& ctx) : _ctx(&ctx) {};

		DWriter writer = {};
		PipelineCoreData* currentPipelineCommon = nullptr;
		const char* currentPipelineDebugName = nullptr;
		CTX* _ctx = nullptr;

		friend class CTX;
	};
}
