//
// Created by Hayden Rivas on 11/7/25.
//

#include "SlangCompiler.h"
#include "HelperMacros.h"

#include <array>
#include <cstring>
#include <filesystem>
#include <string>
#include <fstream>
#include <vector>

#include <slang/slang.h>

#include "mythril/CPUProfiler.h"
#include "mythril/CTX.h"
#include "mythril/CTXBuilder.h"

namespace mythril {
	static constexpr const char* kBindlessFunctionName = "getDescriptorFromHandle";
	// must match the descriptor set layout built in CTX::growBindlessDescriptorPoolImpl
	static constexpr const char* kBindlessModuleSource = R"(
export T getDescriptorFromHandle<T : IOpaqueDescriptor>(DescriptorHandle<T> handleValue) {
    return defaultGetDescriptorFromHandle(handleValue, BindlessDescriptorOptions.None);
}
)";

	static SlangMatrixLayoutMode SlangMatrixModeFromMythrilMatrixMode(SlangCfg::MatrixMode matrix_mode) {
		switch (matrix_mode) {
			case SlangCfg::MatrixMode::ColMajor:
				return SLANG_MATRIX_LAYOUT_COLUMN_MAJOR;
			case SlangCfg::MatrixMode::RowMajor:
				return SLANG_MATRIX_LAYOUT_ROW_MAJOR;
		}
		ASSERT_MSG(false, "If this happens something went horribly wrong.");
	}

	void SlangCompiler::create() {
		if (this->_sessionExists)
			return;
		createSessionImpl();
		_sessionExists = true;
	}
	void SlangCompiler::destroy() {
		if (!this->_sessionExists)
			return;
		_bindlessModule.detach();
		_slangSession.detach();
		_globalSlangSession.detach();
		_sessionExists = false;
	}

	void SlangCompiler::addSearchPath(const std::filesystem::path& searchPath) { this->_shaderSearchPaths.push_back(searchPath); }
	void SlangCompiler::clearSearchPaths() { this->_shaderSearchPaths.clear(); }
	void SlangCompiler::setMatrixLayoutMode(const SlangCfg::MatrixMode mode) {
		ASSERT_MSG(!_sessionExists, "The matrix layout must be set before the Slang session is created.");
		_matrixLayoutMode = mode;
	}
	void SlangCompiler::addCompilerOptions(const std::span<const slang::CompilerOptionEntry> options) {
		ASSERT_MSG(!_sessionExists, "Compiler options must be added before the Slang session is created.");
		const auto ownString = [this](const char* str) -> const char* { return str ? _userOptionStrings.emplace_back(str).c_str() : nullptr; };
		for (slang::CompilerOptionEntry entry: options) {
			ASSERT_MSG(
			        entry.name != slang::CompilerOptionName::BindlessSpaceIndex && entry.name != slang::CompilerOptionName::VulkanUseEntryPointName && entry.name != slang::CompilerOptionName::ForceCLayout,
			        "Slang compiler option {} is required by Mythril and cannot be overridden.", static_cast<int>(entry.name)
			);
			entry.value.stringValue0 = ownString(entry.value.stringValue0);
			entry.value.stringValue1 = ownString(entry.value.stringValue1);
			_userOptions.push_back(entry);
		}
	}

	static const char* ResolveDiagnosticsMessage(const Slang::ComPtr<slang::IBlob>& diagnostics_blob) {
		return diagnostics_blob ? static_cast<const char*>(diagnostics_blob->getBufferPointer()) : "(no diagnostics available)";
	}

	void SlangCompiler::createSessionImpl() {
		const SlangResult global_result = slang::createGlobalSession(this->_globalSlangSession.writeRef());
		ASSERT_MSG(SLANG_SUCCEEDED(global_result), "Slang failed to create global session!");

		slang::TargetDesc targetDesc = {
		    .format = SLANG_SPIRV,
		    .profile = this->_globalSlangSession->findProfile("spirv_1_6"),
		};
		// by default emits spirv
		std::array entries = {
		    // capabilities
		    // slang::CompilerOptionEntry{ .name = slang::CompilerOptionName::Capability,
		    // 	.value = { .kind = slang::CompilerOptionValueKind::String, .stringValue0 = "SPV_KHR_vulkan_memory_model" }
		    // },
		    // slang::CompilerOptionEntry{
		    // 	.value = { .kind = slang::CompilerOptionValueKind::String, .stringValue0 = "spvDerivativeControl" }
		    // },
		    // slang::CompilerOptionEntry{ .name = slang::CompilerOptionName::Capability,
		    // 	.value = { .kind = slang::CompilerOptionValueKind::String, .stringValue0 = "SPV_KHR_compute_shader_derivatives" }
		    // },
		    slang::CompilerOptionEntry{.name = slang::CompilerOptionName::BindlessSpaceIndex, .value = {.kind = slang::CompilerOptionValueKind::Int, .intValue0 = 0}},

		    slang::CompilerOptionEntry{.name = slang::CompilerOptionName::VulkanUseEntryPointName, .value = {.kind = slang::CompilerOptionValueKind::Int, .intValue0 = true}},
		    slang::CompilerOptionEntry{.name = slang::CompilerOptionName::VulkanInvertY, .value = {.kind = slang::CompilerOptionValueKind::Int, .intValue0 = true}},
#ifdef DEBUG
		    slang::CompilerOptionEntry{.name = slang::CompilerOptionName::Optimization, .value = {.kind = slang::CompilerOptionValueKind::Int, .intValue0 = SLANG_OPTIMIZATION_LEVEL_DEFAULT}},
		    slang::CompilerOptionEntry{.name = slang::CompilerOptionName::DebugInformation, .value = {.kind = slang::CompilerOptionValueKind::Int, .intValue0 = SLANG_DEBUG_INFO_LEVEL_MAXIMAL}},
#else
		    slang::CompilerOptionEntry{.name = slang::CompilerOptionName::Optimization, .value = {.kind = slang::CompilerOptionValueKind::Int, .intValue0 = SLANG_OPTIMIZATION_LEVEL_MAXIMAL}},
		    slang::CompilerOptionEntry{.name = slang::CompilerOptionName::DebugInformation, .value = {.kind = slang::CompilerOptionValueKind::Int, .intValue0 = SLANG_DEBUG_INFO_LEVEL_NONE}},
#endif
		    // forces scalar layout which is awesome, means we can have a header file that is shared by c++ and slang
		    slang::CompilerOptionEntry{.name = slang::CompilerOptionName::ForceCLayout, .value = {.kind = slang::CompilerOptionValueKind::Int, .intValue0 = true}},
		};

		ASSERT_MSG(this->_shaderSearchPaths.size() <= 16, "You cannot have more than 16 searchpaths in SlangCompiler, this is arbitrary btw lol");
		// path::c_str() is wchar_t on Windows, slang wants char
		std::array<std::string, 16> sp_strings = {};
		std::array<const char*, 16> sp_cstrings = {};
		int64_t sp_count = 0;
		for (const std::filesystem::path& path: this->_shaderSearchPaths) {
			sp_strings[sp_count] = path.string();
			sp_cstrings[sp_count] = sp_strings[sp_count].c_str();
			sp_count++;
		}

		// slang keeps the first entry for each option, so user options go first to override the defaults
		std::vector<slang::CompilerOptionEntry> options = _userOptions;
		options.insert(options.end(), entries.begin(), entries.end());

		const slang::SessionDesc sessionDesc = {
		    .targets = &targetDesc,
		    .targetCount = 1,

		    .defaultMatrixLayoutMode = SlangMatrixModeFromMythrilMatrixMode(_matrixLayoutMode),

		    .searchPaths = sp_cstrings.data(),
		    .searchPathCount = sp_count,

		    .compilerOptionEntries = options.data(),
		    .compilerOptionEntryCount = static_cast<uint32_t>(options.size()),
		};
		const SlangResult session_result = this->_globalSlangSession->createSession(sessionDesc, this->_slangSession.writeRef());
		ASSERT_MSG(SLANG_SUCCEEDED(session_result), "Slang failed to create session!");

		Slang::ComPtr<slang::IBlob> bindless_diagnostics;
		this->_bindlessModule = this->_slangSession->loadModuleFromSourceString("mythril_bindless", "mythril_bindless.slang", kBindlessModuleSource, bindless_diagnostics.writeRef());
		ASSERT_MSG(this->_bindlessModule, "Failed to load Mythril's bindless module! Diagnostics Below:\n{}", ResolveDiagnosticsMessage(bindless_diagnostics));
	}

	static void CollectPushResourcesIn(slang::TypeLayoutReflection* type, const size_t base, std::vector<PushResourceField>& out) {
		for (unsigned i = 0; i < type->getFieldCount(); ++i) {
			slang::VariableLayoutReflection* field = type->getFieldByIndex(i);
			slang::TypeLayoutReflection* layout = field->getTypeLayout();
			const auto offset = static_cast<uint32_t>(base + field->getOffset());
			if (layout->getKind() == slang::TypeReflection::Kind::Pointer) {
				out.push_back({field->getName(), offset, PushResourceField::Kind::Pointer});
				continue;
			}
			if (layout->getKind() == slang::TypeReflection::Kind::Struct) {
				CollectPushResourcesIn(layout, offset, out);
				continue;
			}
			// handles lower to uint2, only the declared type still names DescriptorHandle<T>
			slang::TypeReflection* declared = field->getVariable()->getType();
			const bool isArray = declared->getKind() == slang::TypeReflection::Kind::Array;
			Slang::ComPtr<slang::IBlob> fullName;
			(isArray ? declared->getElementType() : declared)->getFullName(fullName.writeRef());
			const std::string_view name = fullName ? std::string_view(static_cast<const char*>(fullName->getBufferPointer()), fullName->getBufferSize()) : std::string_view{};
			constexpr std::string_view kHandle = "DescriptorHandle<";
			if (!name.starts_with(kHandle))
				continue;
			const std::string_view resource = name.substr(kHandle.size());
			if (!resource.starts_with("Texture") && !resource.starts_with("RWTexture"))
				continue;
			const PushResourceField::Kind kind = resource.starts_with("RW") ? PushResourceField::Kind::StorageTexture : PushResourceField::Kind::SampledTexture;
			const size_t count = isArray ? declared->getElementCount() : 1;
			const size_t stride = isArray ? layout->getElementStride(SLANG_PARAMETER_CATEGORY_UNIFORM) : 0;
			for (size_t element = 0; element < count; ++element)
				out.push_back({field->getName(), static_cast<uint32_t>(offset + element * stride), kind});
		}
	}
	std::vector<PushResourceField> CollectPushResources(slang::ProgramLayout* layout) {
		std::vector<PushResourceField> out;
		for (unsigned i = 0; i < layout->getParameterCount(); ++i) {
			slang::VariableLayoutReflection* parameter = layout->getParameterByIndex(i);
			if (parameter->getCategory() != slang::ParameterCategory::PushConstantBuffer)
				continue;
			slang::TypeLayoutReflection* type = parameter->getTypeLayout();
			if (type->getKind() == slang::TypeReflection::Kind::ConstantBuffer)
				type = type->getElementTypeLayout();
			CollectPushResourcesIn(type, 0, out);
		}
		return out;
	}

	static bool DeclaresBindlessOverride(slang::DeclReflection* moduleDecl) {
		for (slang::DeclReflection* child: moduleDecl->getChildren()) {
			const auto kind = child->getKind();
			if (kind != slang::DeclReflection::Kind::Func && kind != slang::DeclReflection::Kind::Generic)
				continue;
			const char* name = child->getName();
			if (name && std::strcmp(name, kBindlessFunctionName) == 0)
				return true;
		}
		return false;
	}

	void SlangCompiler::validateNoBindlessOverride() const {
		for (SlangInt i = 0; i < _slangSession->getLoadedModuleCount(); ++i) {
			slang::IModule* loaded = _slangSession->getLoadedModule(i);
			if (loaded == _bindlessModule.get())
				continue;
			ASSERT_MSG(
			        !DeclaresBindlessOverride(loaded->getModuleReflection()),
			        "Module '{}' defines '{}', but Mythril already provides it and a second definition would be silently ignored. Remove it from your shader.",
			        loaded->getFilePath() ? loaded->getFilePath() : loaded->getName(),
			        kBindlessFunctionName
			);
		}
	}

	static std::string JoinActiveSearchPathsLog(const std::vector<std::filesystem::path>& paths) {
		std::string message;
		for (const auto& path: paths) {
			message += "'" + path.string() + "'" + "\n";
		}
		return message;
	}


	CompileResult SlangCompiler::compileSlangImpl(Slang::ComPtr<slang::IModule>& slang_module, Slang::ComPtr<slang::IBlob>& diagnostics_blob) const {
		validateNoBindlessOverride();

		// 2. query entry points
		// if you have more than 8 components we want to link than whatever man
		std::vector<Slang::ComPtr<slang::IComponentType>> componentTypes = {};
		componentTypes.emplace_back(slang_module);
		const int definedEntryPointCount = slang_module->getDefinedEntryPointCount();
		for (int i = 0; i < definedEntryPointCount; i++) {
			Slang::ComPtr<slang::IEntryPoint> entryPoint;
			const SlangResult entry_point_result = slang_module->getDefinedEntryPoint(i, entryPoint.writeRef());
			ASSERT_MSG(SLANG_SUCCEEDED(entry_point_result), "Entry point retrieval failed!");
			componentTypes.emplace_back(entryPoint.get());
		}
		componentTypes.emplace_back(_bindlessModule);

		// 3. compose module
		Slang::ComPtr<slang::IComponentType> composed_program;
		const SlangResult module_result = this->_slangSession->createCompositeComponentType(
		        reinterpret_cast<slang::IComponentType**>(componentTypes.data()), static_cast<int>(componentTypes.size()), composed_program.writeRef(), diagnostics_blob.writeRef()
		);
		ASSERT_MSG(SLANG_SUCCEEDED(module_result), "Composition failed! Diagnostics Below:\n{}", ResolveDiagnosticsMessage(diagnostics_blob));
		diagnostics_blob.setNull();

		// 4. linking
		// linked_program needs to be kept alive as we want to access entrypoints, and therefore we store linked_program in the CompileResult return
		Slang::ComPtr<slang::IComponentType> linked_program;
		const SlangResult compose_result = composed_program->link(linked_program.writeRef(), diagnostics_blob.writeRef());
		ASSERT_MSG(SLANG_SUCCEEDED(compose_result), "Linking failed! Diagnostics Below:\n{}", ResolveDiagnosticsMessage(diagnostics_blob));
		diagnostics_blob.setNull();

		// 4.5. retrieve layout for reflection
		// will always be 0 for raster shaders at least
		int targetIndex = 0;
		slang::ProgramLayout* program_layout = linked_program->getLayout(targetIndex, diagnostics_blob.writeRef());
		ASSERT_MSG(program_layout, "Failed to get ProgramLayout*, therefore shader reflection will also fail! Diagnostics Below:\n{}", ResolveDiagnosticsMessage(diagnostics_blob));
		diagnostics_blob.setNull();

		// 5. retrieve kernel code
		Slang::ComPtr<slang::IBlob> spirvBlob;
		// use getTargetCode instead of something like getEntryPointCode as this works with multiple entry points
		const SlangResult code_result = linked_program->getTargetCode(0, spirvBlob.writeRef(), diagnostics_blob.writeRef());
		ASSERT_MSG(SLANG_SUCCEEDED(code_result), "Code retrieval failed! Diagnostics Below:\n{}", ResolveDiagnosticsMessage(diagnostics_blob));


		// 5+. bonus: retreive metadata to determine if parameters are used or not
		Slang::ComPtr<slang::IMetadata> target_metadata;
		const SlangResult metadata_result = linked_program->getTargetMetadata(0, target_metadata.writeRef(), diagnostics_blob.writeRef());
		ASSERT_MSG(SLANG_SUCCEEDED(metadata_result), "Metadata retrieval Failed! Diagnostics Below:\n{}", ResolveDiagnosticsMessage(diagnostics_blob));

		CompileResult result;
		result._linkedProgram = linked_program;
		result._spirvBlob = spirvBlob;
		result._programLayout = program_layout;
		result.success = true;
		return result;
	}

	// this isnt const, loadModule
	// NOLINT(readability-make-member-function-const)
	// ReSharper disable once CppMemberFunctionMayBeConst
	CompileResult SlangCompiler::compileSlangFromDisk(const std::filesystem::path& filepath) {
		MYTH_PROFILER_FUNCTION();
		ASSERT_MSG(exists(filepath), "Shader file '{}' does not exist.", filepath.string());
		// 1. load module
		Slang::ComPtr<slang::IModule> slang_module;
		Slang::ComPtr<slang::IBlob> diagnostics_blob;
		slang_module = this->_slangSession->loadModule(filepath.string().c_str(), diagnostics_blob.writeRef());
		ASSERT_MSG(
		        slang_module,
		        "--------------------------- FILE: -------------------------- \n'{}'\n -------------------------- SEARCH_PATHS: -------------------------- \n{} -------------------------- ERROR: -------------------------- \nSlang failed to load module, this might happen for a bunch of different reasons like filepath couldnt be found or the shader is "
		        "invalid.\nDiagnostics Below:\n{}",
		        filepath.string().c_str(),
		        JoinActiveSearchPathsLog(this->_shaderSearchPaths),
		        ResolveDiagnosticsMessage(diagnostics_blob)
		);
		diagnostics_blob.setNull();
		return compileSlangImpl(slang_module, diagnostics_blob);
	}
} // namespace mythril
