//
// Created by Hayden Rivas on 11/7/25.
//

#include "SlangCompiler.h"

#include <algorithm>
#include <array>
#include <cstring>
#include <filesystem>
#include <optional>
#include <string>
#include <vector>

#include <slang/slang.h>
#include <fmt/format.h>

namespace mythril {
	static constexpr const char* kBindlessFunctionName = "getDescriptorFromHandle";
	// must match the descriptor set layout built in CTX::growBindlessDescriptorPoolImpl
	static constexpr const char* kBindlessModuleSource = R"(
export T getDescriptorFromHandle<T : IOpaqueDescriptor>(DescriptorHandle<T> handleValue) {
    return defaultGetDescriptorFromHandle(handleValue, BindlessDescriptorOptions.None);
}
)";

	static std::string BlobText(const Slang::ComPtr<slang::IBlob>& blob) {
		return blob ? std::string(static_cast<const char*>(blob->getBufferPointer()), blob->getBufferSize()) : std::string();
	}

	std::optional<SlangCompiler> SlangCompiler::Create(Options options, std::string& diagnostics) {
		SlangCompiler compiler;
		if (SLANG_FAILED(slang::createGlobalSession(compiler._globalSlangSession.writeRef()))) {
			diagnostics = "Slang failed to create global session!";
			return std::nullopt;
		}

		slang::TargetDesc targetDesc = {
			.format = SLANG_SPIRV,
			.profile = compiler._globalSlangSession->findProfile("spirv_1_6"),
		};
		const auto option = [](const slang::CompilerOptionName name, const int value) {
			return slang::CompilerOptionEntry{ .name = name, .value = { .kind = slang::CompilerOptionValueKind::Int, .intValue0 = value } };
		};
		std::array entries = {
			option(slang::CompilerOptionName::BindlessSpaceIndex, 0),
			option(slang::CompilerOptionName::VulkanUseEntryPointName, true),
			option(slang::CompilerOptionName::VulkanInvertY, true),
			option(slang::CompilerOptionName::Optimization, options.debug ? SLANG_OPTIMIZATION_LEVEL_DEFAULT : SLANG_OPTIMIZATION_LEVEL_MAXIMAL),
			option(slang::CompilerOptionName::DebugInformation, options.debug ? SLANG_DEBUG_INFO_LEVEL_MAXIMAL : SLANG_DEBUG_INFO_LEVEL_NONE),
			// forces scalar layout which is awesome, means we can have a header file that is shared by c++ and slang
			option(slang::CompilerOptionName::ForceCLayout, true),
		};

		// path::c_str() is wchar_t on Windows, slang wants char
		std::vector<std::string> searchPaths;
		searchPaths.reserve(options.searchPaths.size());
		for (const std::filesystem::path& path: options.searchPaths)
			searchPaths.push_back(path.string());
		std::vector<const char*> searchPathPointers;
		searchPathPointers.reserve(searchPaths.size());
		for (const std::string& path: searchPaths)
			searchPathPointers.push_back(path.c_str());

		const slang::SessionDesc sessionDesc = {
			.targets = &targetDesc,
			.targetCount = 1,
			.defaultMatrixLayoutMode = options.matrixLayout,
			.searchPaths = searchPathPointers.data(),
			.searchPathCount = static_cast<SlangInt>(searchPathPointers.size()),
			.compilerOptionEntries = entries.data(),
			.compilerOptionEntryCount = static_cast<uint32_t>(entries.size()),
		};
		if (SLANG_FAILED(compiler._globalSlangSession->createSession(sessionDesc, compiler._slangSession.writeRef()))) {
			diagnostics = "Slang failed to create session!";
			return std::nullopt;
		}

		Slang::ComPtr<slang::IBlob> bindlessDiagnostics;
		compiler._bindlessModule = compiler._slangSession->loadModuleFromSourceString("mythril_bindless", "mythril_bindless.slang", kBindlessModuleSource, bindlessDiagnostics.writeRef());
		if (!compiler._bindlessModule) {
			diagnostics = fmt::format("Failed to load Mythril's bindless module! Diagnostics Below:\n{}", BlobText(bindlessDiagnostics));
			return std::nullopt;
		}
		compiler._shaderSearchPaths = std::move(options.searchPaths);
		return compiler;
	}

	static bool DeclaresBindlessOverride(slang::DeclReflection* moduleDecl) {
		auto children = moduleDecl->getChildren();
		return std::any_of(children.begin(), children.end(), [](slang::DeclReflection* child) {
			const auto kind = child->getKind();
			if (kind != slang::DeclReflection::Kind::Func && kind != slang::DeclReflection::Kind::Generic)
				return false;
			const char* name = child->getName();
			return name && std::strcmp(name, kBindlessFunctionName) == 0;
		});
	}

	std::string SlangCompiler::findBindlessOverride() const {
		for (SlangInt i = 0; i < _slangSession->getLoadedModuleCount(); ++i) {
			slang::IModule* loaded = _slangSession->getLoadedModule(i);
			if (loaded == _bindlessModule.get() || !DeclaresBindlessOverride(loaded->getModuleReflection()))
				continue;
			return fmt::format(
				"Module '{}' defines '{}', but Mythril already provides it and a second definition would be silently ignored. Remove it from your shader.",
				loaded->getFilePath() ? loaded->getFilePath() : loaded->getName(),
				kBindlessFunctionName);
		}
		return {};
	}

	static std::string JoinActiveSearchPathsLog(const std::vector<std::filesystem::path>& paths) {
		std::string message;
		for (const auto& path: paths) {
			message += "'" + path.string() + "'" + "\n";
		}
		return message;
	}


	Slang::ComPtr<slang::IComponentType> SlangCompiler::linkFromDisk(const std::filesystem::path& filepath, std::string& diagnostics) {
		Slang::ComPtr<slang::IBlob> diagnostics_blob;
		Slang::ComPtr<slang::IModule> slang_module;
		slang_module = this->_slangSession->loadModule(filepath.string().c_str(), diagnostics_blob.writeRef());
		if (!slang_module) {
			diagnostics = fmt::format(
				"--------------------------- FILE: -------------------------- \n'{}'\n -------------------------- SEARCH_PATHS: -------------------------- \n{} -------------------------- ERROR: -------------------------- \nSlang failed to load module, this might happen for a bunch of different reasons like filepath couldnt be found or the shader is "
				"invalid.\nDiagnostics Below:\n{}",
				filepath.string(),
				JoinActiveSearchPathsLog(this->_shaderSearchPaths),
				BlobText(diagnostics_blob));
			return nullptr;
		}
		std::string warnings = BlobText(diagnostics_blob);
		diagnostics_blob.setNull();
		if (diagnostics = findBindlessOverride(); !diagnostics.empty())
			return nullptr;

		std::vector<Slang::ComPtr<slang::IComponentType>> componentTypes = {};
		componentTypes.emplace_back(slang_module);
		const int definedEntryPointCount = slang_module->getDefinedEntryPointCount();
		for (int i = 0; i < definedEntryPointCount; i++) {
			Slang::ComPtr<slang::IEntryPoint> entryPoint;
			if (SLANG_FAILED(slang_module->getDefinedEntryPoint(i, entryPoint.writeRef()))) {
				diagnostics = fmt::format("Entry point {} of '{}' could not be retrieved!", i, filepath.string());
				return nullptr;
			}
			componentTypes.emplace_back(entryPoint.get());
		}
		componentTypes.emplace_back(_bindlessModule);

		Slang::ComPtr<slang::IComponentType> composed_program;
		const SlangResult module_result = this->_slangSession->createCompositeComponentType(
			reinterpret_cast<slang::IComponentType**>(componentTypes.data()), static_cast<int>(componentTypes.size()), composed_program.writeRef(), diagnostics_blob.writeRef());
		if (SLANG_FAILED(module_result)) {
			diagnostics = fmt::format("Composition failed! Diagnostics Below:\n{}", BlobText(diagnostics_blob));
			return nullptr;
		}
		warnings += BlobText(diagnostics_blob);
		diagnostics_blob.setNull();

		Slang::ComPtr<slang::IComponentType> linked_program;
		if (SLANG_FAILED(composed_program->link(linked_program.writeRef(), diagnostics_blob.writeRef()))) {
			diagnostics = fmt::format("Linking failed! Diagnostics Below:\n{}", BlobText(diagnostics_blob));
			return nullptr;
		}
		diagnostics = warnings + BlobText(diagnostics_blob);
		return linked_program;
	}

	std::vector<std::filesystem::path> SlangCompiler::loadedDependencyFiles() const {
		std::vector<std::filesystem::path> files;
		for (SlangInt i = 0; i < _slangSession->getLoadedModuleCount(); ++i) {
			slang::IModule* loaded = _slangSession->getLoadedModule(i);
			if (loaded == _bindlessModule.get())
				continue;
			for (SlangInt32 file = 0; file < loaded->getDependencyFileCount(); ++file) {
				std::filesystem::path path = loaded->getDependencyFilePath(file);
				if (std::ranges::find(files, path) == files.end())
					files.push_back(std::move(path));
			}
		}
		return files;
	}

	static void CollectDeclarations(slang::DeclReflection* decl, const std::filesystem::path& file, const bool moduleScope, SlangCompiler::Declarations& out) {
		for (slang::DeclReflection* child: decl->getChildren()) {
			const auto kind = child->getKind();
			if (kind == slang::DeclReflection::Kind::Struct && child->getName())
				out.structs.emplace_back(child->getName(), file);
			if (kind == slang::DeclReflection::Kind::Struct || kind == slang::DeclReflection::Kind::Namespace)
				CollectDeclarations(child, file, false, out);
			if (kind != slang::DeclReflection::Kind::Variable || !moduleScope)
				continue;
			slang::VariableReflection* variable = child->asVariable();
			slang::TypeReflection* type = variable->getType();
			int64_t value = 0;
			if (!variable->findModifier(slang::Modifier::Static) || !variable->findModifier(slang::Modifier::Const) || type->getKind() != slang::TypeReflection::Kind::Scalar ||
				!variable->hasDefaultValue() || SLANG_FAILED(variable->getDefaultValueInt(&value)))
				continue;
			const auto scalar = type->getScalarType();
			if (scalar == slang::TypeReflection::ScalarType::Int32 || scalar == slang::TypeReflection::ScalarType::UInt32)
				out.constants.push_back({ variable->getName(), scalar == slang::TypeReflection::ScalarType::Int32, value, file });
		}
	}

	SlangCompiler::Declarations SlangCompiler::loadedDeclarations() const {
		Declarations declarations;
		for (SlangInt i = 0; i < _slangSession->getLoadedModuleCount(); ++i) {
			slang::IModule* loaded = _slangSession->getLoadedModule(i);
			if (loaded != _bindlessModule.get() && loaded->getFilePath())
				CollectDeclarations(loaded->getModuleReflection(), loaded->getFilePath(), true, declarations);
		}
		return declarations;
	}
} // namespace mythril
