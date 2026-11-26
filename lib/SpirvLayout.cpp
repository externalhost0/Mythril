#include "SpirvLayout.h"

#include <fmt/format.h>
#include <vulkan/vk_enum_string_helper.h>

#include <algorithm>

namespace mythril {
	SpirvLayout::SpirvLayout(std::vector<uint32_t> spirv) : _words(std::move(spirv)) {
		const std::span<const uint32_t> words = _words;
		// ids may have gaps, so the bound only has the spec's universal limit
		constexpr uint32_t kMaxIdBound = 4'194'303;
		valid = words.size() >= 5 && words[0] == SpvMagicNumber && words[3] <= kMaxIdBound;
		if (!valid)
			return;
		bound = words[3];
		types.resize(bound);
		for (size_t i = 5; i < words.size();) {
			const uint32_t count = words[i] >> 16;
			if (count == 0 || i + count > words.size()) {
				valid = false;
				return;
			}
			const std::span<const uint32_t> w = words.subspan(i, count);
			i += count;
			instructions.push_back(w);
			switch (static_cast<SpvOp>(w[0] & 0xffff)) {
				case SpvOpName:
					if (count >= 3)
						names[w[1]] = reinterpret_cast<const char*>(&w[2]);
					break;
				case SpvOpMemberName:
					if (count >= 4)
						memberNames[(uint64_t{w[1]} << 32) | w[2]] = reinterpret_cast<const char*>(&w[3]);
					break;
				case SpvOpEntryPoint:
					if (count >= 4)
						entryPoints.push_back({static_cast<SpvExecutionModel>(w[1]), reinterpret_cast<const char*>(&w[3])});
					break;
				case SpvOpDecorate:
					if (count < 4)
						break;
					if (w[2] == SpvDecorationArrayStride)
						arrayStrides[w[1]] = w[3];
					else if (w[2] == SpvDecorationDescriptorSet)
						descriptorSets[w[1]] = w[3];
					else if (w[2] == SpvDecorationBinding)
						bindings[w[1]] = w[3];
					else if (w[2] == SpvDecorationSpecId)
						specIds.push_back({w[1], w[3]});
					break;
				case SpvOpMemberDecorate:
					if (count >= 5 && w[3] == SpvDecorationOffset)
						memberOffsets[(uint64_t{w[1]} << 32) | w[2]] = w[4];
					break;
				case SpvOpTypeBool:
				case SpvOpTypeInt:
				case SpvOpTypeFloat:
				case SpvOpTypeVector:
				case SpvOpTypeMatrix:
				case SpvOpTypeArray:
				case SpvOpTypeRuntimeArray:
				case SpvOpTypePointer:
				case SpvOpTypeStruct:
					if (count >= 2 && w[1] < bound)
						types[w[1]] = {static_cast<SpvOp>(w[0] & 0xffff), {w.begin() + 2, w.end()}};
					break;
				case SpvOpConstant:
					if (count >= 4)
						constants[w[2]] = w[3];
					break;
				case SpvOpSpecConstantTrue:
				case SpvOpSpecConstantFalse:
				case SpvOpSpecConstant:
					if (count >= 3)
						specConstantTypes[w[2]] = w[1];
					break;
				case SpvOpVariable:
					if (count < 4)
						break;
					if (w[3] == SpvStorageClassPushConstant)
						pushVariables.push_back({w[2], w[1]});
					else if (w[3] == SpvStorageClassUniform || w[3] == SpvStorageClassUniformConstant || w[3] == SpvStorageClassStorageBuffer)
						resourceVariables.push_back({w[2], w[1]});
					break;
				default:
					break;
			}
		}
	}

	const SpirvLayout::Type& SpirvLayout::type(const uint32_t id) const {
		static const Type kNone = {};
		return id < types.size() ? types[id] : kNone;
	}
	uint32_t SpirvLayout::arg(const uint32_t id, const size_t i) const {
		const std::vector<uint32_t>& args = type(id).args;
		return i < args.size() ? args[i] : 0;
	}
	std::optional<uint32_t> SpirvLayout::offset(const uint32_t structId, const uint32_t member) const {
		const auto it = memberOffsets.find((uint64_t{structId} << 32) | member);
		return it == memberOffsets.end() ? std::nullopt : std::optional(it->second);
	}
	std::optional<uint32_t> SpirvLayout::stride(const uint32_t id) const {
		const auto it = arrayStrides.find(id);
		return it == arrayStrides.end() ? std::nullopt : std::optional(it->second);
	}
	std::optional<uint32_t> SpirvLayout::constant(const uint32_t id) const {
		const auto it = constants.find(id);
		return it == constants.end() ? std::nullopt : std::optional(it->second);
	}
	std::string_view SpirvLayout::memberName(const uint32_t structId, const uint32_t member) const {
		const auto it = memberNames.find((uint64_t{structId} << 32) | member);
		return it == memberNames.end() ? std::string_view{} : std::string_view(it->second);
	}
	std::string_view SpirvLayout::name(const uint32_t id) const {
		const auto it = names.find(id);
		return it == names.end() ? std::string_view{} : std::string_view(it->second);
	}
	uint32_t SpirvLayout::pointee(const uint32_t pointerType) const {
		const Type& t = type(pointerType);
		return t.op == SpvOpTypePointer && t.args.size() == 2 ? t.args[1] : 0;
	}
	uint32_t SpirvLayout::size(const uint32_t id) const {
		const Type& t = type(id);
		const auto arg = [&](const size_t i) { return this->arg(id, i); };
		switch (t.op) {
			case SpvOpTypeBool: return 4;
			case SpvOpTypeInt:
			case SpvOpTypeFloat: return arg(0) / 8;
			case SpvOpTypeVector:
			case SpvOpTypeMatrix: return size(arg(0)) * arg(1);
			// scalar layout packs elements, so a missing ArrayStride means the element size
			case SpvOpTypeArray: return constant(arg(1)).value_or(0) * stride(id).value_or(size(arg(0)));
			case SpvOpTypePointer: return 8;
			case SpvOpTypeStruct: {
				uint32_t end = 0;
				for (uint32_t m = 0; m < t.args.size(); ++m)
					end = std::max(end, offset(id, m).value_or(end) + size(t.args[m]));
				return end;
			}
			default: return 0;
		}
	}
	ScalarKind SpirvLayout::scalarKind(const uint32_t id) const {
		const Type& t = type(id);
		switch (t.op) {
			case SpvOpTypeBool: return ScalarKind::Bool;
			case SpvOpTypeInt: return t.args.size() > 1 && t.args[1] == 1 ? ScalarKind::Int : ScalarKind::UInt;
			case SpvOpTypeFloat: return !t.args.empty() && t.args[0] == 64 ? ScalarKind::Double : ScalarKind::Float;
			default: return ScalarKind::None;
		}
	}
	bool SpirvLayout::isMatrixStorage(const uint32_t id) const { return type(id).op == SpvOpTypeStruct && name(id).starts_with("_MatrixStorage_"); }
	bool SpirvLayout::isColumnMajorStorage(const uint32_t id) const { return isMatrixStorage(id) && name(id).find("_ColMajor") != std::string_view::npos; }
	bool SpirvLayout::isArrayWrapper(const uint32_t id) const { return type(id).op == SpvOpTypeStruct && type(id).args.size() == 1 && name(id).starts_with("_Array_"); }
	uint32_t SpirvLayout::unwrapArray(const uint32_t id) const {
		const Type& t = type(id);
		if (t.op == SpvOpTypeStruct && t.args.size() == 1 && type(t.args[0]).op == SpvOpTypeArray && offset(id, 0).value_or(0) == 0)
			return t.args[0];
		return id;
	}

	static uint64_t HashLayout(const std::string_view description) {
		uint64_t hash = 0xcbf29ce484222325ull;
		for (const char c: description)
			hash = (hash ^ static_cast<uint8_t>(c)) * 0x100000001b3ull;
		return hash;
	}

	// struct type names are left out, Slang renames them per layout
	static std::string DescribeSpirvType(const SpirvLayout& layout, const uint32_t id, std::vector<uint32_t>& structs) {
		const SpirvLayout::Type& t = layout.type(id);
		const auto arg = [&](const size_t i) { return layout.arg(id, i); };
		switch (t.op) {
			case SpvOpTypeBool: return "bool";
			case SpvOpTypeInt: return fmt::format("{}{}", arg(1) ? "i" : "u", arg(0));
			case SpvOpTypeFloat: return fmt::format("f{}", arg(0));
			case SpvOpTypeVector: return fmt::format("{}x{}", DescribeSpirvType(layout, arg(0), structs), arg(1));
			case SpvOpTypeMatrix: return fmt::format("{}m{}", DescribeSpirvType(layout, arg(0), structs), arg(1));
			case SpvOpTypeArray: {
				return fmt::format("{}[{}/{}]", DescribeSpirvType(layout, arg(0), structs), layout.constant(arg(1)).value_or(0), layout.stride(id).value_or(0));
			}
			case SpvOpTypeRuntimeArray: return fmt::format("{}[/{}]", DescribeSpirvType(layout, arg(0), structs), layout.stride(id).value_or(0));
			case SpvOpTypePointer:
				if (arg(0) != SpvStorageClassPhysicalStorageBuffer)
					return "?pointer";
				return fmt::format("*{}:{}", layout.stride(id).value_or(0), DescribeSpirvType(layout, arg(1), structs));
			case SpvOpTypeStruct: {
				if (const auto it = std::ranges::find(structs, id); it != structs.end())
					return fmt::format("^{}", it - structs.begin());
				structs.push_back(id);
				std::string text = "{";
				for (uint32_t m = 0; m < t.args.size(); ++m)
					text += fmt::format("{}@{}:{};", layout.memberName(id, m), layout.offset(id, m).value_or(0), DescribeSpirvType(layout, t.args[m], structs));
				structs.pop_back();
				// Slang lowers a matrix to a struct of vectors and only the struct name says if it is column-major
				if (layout.isMatrixStorage(id))
					text.insert(0, layout.isColumnMajorStorage(id) ? "col" : "row");
				return text + "}";
			}
			default: return "?";
		}
	}

	namespace {
		class PushFieldBuilder {
		public:
			PushFieldBuilder(const SpirvLayout& layout, std::vector<PushField>& pointees) : _layout(layout), _pointees(pointees) {}

			PushField build(const uint32_t id, std::string name, const uint32_t offset) {
				PushField field;
				field.name = std::move(name);
				field.offset = offset;
				field.size = _layout.size(id);
				fill(field, id);
				return field;
			}

		private:
			// Slang names a struct after its layout, Vertex_c or Vertex_c_logical_0
			static std::string StructName(std::string_view name) {
				if (const size_t end = name.find_last_not_of("0123456789"); end != std::string_view::npos && end + 1 < name.size() && name[end] == '_')
					name = name.substr(0, end);
				for (const std::string_view suffix: {"_c_logical", "_logical", "_c"})
					if (name.ends_with(suffix) && name.size() > suffix.size())
						return std::string(name.substr(0, name.size() - suffix.size()));
				return std::string(name);
			}
			void components(PushField& field, const uint32_t scalar) const {
				const SpirvLayout::Type& t = _layout.type(scalar);
				field.scalar = _layout.scalarKind(scalar);
				field.bits = static_cast<uint8_t>(t.op == SpvOpTypeBool || t.args.empty() ? 32 : t.args[0]);
			}
			void fill(PushField& field, const uint32_t id) {
				using Kind = PushField::Kind;
				const SpirvLayout::Type& t = _layout.type(id);
				const auto arg = [&](const size_t i) { return _layout.arg(id, i); };
				switch (t.op) {
					case SpvOpTypeBool:
					case SpvOpTypeInt:
					case SpvOpTypeFloat:
						field.kind = Kind::Scalar;
						components(field, id);
						return;
					case SpvOpTypeVector:
						field.kind = Kind::Vector;
						field.columns = static_cast<uint8_t>(arg(1));
						components(field, arg(0));
						return;
					case SpvOpTypeMatrix:
						field.kind = Kind::Matrix;
						field.columns = static_cast<uint8_t>(arg(1));
						field.rows = static_cast<uint8_t>(_layout.arg(arg(0), 1));
						components(field, _layout.arg(arg(0), 0));
						return;
					case SpvOpTypeArray:
					case SpvOpTypeRuntimeArray:
						field.kind = Kind::Array;
						field.count = t.op == SpvOpTypeArray ? _layout.constant(arg(1)).value_or(0) : 0;
						field.stride = _layout.stride(id).value_or(_layout.size(arg(0)));
						field.fields.push_back(build(arg(0), "", field.offset));
						return;
					case SpvOpTypePointer:
						if (arg(0) != SpvStorageClassPhysicalStorageBuffer)
							return;
						field.kind = Kind::Pointer;
						field.stride = _layout.stride(id).value_or(_layout.size(arg(1)));
						field.pointee = pointee(arg(1));
						return;
					case SpvOpTypeStruct:
						break;
					default:
						return;
				}
				const std::string_view name = _layout.name(id);
				if (_layout.isMatrixStorage(id)) {
					// a row-major matrix is stored as one vector per row
					const uint32_t array = arg(0);
					const uint32_t vector = _layout.arg(array, 0);
					const auto vectors = static_cast<uint8_t>(_layout.constant(_layout.arg(array, 1)).value_or(0));
					const auto perVector = static_cast<uint8_t>(_layout.arg(vector, 1));
					field.kind = Kind::Matrix;
					field.rowMajor = !_layout.isColumnMajorStorage(id);
					field.rows = field.rowMajor ? vectors : perVector;
					field.columns = field.rowMajor ? perVector : vectors;
					components(field, _layout.arg(vector, 0));
					return;
				}
				if (_layout.isArrayWrapper(id)) {
					fill(field, t.args[0]);
					return;
				}
				field.kind = Kind::Struct;
				field.typeName = StructName(name);
				for (uint32_t m = 0; m < t.args.size(); ++m)
					field.fields.push_back(build(t.args[m], std::string(_layout.memberName(id, m)), field.offset + _layout.offset(id, m).value_or(0)));
			}
			uint32_t pointee(const uint32_t id) {
				if (const auto it = _pointeeIndex.find(id); it != _pointeeIndex.end())
					return it->second;
				const auto index = static_cast<uint32_t>(_pointees.size());
				_pointeeIndex.emplace(id, index);
				_pointees.emplace_back();
				PushField field = build(id, "", 0);
				_pointees[index] = std::move(field);
				return index;
			}

			const SpirvLayout& _layout;
			std::vector<PushField>& _pointees;
			std::unordered_map<uint32_t, uint32_t> _pointeeIndex;
		};
	} // namespace

	std::vector<SpirvPushBlock> DescribeSpirvPushBlocks(const SpirvLayout& layout) {
		std::vector<SpirvPushBlock> out;
		if (!layout.valid)
			return out;
		for (const SpirvLayout::Variable& variable: layout.pushVariables) {
			const uint32_t block = layout.pointee(variable.pointerType);
			if (!block)
				continue;
			std::vector<uint32_t> structs;
			std::string description = DescribeSpirvType(layout, block, structs);
			PushBlockLayout push = {.variable = std::string(layout.name(variable.id)), .hash = HashLayout(description), .description = std::move(description)};
			push.root = PushFieldBuilder(layout, push.pointees).build(block, push.variable, 0);
			out.push_back({block, std::move(push)});
		}
		return out;
	}

	VkShaderStageFlagBits ToShaderStage(const SpvExecutionModel model) {
		switch (model) {
			case SpvExecutionModelVertex: return VK_SHADER_STAGE_VERTEX_BIT;
			case SpvExecutionModelTessellationControl: return VK_SHADER_STAGE_TESSELLATION_CONTROL_BIT;
			case SpvExecutionModelTessellationEvaluation: return VK_SHADER_STAGE_TESSELLATION_EVALUATION_BIT;
			case SpvExecutionModelGeometry: return VK_SHADER_STAGE_GEOMETRY_BIT;
			case SpvExecutionModelFragment: return VK_SHADER_STAGE_FRAGMENT_BIT;
			case SpvExecutionModelGLCompute: return VK_SHADER_STAGE_COMPUTE_BIT;
			case SpvExecutionModelTaskNV: return VK_SHADER_STAGE_TASK_BIT_NV;
			case SpvExecutionModelMeshNV: return VK_SHADER_STAGE_MESH_BIT_NV;
			case SpvExecutionModelTaskEXT: return VK_SHADER_STAGE_TASK_BIT_EXT;
			case SpvExecutionModelMeshEXT: return VK_SHADER_STAGE_MESH_BIT_EXT;
			case SpvExecutionModelRayGenerationKHR: return VK_SHADER_STAGE_RAYGEN_BIT_KHR;
			case SpvExecutionModelIntersectionKHR: return VK_SHADER_STAGE_INTERSECTION_BIT_KHR;
			case SpvExecutionModelAnyHitKHR: return VK_SHADER_STAGE_ANY_HIT_BIT_KHR;
			case SpvExecutionModelClosestHitKHR: return VK_SHADER_STAGE_CLOSEST_HIT_BIT_KHR;
			case SpvExecutionModelMissKHR: return VK_SHADER_STAGE_MISS_BIT_KHR;
			case SpvExecutionModelCallableKHR: return VK_SHADER_STAGE_CALLABLE_BIT_KHR;
			default: return static_cast<VkShaderStageFlagBits>(0);
		}
	}

	std::string CheckSPIRVRules(const SpirvLayout& layout) {
		if (!layout.valid)
			return "It is not valid SPIR-V.";
		// a raytracing pipeline picks its shaders by entry point, so several per stage is expected
		constexpr VkShaderStageFlags kRayTracingStages = VK_SHADER_STAGE_RAYGEN_BIT_KHR | VK_SHADER_STAGE_ANY_HIT_BIT_KHR | VK_SHADER_STAGE_CLOSEST_HIT_BIT_KHR | VK_SHADER_STAGE_MISS_BIT_KHR |
		                                                 VK_SHADER_STAGE_INTERSECTION_BIT_KHR | VK_SHADER_STAGE_CALLABLE_BIT_KHR;
		VkShaderStageFlags seenStages = 0;
		for (const SpirvLayout::EntryPoint& entryPoint: layout.entryPoints) {
			const VkShaderStageFlagBits stage = ToShaderStage(entryPoint.model);
			if (stage & kRayTracingStages)
				continue;
			if (seenStages & stage)
				return fmt::format("You cannot have more than 1 entrypoint for the same shader stage! '{}' is the second entrypoint that targets '{}'", entryPoint.name, string_VkShaderStageFlagBits(stage));
			seenStages |= stage;
		}
		struct Binding {
			uint32_t set;
			uint32_t binding;
			uint32_t id;
			auto operator<=>(const Binding&) const = default;
		};
		std::vector<Binding> bindings;
		for (const SpirvLayout::Variable& variable: layout.resourceVariables) {
			const auto set = layout.descriptorSets.find(variable.id);
			const auto binding = layout.bindings.find(variable.id);
			if (set != layout.descriptorSets.end() && binding != layout.bindings.end())
				bindings.push_back({set->second, binding->second, variable.id});
		}
		std::ranges::sort(bindings);
		for (const Binding& binding: bindings) {
			if (layout.name(binding.id) != "__slang_resource_heap")
				return fmt::format("Shader declares non-bindless descriptor binding '{}' at set={}, binding={}. Mythril is fully bindless: use Ptr<T> in push constants or DescriptorHandle<T>.", layout.name(binding.id), binding.set, binding.binding);
			if (binding.set != 0)
				return fmt::format("Slang put the bindless heap __slang_resource_heap in descriptor set={}, Mythril binds it at set 0. Keep the BindlessSpaceIndex compiler option at 0.", binding.set);
		}
		return {};
	}
} // namespace mythril
