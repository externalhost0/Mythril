#include "PushLayout.h"
#include "SpirvLayout.h"

#include <slang/slang-com-ptr.h>
#include <spirv/unified1/spirv.h>

#include <fmt/format.h>
#include <fmt/ranges.h>

#include <algorithm>
#include <cstring>
#include <optional>
#include <set>
#include <unordered_set>

namespace mythril {
	struct ScalarInfo {
		const char* slang;
		// nullptr when C++ has no matching type
		const char* cpp;
		uint32_t bits;
	};
	static ScalarInfo Scalar(const slang::TypeReflection::ScalarType scalar) {
		using S = slang::TypeReflection::ScalarType;
		switch (scalar) {
			case S::Bool: return {"bool", nullptr, 8};
			case S::Int8: return {"int8_t", "int8_t", 8};
			case S::UInt8: return {"uint8_t", "uint8_t", 8};
			case S::Int16: return {"int16_t", "int16_t", 16};
			case S::UInt16: return {"uint16_t", "uint16_t", 16};
			case S::Float16: return {"half", "uint16_t", 16};
			case S::Int32: return {"int", "int32_t", 32};
			case S::UInt32: return {"uint", "uint32_t", 32};
			case S::Int64: return {"int64_t", "int64_t", 64};
			case S::UInt64: return {"uint64_t", "uint64_t", 64};
			case S::Float32: return {"float", "float", 32};
			case S::Float64: return {"double", "double", 64};
			default: return {"?", nullptr, 32};
		}
	}

	static std::string FullName(slang::TypeReflection* type) {
		Slang::ComPtr<slang::IBlob> blob;
		if (!type || SLANG_FAILED(type->getFullName(blob.writeRef())) || !blob)
			return {};
		const auto* text = static_cast<const char*>(blob->getBufferPointer());
		return {text, strnlen(text, blob->getBufferSize())};
	}

	const ReflectedStruct* ReflectedTypes::find(const std::string_view name) const {
		const auto it = std::ranges::find(structs, name, &ReflectedStruct::name);
		return it == structs.end() ? nullptr : &*it;
	}

	namespace {
		struct TypeWalker {
			std::string_view origin;
			StructRegistry& registry;
			ReflectedTypes& out;
			std::vector<std::string> visiting = {};

			void visitStruct(slang::TypeLayoutReflection* layout);
			ReflectedType classifyElement(slang::TypeLayoutReflection* layout, slang::TypeReflection* declared);
			ReflectedType classify(slang::TypeLayoutReflection* layout, slang::TypeReflection* declared);
		};

		ReflectedType TypeWalker::classifyElement(slang::TypeLayoutReflection* layout, slang::TypeReflection* declared) {
			using Kind = ReflectedType::Kind;
			ReflectedType type;
			// handles lower to uint2, only the declared type still names DescriptorHandle<T>
			const std::string declaredName = FullName(declared);
			constexpr std::string_view kHandle = "DescriptorHandle<";
			if (declaredName.starts_with(kHandle)) {
				const std::string_view resource = std::string_view(declaredName).substr(kHandle.size());
				if (resource.starts_with("RWTexture"))
					type.kind = Kind::StorageTexture;
				else if (resource.starts_with("Texture"))
					type.kind = Kind::SampledTexture;
				else if (resource.starts_with("SamplerState") || resource.starts_with("SamplerComparisonState"))
					type.kind = Kind::Sampler;
				else if (resource.starts_with("RaytracingAccelerationStructure"))
					type.kind = Kind::AccelerationStructure;
				else
					type.name = declaredName;
				return type;
			}
			switch (layout->getKind()) {
				case slang::TypeReflection::Kind::Scalar:
					type.kind = Kind::Scalar;
					type.scalar = layout->getScalarType();
					break;
				case slang::TypeReflection::Kind::Vector:
					type.kind = Kind::Vector;
					type.scalar = layout->getElementTypeLayout()->getScalarType();
					type.columns = static_cast<uint32_t>(layout->getElementCount());
					break;
				case slang::TypeReflection::Kind::Matrix:
					type.kind = Kind::Matrix;
					type.scalar = layout->getElementTypeLayout()->getElementTypeLayout()->getScalarType();
					type.rows = layout->getRowCount();
					type.columns = layout->getColumnCount();
					type.rowMajor = layout->getMatrixLayoutMode() == SLANG_MATRIX_LAYOUT_ROW_MAJOR;
					break;
				case slang::TypeReflection::Kind::Struct:
					type.kind = Kind::Struct;
					type.name = layout->getName();
					visitStruct(layout);
					break;
				default:
					type.name = declaredName.empty() ? FullName(layout->getType()) : declaredName;
					break;
			}
			return type;
		}

		ReflectedType TypeWalker::classify(slang::TypeLayoutReflection* layout, slang::TypeReflection* declared) {
			uint32_t count = 0;
			uint32_t stride = 0;
			const std::string arrayName = FullName(declared ? declared : layout->getType());
			if (layout->getKind() == slang::TypeReflection::Kind::Array) {
				count = static_cast<uint32_t>(layout->getElementCount());
				stride = static_cast<uint32_t>(layout->getElementStride(SLANG_PARAMETER_CATEGORY_UNIFORM));
				layout = layout->getElementTypeLayout();
				declared = declared ? declared->getElementType() : nullptr;
			}
			ReflectedType type;
			if (layout->getKind() == slang::TypeReflection::Kind::Pointer) {
				slang::TypeLayoutReflection* pointee = layout->getElementTypeLayout();
				const auto pointeeKind = pointee->getKind();
				if (pointeeKind == slang::TypeReflection::Kind::Pointer || pointeeKind == slang::TypeReflection::Kind::Array)
					type.name = FullName(layout->getType());
				else
					type = classifyElement(pointee, pointee->getType());
				type.pointer = true;
			} else {
				type = classifyElement(layout, declared);
			}
			const bool handle = type.kind == ReflectedType::Kind::SampledTexture || type.kind == ReflectedType::Kind::StorageTexture || type.kind == ReflectedType::Kind::Sampler || type.kind == ReflectedType::Kind::AccelerationStructure;
			type.unsizedArray = stride && !count;
			if ((type.pointer && handle) || type.unsizedArray) {
				type.kind = ReflectedType::Kind::Unsupported;
				type.name = arrayName;
			}
			type.arrayCount = count;
			type.arrayStride = stride;
			return type;
		}

		void TypeWalker::visitStruct(slang::TypeLayoutReflection* layout) {
			std::string name = layout->getName() ? layout->getName() : "";
			if (std::ranges::find(visiting, name) != visiting.end())
				return;
			ReflectedStruct type = {
				.name = name,
				.size = static_cast<uint32_t>(layout->getSize()),
				.stride = static_cast<uint32_t>(layout->getStride()),
				.alignment = static_cast<uint32_t>(layout->getAlignment()),
			};
			visiting.push_back(name);
			for (unsigned i = 0; i < layout->getFieldCount(); ++i) {
				slang::VariableLayoutReflection* field = layout->getFieldByIndex(i);
				type.fields.push_back({
					.name = field->getName(),
					.offset = static_cast<uint32_t>(field->getOffset()),
					.size = static_cast<uint32_t>(field->getTypeLayout()->getSize()),
					.type = classify(field->getTypeLayout(), field->getVariable()->getType()),
				});
			}
			visiting.pop_back();
			std::string description = DescribeStruct(out, type);
			if (const auto [it, inserted] = registry.try_emplace(name, description, origin); !inserted && it->second.first != description)
				out.conflicts.push_back(fmt::format("two different structs are named '{}', {} in '{}' and {} in '{}'. Rename one, generated structs are named after the Slang struct alone.", name, it->second.first, it->second.second, description, origin));
			if (!out.find(name))
				out.structs.push_back(std::move(type));
		}

		std::string DescribeType(const ReflectedTypes& types, const ReflectedType& type) {
			using Kind = ReflectedType::Kind;
			std::string text = type.pointer ? "*" : "";
			switch (type.kind) {
				case Kind::Scalar: text += Scalar(type.scalar).slang; break;
				case Kind::Vector: text += fmt::format("{}x{}", Scalar(type.scalar).slang, type.columns); break;
				case Kind::Matrix: text += fmt::format("{}x{}x{}{}", Scalar(type.scalar).slang, type.rows, type.columns, type.rowMajor ? "r" : "c"); break;
				case Kind::Struct: text += type.pointer ? type.name : DescribeStruct(types, *types.find(type.name)); break;
				case Kind::SampledTexture: text += "sampled"; break;
				case Kind::StorageTexture: text += "storage"; break;
				case Kind::Sampler: text += "sampler"; break;
				case Kind::AccelerationStructure: text += "accel"; break;
				case Kind::Unsupported: text += "?" + type.name; break;
			}
			if (type.arrayCount)
				text += fmt::format("[{}/{}]", type.arrayCount, type.arrayStride);
			return text;
		}
	} // namespace

	void ReflectPushTypes(slang::ProgramLayout* layout, const std::string_view origin, StructRegistry& registry, ReflectedTypes& out) {
		TypeWalker walker{origin, registry, out};
		for (unsigned i = 0; i < layout->getParameterCount(); ++i) {
			slang::VariableLayoutReflection* parameter = layout->getParameterByIndex(i);
			if (parameter->getCategory() != slang::ParameterCategory::PushConstantBuffer)
				continue;
			slang::TypeLayoutReflection* type = parameter->getTypeLayout();
			if (type->getKind() == slang::TypeReflection::Kind::ConstantBuffer)
				type = type->getElementTypeLayout();
			if (type->getKind() != slang::TypeReflection::Kind::Struct)
				continue;
			walker.visitStruct(type);
			if (std::ranges::find(out.pushBlocks, std::string_view(type->getName()), &ReflectedPushBlock::type) == out.pushBlocks.end())
				out.pushBlocks.push_back({parameter->getName() ? parameter->getName() : "", type->getName()});
		}
		for (unsigned i = 0; i < layout->getEntryPointCount(); ++i) {
			slang::EntryPointReflection* entryPoint = layout->getEntryPointByIndex(i);
			for (unsigned p = 0; p < entryPoint->getParameterCount(); ++p) {
				slang::VariableLayoutReflection* parameter = entryPoint->getParameterByIndex(p);
				const auto category = parameter->getCategory();
				if (category == slang::ParameterCategory::Uniform || category == slang::ParameterCategory::PushConstantBuffer || category == slang::ParameterCategory::Mixed)
					out.entryPointUniforms.push_back(fmt::format("{}::{}", entryPoint->getName(), parameter->getName()));
			}
		}
	}

	std::string DescribeStruct(const ReflectedTypes& types, const ReflectedStruct& type) {
		std::string text = type.name + "{";
		for (const ReflectedField& field: type.fields)
			text += fmt::format("{}@{}:{};", field.name, field.offset, DescribeType(types, field.type));
		return text + fmt::format("}}{}/{}/{}", type.size, type.stride, type.alignment);
	}

	static bool HasUnsizedArray(const ReflectedStruct& type) {
		return std::ranges::any_of(type.fields, [](const ReflectedField& field) { return field.type.unsizedArray; });
	}

	static std::vector<std::string> CheckCodegenRules(const ReflectedTypes& types) {
		std::vector<std::string> errors = types.conflicts;
		for (const std::string& uniform: types.entryPointUniforms)
			errors.push_back(fmt::format("entry point parameter '{}' is uniform, and Slang puts it in a second push constant block at offset 0. Move it into the [vk::push_constant] struct.", uniform));
		for (const ReflectedStruct& type: types.structs) {
			if (HasUnsizedArray(type)) {
				for (const ReflectedField& field: type.fields)
					if (field.type.unsizedArray)
						errors.push_back(fmt::format("'{}::{}' is the unsized array '{}', which has no C++ struct member. Point to the elements with a separate Ptr<T>.", type.name, field.name, field.type.name));
				continue;
			}
			if (type.fields.empty())
				errors.push_back(fmt::format("struct '{}' has no fields, and an empty C++ struct is 1 byte.", type.name));
			uint32_t end = 0;
			for (const ReflectedField& field: type.fields) {
				const std::string where = fmt::format("{}::{}", type.name, field.name);
				if (field.type.kind == ReflectedType::Kind::Unsupported)
					errors.push_back(fmt::format("'{}' has type '{}', which has no C++ slot type.", where, field.type.name));
				if (field.type.scalar == slang::TypeReflection::ScalarType::Bool)
					errors.push_back(fmt::format("'{}' is a bool, which Slang lays out as 1 byte in a push block and 4 bytes behind a pointer. Use uint.", where));
				if (field.offset > end)
					errors.push_back(fmt::format("'{}' starts at byte {}, so bytes {}..{} are implicit padding. Add a {} byte padding field before it in the Slang struct.", where, field.offset, end, field.offset, field.offset - end));
				else if (field.offset < end)
					errors.push_back(fmt::format("'{}' starts at byte {}, inside the field before it, which ends at byte {}.", where, field.offset, end));
				end = std::max(end, field.offset + field.size);
			}
			if (!type.fields.empty() && type.stride > end)
				errors.push_back(fmt::format("struct '{}' ends at byte {} but its stride is {}, so it has {} bytes of implicit tail padding. Add a {} byte padding field at its end in the Slang struct.", type.name, end, type.stride, type.stride - end, type.stride - end));
		}
		return errors;
	}

	namespace {
		class SpirvComparison {
		public:
			SpirvComparison(const ReflectedTypes& types, const SpirvLayout& spirv, std::vector<std::string>& errors) :
				_types(types),
				_spirv(spirv),
				_errors(errors) {}

			void compareStruct(const ReflectedStruct& type, const uint32_t id) {
				if (!_compared.insert(id).second || HasUnsizedArray(type))
					return;
				const SpirvLayout::Type& spirvType = _spirv.type(id);
				if (spirvType.op != SpvOpTypeStruct || spirvType.args.size() != type.fields.size()) {
					fail(type.name, "is not a struct of {} members in the SPIR-V", type.fields.size());
					return;
				}
				for (uint32_t i = 0; i < type.fields.size(); ++i) {
					const ReflectedField& field = type.fields[i];
					const std::string where = fmt::format("{}::{}", type.name, field.name);
					const std::optional<uint32_t> offset = _spirv.offset(id, i);
					if (offset != field.offset)
						fail(where, "is at byte {} in reflection but {} in the SPIR-V", field.offset, offset ? fmt::format("{}", *offset) : "nowhere");
					compareField(where, field.type, spirvType.args[i]);
				}
			}

		private:
			template<class... Args>
			void fail(const std::string_view where, fmt::format_string<Args...> format, Args&&... args) {
				_errors.push_back(fmt::format("'{}' {}. Slang reflection disagrees with the code it emits, so the generated struct would be wrong.", where, fmt::format(format, std::forward<Args>(args)...)));
			}

			void compareField(const std::string& where, const ReflectedType& type, uint32_t id) {
				if (type.arrayCount) {
					const uint32_t array = _spirv.unwrapArray(id);
					const SpirvLayout::Type& spirvType = _spirv.type(array);
					if (spirvType.op != SpvOpTypeArray || spirvType.args.size() != 2 || _spirv.constant(spirvType.args[1]) != type.arrayCount) {
						fail(where, "is not an array of {} in the SPIR-V", type.arrayCount);
						return;
					}
					if (_spirv.stride(array) != type.arrayStride)
						fail(where, "has element stride {} in reflection but {} in the SPIR-V", type.arrayStride, _spirv.stride(array).value_or(0));
					id = spirvType.args[0];
				}
				if (type.pointer) {
					const SpirvLayout::Type& pointer = _spirv.type(id);
					if (pointer.op != SpvOpTypePointer || pointer.args.size() != 2 || pointer.args[0] != SpvStorageClassPhysicalStorageBuffer) {
						fail(where, "is not a buffer device address in the SPIR-V");
						return;
					}
					if (const std::optional<uint32_t> stride = _spirv.stride(id); stride && elementStride(type) && stride != elementStride(type))
						fail(where, "points to elements {} bytes apart in reflection but {} in the SPIR-V", elementStride(type), *stride);
					id = pointer.args[1];
				}
				compareValue(where, type, id);
			}

			uint32_t elementStride(const ReflectedType& type) const {
				const uint32_t scalarBytes = Scalar(type.scalar).bits / 8;
				switch (type.kind) {
					case ReflectedType::Kind::Scalar: return scalarBytes;
					case ReflectedType::Kind::Vector: return scalarBytes * type.columns;
					case ReflectedType::Kind::Matrix: return scalarBytes * type.rows * type.columns;
					case ReflectedType::Kind::Struct: {
						const ReflectedStruct* nested = _types.find(type.name);
						return nested && !HasUnsizedArray(*nested) ? nested->stride : 0;
					}
					default: return 8;
				}
			}

			bool isScalar(const uint32_t id, const slang::TypeReflection::ScalarType scalar) const {
				const SpirvLayout::Type& t = _spirv.type(id);
				const bool isFloat = scalar == slang::TypeReflection::ScalarType::Float16 || scalar == slang::TypeReflection::ScalarType::Float32 || scalar == slang::TypeReflection::ScalarType::Float64;
				return t.op == (isFloat ? SpvOpTypeFloat : SpvOpTypeInt) && !t.args.empty() && t.args[0] == Scalar(scalar).bits;
			}
			bool isVector(const uint32_t id, const slang::TypeReflection::ScalarType scalar, const uint32_t count) const {
				const SpirvLayout::Type& t = _spirv.type(id);
				return t.op == SpvOpTypeVector && t.args.size() == 2 && t.args[1] == count && isScalar(t.args[0], scalar);
			}

			void compareValue(const std::string& where, const ReflectedType& type, const uint32_t id) {
				using Kind = ReflectedType::Kind;
				switch (type.kind) {
					case Kind::Scalar:
						if (!isScalar(id, type.scalar))
							fail(where, "is not a {} bit scalar in the SPIR-V", Scalar(type.scalar).bits);
						break;
					case Kind::Vector:
						if (!isVector(id, type.scalar, type.columns))
							fail(where, "is not a {} component vector in the SPIR-V", type.columns);
						break;
					case Kind::Matrix: {
						const uint32_t vectors = type.rowMajor ? type.rows : type.columns;
						const uint32_t components = type.rowMajor ? type.columns : type.rows;
						const uint32_t array = _spirv.unwrapArray(id);
						const SpirvLayout::Type& t = _spirv.type(array);
						const bool arrayOfVectors = t.op == SpvOpTypeArray && t.args.size() == 2 && _spirv.constant(t.args[1]) == vectors && isVector(t.args[0], type.scalar, components) && _spirv.stride(array) == components * Scalar(type.scalar).bits / 8;
						if (!arrayOfVectors)
							fail(where, "is not {} tightly packed vectors of {} in the SPIR-V", vectors, components);
						break;
					}
					case Kind::Struct:
						if (const ReflectedStruct* nested = _types.find(type.name))
							compareStruct(*nested, id);
						break;
					case Kind::SampledTexture:
					case Kind::StorageTexture:
					case Kind::Sampler:
					case Kind::AccelerationStructure:
						if (!isVector(id, slang::TypeReflection::ScalarType::UInt32, 2))
							fail(where, "is not a uint2 descriptor handle in the SPIR-V");
						break;
					case Kind::Unsupported:
						break;
				}
			}

			const ReflectedTypes& _types;
			const SpirvLayout& _spirv;
			std::vector<std::string>& _errors;
			std::unordered_set<uint32_t> _compared;
		};
	} // namespace

	static std::string CppType(const ReflectedType& type, const HeaderOptions& options) {
		using Kind = ReflectedType::Kind;
		using S = slang::TypeReflection::ScalarType;
		std::string base;
		switch (type.kind) {
			case Kind::Scalar:
				if (const char* cpp = Scalar(type.scalar).cpp)
					base = cpp;
				break;
			case Kind::Vector:
				if (type.scalar == S::Float32 || type.scalar == S::Int32 || type.scalar == S::UInt32)
					base = fmt::format("::{}::{}{}", options.typesNamespace, Scalar(type.scalar).slang, type.columns);
				break;
			case Kind::Matrix:
				if (type.scalar == S::Float32)
					base = fmt::format("::{}::{}{}x{}", options.typesNamespace, Scalar(type.scalar).slang, type.rows, type.columns);
				break;
			case Kind::Struct: base = type.name; break;
			case Kind::SampledTexture: base = "::mythril::SampledTextureSlot"; break;
			case Kind::StorageTexture: base = "::mythril::StorageTextureSlot"; break;
			case Kind::Sampler: base = "::mythril::SamplerSlot"; break;
			case Kind::AccelerationStructure: base = "::mythril::AccelerationStructureSlot"; break;
			case Kind::Unsupported: break;
		}
		if (base.empty() || !type.pointer)
			return base;
		return fmt::format("::mythril::DevicePtr<{}>", base);
	}

	static std::string Escape(const std::string_view text) {
		std::string out;
		for (const char c: text) {
			if (c == '"' || c == '\\')
				out += '\\';
			out += c;
		}
		return out;
	}

	static std::string EmitMatrixChecks(const ReflectedTypes& types, const HeaderOptions& options) {
		std::set<std::string> checks;
		for (const ReflectedStruct& type: types.structs) {
			for (const ReflectedField& field: type.fields) {
				const ReflectedType& t = field.type;
				if (t.kind != ReflectedType::Kind::Matrix)
					continue;
				ReflectedType value = t;
				value.pointer = false;
				const std::string cpp = CppType(value, options);
				const uint32_t scalarBytes = Scalar(t.scalar).bits / 8;
				const uint32_t vectors = t.rowMajor ? t.rows : t.columns;
				const uint32_t vectorBytes = (t.rowMajor ? t.columns : t.rows) * scalarBytes;
				checks.insert(fmt::format("\tstatic_assert(sizeof({0}) == {1} && ::mythril::detail::MatrixVectorBytes<{0}>({2}) == {2}, \"{0} must hold {3} vectors of {2} bytes like the {4} Slang {5}{6}, check the types header\");\n", cpp, vectors * vectorBytes, vectorBytes, vectors, t.rowMajor ? "row-major" : "column-major", Scalar(t.scalar).slang, fmt::format("{}x{}", t.rows, t.columns)));
			}
		}
		std::string out;
		for (const std::string& check: checks)
			out += check;
		return out;
	}

	std::string HeaderBanner(const std::string_view sourceName) { return fmt::format("// generated by mythril-reflect from {}, do not edit\n#pragma once\n", sourceName); }

	std::string EmitHeader(const std::string_view sourceName, const ReflectedTypes& types, const std::span<const std::string> includes, const PushLayouts& pushLayouts, const HeaderOptions& options) {
		std::string out = HeaderBanner(sourceName) + fmt::format("\n#include <cstddef>\n#include <cstdint>\n\n#include \"mythril/ShaderSlots.h\"\n#include \"{}\"\n", options.typesHeader);
		for (const std::string& include: includes)
			out += fmt::format("#include \"{}\"\n", include);
		if (types.structs.empty())
			return out;
		out += fmt::format("\nnamespace {} {{\n", options.ns);
		out += EmitMatrixChecks(types, options);
		for (const ReflectedStruct& type: types.structs)
			out += fmt::format("\tstruct {};\n", type.name);
		for (const ReflectedStruct& type: types.structs) {
			out += fmt::format("\n\tstruct {} {{\n", type.name);
			for (const ReflectedField& field: type.fields) {
				const std::string array = field.type.arrayCount ? fmt::format("[{}]", field.type.arrayCount) : "";
				out += fmt::format("\t\t{} {}{};\n", CppType(field.type, options), field.name, array);
			}
			if (const auto layout = pushLayouts.find(type.name); layout != pushLayouts.end()) {
				out += fmt::format("\n\t\tstatic constexpr uint64_t kLayoutHash = 0x{:016x}ull;\n#ifdef DEBUG\n\t\tstatic constexpr const char* kLayout = \"{}\";\n#endif\n", layout->second.hash, Escape(layout->second.description));
			}
			out += "\t};\n";
			for (const ReflectedField& field: type.fields) {
				out += fmt::format("\tstatic_assert(offsetof({0}, {1}) == {2}, \"{0}::{1} is not at Slang offset {2}, check the types header\");\n", type.name, field.name, field.offset);
				if (field.type.arrayCount)
					out += fmt::format("\tstatic_assert(sizeof({0}::{1}) == {2}, \"{0}::{1} elements are not {3} bytes apart as in Slang, check the types header\");\n", type.name, field.name, field.type.arrayCount * field.type.arrayStride, field.type.arrayStride);
			}
			out += fmt::format("\tstatic_assert(sizeof({0}) == {1} && alignof({0}) == {2}, \"{0} is not {1} bytes aligned to {2} as in Slang, check the types header\");\n", type.name, type.stride, type.alignment);
		}
		return out + fmt::format("}} // namespace {}\n", options.ns);
	}

	std::string EmitModuleData(const ModuleData& data, const HeaderOptions& options) {
		std::string body;
		if (!data.spirvFile.empty())
			body += fmt::format("\tinline constexpr const char* kSpirvFile = \"{}\";\n", data.spirvFile);
		if (!data.threadGroupSize.empty())
			body += fmt::format("\tinline constexpr uint32_t kThreadGroupSize[3] = {{{}}};\n", fmt::join(data.threadGroupSize, ", "));
		for (const SlangCompiler::Constant& constant: data.constants)
			body += fmt::format("\tinline constexpr {} {} = {};\n", constant.isSigned ? "int32_t" : "uint32_t", constant.name, constant.value);
		return body.empty() ? std::string() : fmt::format("\nnamespace {0}::{1} {{\n{2}}} // namespace {0}::{1}\n", options.ns, data.name, body);
	}

	// an unused push block is not in the SPIR-V
	static const SpirvPushBlock* FindSpirvBlock(const ReflectedTypes& types, const ReflectedPushBlock& block, const std::vector<SpirvPushBlock>& blocks) {
		if (const auto it = std::ranges::find(blocks, block.variable, [](const SpirvPushBlock& spirv) { return spirv.layout.variable; }); it != blocks.end())
			return &*it;
		return blocks.size() == 1 && types.pushBlocks.size() == 1 ? &blocks.front() : nullptr;
	}

	std::vector<std::string> CheckHeader(const ReflectedTypes& types, const SpirvLayout& layout, const HeaderOptions& options, PushLayouts& pushLayouts) {
		std::vector<std::string> errors = CheckCodegenRules(types);
		if (!layout.valid)
			errors.emplace_back("the SPIR-V module is not valid");
		else if (types.conflicts.empty()) {
			const std::vector<SpirvPushBlock> spirvBlocks = DescribeSpirvPushBlocks(layout);
			SpirvComparison comparison(types, layout, errors);
			for (const ReflectedPushBlock& block: types.pushBlocks) {
				const SpirvPushBlock* spirvBlock = FindSpirvBlock(types, block, spirvBlocks);
				if (!spirvBlock) {
					errors.push_back(fmt::format("push block '{}' is never read, so it is not in the SPIR-V and has no layout hash. Read it in the shader or remove it.", block.variable));
					continue;
				}
				comparison.compareStruct(*types.find(block.type), spirvBlock->type);
				pushLayouts.try_emplace(block.type, spirvBlock->layout);
			}
		}
		const bool sessionRowMajor = options.matrixMode == SLANG_MATRIX_LAYOUT_ROW_MAJOR;
		for (const ReflectedStruct& type: types.structs) {
			for (const ReflectedField& field: type.fields) {
				if (field.type.kind != ReflectedType::Kind::Unsupported && field.type.scalar != slang::TypeReflection::ScalarType::Bool && CppType(field.type, options).empty())
					errors.push_back(fmt::format("'{}::{}' has a type with no C++ equivalent, vectors must be float, int or uint and matrices float.", type.name, field.name));
				if (field.type.kind == ReflectedType::Kind::Matrix && field.type.rowMajor != sessionRowMajor)
					errors.push_back(fmt::format("'{}::{}' is {} but the session is {}, so its C++ type would hold the other orientation. Remove the layout qualifier.", type.name, field.name, field.type.rowMajor ? "row_major" : "column_major", sessionRowMajor ? "row-major" : "column-major"));
			}
		}
		return errors;
	}
} // namespace mythril
