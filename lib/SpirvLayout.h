#pragma once

#include "mythril/Shader.h"

#include <spirv/unified1/spirv.h>

#include <cstdint>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <unordered_map>
#include <vector>

namespace mythril {
	class SpirvLayout {
	public:
		explicit SpirvLayout(std::vector<uint32_t> words);
		SpirvLayout(const SpirvLayout&) = delete;
		SpirvLayout& operator=(const SpirvLayout&) = delete;
		SpirvLayout(SpirvLayout&&) = default;
		SpirvLayout& operator=(SpirvLayout&&) = default;

		std::span<const uint32_t> words() const { return _words; }

		struct Type {
			SpvOp op = SpvOpNop;
			std::vector<uint32_t> args;
		};
		struct Variable {
			uint32_t id;
			uint32_t pointerType;
		};
		struct EntryPoint {
			SpvExecutionModel model;
			std::string name;
		};
		struct SpecConstant {
			uint32_t id;
			uint32_t specId;
		};
		const Type& type(uint32_t id) const;
		uint32_t arg(uint32_t id, size_t i) const;
		std::optional<uint32_t> offset(uint32_t structId, uint32_t member) const;
		std::optional<uint32_t> stride(uint32_t id) const;
		std::optional<uint32_t> constant(uint32_t id) const;
		std::string_view memberName(uint32_t structId, uint32_t member) const;
		std::string_view name(uint32_t id) const;
		uint32_t pointee(uint32_t pointerType) const;
		uint32_t size(uint32_t id) const;
		ScalarKind scalarKind(uint32_t id) const;
		// Slang lowers a matrix to a struct of vectors whose name holds the major order
		bool isMatrixStorage(uint32_t id) const;
		bool isColumnMajorStorage(uint32_t id) const;
		// Slang wraps an array field in a struct of one member named _Array_...
		bool isArrayWrapper(uint32_t id) const;
		// Slang wraps arrays and buffer matrices in a struct with one array member
		uint32_t unwrapArray(uint32_t id) const;

		bool valid = false;
		uint32_t bound = 0;
		std::vector<std::span<const uint32_t>> instructions;
		std::vector<Type> types;
		std::unordered_map<uint32_t, std::string> names;
		std::unordered_map<uint64_t, std::string> memberNames;
		std::unordered_map<uint64_t, uint32_t> memberOffsets;
		std::unordered_map<uint32_t, uint32_t> arrayStrides;
		std::unordered_map<uint32_t, uint32_t> constants;
		std::vector<Variable> pushVariables;
		std::vector<Variable> resourceVariables;
		std::vector<EntryPoint> entryPoints;
		std::unordered_map<uint32_t, uint32_t> descriptorSets;
		std::unordered_map<uint32_t, uint32_t> bindings;
		std::vector<SpecConstant> specIds;
		std::unordered_map<uint32_t, uint32_t> specConstantTypes;

	private:
		std::vector<uint32_t> _words;
	};

	struct SpirvPushBlock {
		uint32_t type{};
		PushBlockLayout layout;
	};
	std::vector<SpirvPushBlock> DescribeSpirvPushBlocks(const SpirvLayout& layout);
	VkShaderStageFlagBits ToShaderStage(SpvExecutionModel model);
	std::string CheckSPIRVRules(const SpirvLayout& layout);
} // namespace mythril
