//
// Created by Hayden Rivas on 10/6/25.
//

#pragma once

#include "HelperMacros.h"

#include <cstdint>

namespace mythril {
	struct SubmitHandle {
		SubmitHandle() = default;
		explicit SubmitHandle(uint64_t handle) :
		    bufferIndex_(static_cast<uint32_t>(handle & 0xffffffff)),
		    submitId_(static_cast<uint32_t>(handle >> 32)) {
			ASSERT_MSG(submitId_, "Submit handle is invalid!");
		}

		uint32_t bufferIndex_ = 0;
		uint32_t submitId_ = 0;
		bool empty() const { return submitId_ == 0; }
		uint64_t handle() const { return (static_cast<uint64_t>(submitId_) << 32) + bufferIndex_; }
	};
} // namespace mythril
