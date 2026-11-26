//
// Created by Hayden Rivas on 1/10/26.
//

#include "mythril/Objects.h"
#include "mythril/CTX.h"
#include "HelperMacros.h"

namespace mythril {

	ShaderStage::ShaderStage(const Shader& shader) :
		handle(shader.handle()) {}

	ShaderStage::ShaderStage(const Shader& shader, const char* entryPoint) :
		handle(shader.handle()),
		entryPoint(entryPoint) {}

	namespace detail {
		template<typename InternalHandle>
		Holder<InternalHandle>::~Holder() {
			if (_pCtx) {
				_pCtx->destroy(_handle);
			}
		}
		template<typename InternalHandle>
		void Holder<InternalHandle>::reset() {
			ASSERT_MSG(_pCtx, "Holder is empty, there is nothing to reset!");
			_pCtx->destroy(_handle);
			_pCtx = nullptr;
			_handle = InternalHandle{};
		}

		// define all types of holders so that the definitions can be pre generated
		template class Holder<TextureHandle>;
		template class Holder<BufferHandle>;
		template class Holder<SamplerHandle>;
		template class Holder<QueryPoolHandle>;
		template class Holder<ShaderHandle>;
		template class Holder<GraphicsPipelineHandle>;
		template class Holder<ComputePipelineHandle>;
		template class Holder<RayTracingPipelineHandle>;
		template class Holder<AccelerationStructureHandle>;
	} // namespace detail
} // namespace mythril
