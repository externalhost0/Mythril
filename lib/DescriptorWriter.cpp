//
// Created by Hayden Rivas on 12/9/25.
//

#include "HelperMacros.h"
#include "CTX.h"

namespace mythril {
	ShaderStage::ShaderStage(const Shader& shader) : handle(shader.handle()) {}

	void DescriptorSetWriter::updateBinding(const Buffer& buffer, const char* name) {
		updateBinding(buffer.handle(), name);
	}
	void DescriptorSetWriter::updateBinding(const Buffer& buffer, int set, int binding) {
		updateBinding(buffer.handle(), set, binding);
	}

	static void CheckUpdateBindingCall(const PipelineCoreData* pipeline, BufferHandle bufHandle, const char* debugName) {
		ASSERT_MSG(pipeline, "You must call updateBinding within opening and submitting a DescriptorSetWriter!");
		ASSERT_MSG(pipeline->_vkPipeline != VK_NULL_HANDLE, "Pipeline '{}' has not yet been resolved, pipeline was probably not included when compiling RenderGraph!", debugName);
		ASSERT_MSG(bufHandle.valid(), "Handle must be for a valid buffer object!");
	}

	// less performant than just calling it by set and bind index but just way better for ease of use
	// + lookup in unordered_map
	// + repeated memory access for common data
	void DescriptorSetWriter::updateBinding(mythril::BufferHandle bufHandle, const char* name) {
		const PipelineCoreData* common = this->currentPipelineCommon;
		CheckUpdateBindingCall(common, bufHandle, this->currentPipelineDebugName);

		for (size_t i = 0; i < common->signature.setSignatures.size(); i++) {
			const DescriptorSetSignature& set_signature = common->signature.setSignatures[i];
			auto it = set_signature.nameToBinding.find(name);
			if (it != set_signature.nameToBinding.end()) {
				updateBinding(bufHandle, static_cast<int>(i), static_cast<int>(it->second));
				return;
			}
		}
		ASSERT_MSG(false, "Variable name '{}' could not be found in pipeline '{}'!", name, this->currentPipelineDebugName);
	}

	void DescriptorSetWriter::updateBinding(BufferHandle bufHandle, int set, int binding) {
		const PipelineCoreData* common = this->currentPipelineCommon;
		CheckUpdateBindingCall(common, bufHandle, this->currentPipelineDebugName);

		AllocatedBuffer* buf = _ctx->_bufferPool.get(bufHandle);
		ASSERT_MSG(buf->isUniformBuffer(), "Buffer passed to be written to descriptor binding must be uniform, aka uses 'BufferUsageBits_Uniform'!");
		ASSERT_MSG(buf->_bufferSize > 0, "Buffer size must be greater than 0!");

		const DescriptorSetSignature& set_signature = common->signature.setSignatures[set];
		VkDescriptorSet vkset = common->_managedDescriptorSets[set].vkDescriptorSet;
		ASSERT_MSG(vkset != VK_NULL_HANDLE, "VkDescriptorSet gathered is NULL!");
		this->writer.writeBuffer(vkset, binding, buf->_vkBuffer, buf->_bufferSize, 0, set_signature.bindings[binding].descriptorType);
	}

	void DWriter::writeBuffer(VkDescriptorSet set, unsigned int binding, VkBuffer buffer, size_t size, size_t offset, VkDescriptorType type) {
		VkDescriptorBufferInfo& info = this->_bufferInfos.emplace_back(VkDescriptorBufferInfo{
				.buffer = buffer,
				.offset = offset,
				.range = size
		});

		VkWriteDescriptorSet write = {
				.sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET,
				.pNext = nullptr,
				.dstSet = set,
				.dstBinding = static_cast<uint32_t>(binding),
				.descriptorCount = 1,
				.descriptorType = type,
				.pBufferInfo = &info
		};
		this->_writes.push_back(write);
	}
	void DWriter::updateSets(VkDevice device) {
		vkUpdateDescriptorSets(device, static_cast<uint32_t>(_writes.size()), _writes.data(), 0, nullptr);
	}
}