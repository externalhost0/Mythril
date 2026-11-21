#pragma once

#include "mythril/CTX.h"

namespace mythril {
	template<typename Pool, typename Handle>
	auto& PoolEntryOrAssert(Pool& pool, Handle handle) {
		auto* ptr = pool.get(handle);
		ASSERT_MSG(ptr, "Invalid handle!");
		return *ptr;
	}

	inline const detail::AllocatedTexture& CTX::view(TextureHandle h) const { return PoolEntryOrAssert(_texturePool, h); }
	inline const detail::AllocatedBuffer& CTX::view(BufferHandle h) const { return PoolEntryOrAssert(_bufferPool, h); }
	inline const detail::AllocatedSampler& CTX::view(SamplerHandle h) const { return PoolEntryOrAssert(_samplerPool, h); }
	inline const detail::AllocatedQueryPool& CTX::view(QueryPoolHandle h) const { return PoolEntryOrAssert(_queryPoolPool, h); }
	inline const detail::AllocatedShader& CTX::view(ShaderHandle h) const { return PoolEntryOrAssert(_shaderPool, h); }
	inline const detail::AllocatedGraphicsPipeline& CTX::view(GraphicsPipelineHandle h) const { return PoolEntryOrAssert(_graphicsPipelinePool, h); }
	inline const detail::AllocatedComputePipeline& CTX::view(ComputePipelineHandle h) const { return PoolEntryOrAssert(_computePipelinePool, h); }
	inline const detail::AllocatedRayTracingPipeline& CTX::view(RayTracingPipelineHandle h) const { return PoolEntryOrAssert(_rayTracingPipelinePool, h); }

	inline detail::AllocatedTexture& CTX::access(TextureHandle h) { return PoolEntryOrAssert(_texturePool, h); }
	inline detail::AllocatedBuffer& CTX::access(BufferHandle h) { return PoolEntryOrAssert(_bufferPool, h); }
	inline detail::AllocatedSampler& CTX::access(SamplerHandle h) { return PoolEntryOrAssert(_samplerPool, h); }
	inline detail::AllocatedQueryPool& CTX::access(QueryPoolHandle h) { return PoolEntryOrAssert(_queryPoolPool, h); }
	inline detail::AllocatedShader& CTX::access(ShaderHandle h) { return PoolEntryOrAssert(_shaderPool, h); }
	inline detail::AllocatedGraphicsPipeline& CTX::access(GraphicsPipelineHandle h) { return PoolEntryOrAssert(_graphicsPipelinePool, h); }
	inline detail::AllocatedComputePipeline& CTX::access(ComputePipelineHandle h) { return PoolEntryOrAssert(_computePipelinePool, h); }
	inline detail::AllocatedRayTracingPipeline& CTX::access(RayTracingPipelineHandle h) { return PoolEntryOrAssert(_rayTracingPipelinePool, h); }
} // namespace mythril
