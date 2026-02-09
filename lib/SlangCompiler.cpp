//
// Created by Hayden Rivas on 11/7/25.
//

#include "SlangCompiler.h"
#include "HelperMacros.h"

#include <vector>
#include <array>
#include <fstream>
#include <filesystem>

#include "CTX.h"

namespace mythril {
	void SlangCompiler::create() {
		if (this->_sessionExists)
			return;
		createSessionImpl();
		_sessionExists = true;
	}
	void SlangCompiler::destroy() {
		if (!this->_sessionExists)
			return;
		_slangSession.detach();
		_globalSlangSession.detach();
		_sessionExists = false;
	}

	void SlangCompiler::addSearchPath(const std::filesystem::path& searchPath) {
		this->_shaderSearchPaths.push_back(searchPath);
	}
	void SlangCompiler::clearSearchPaths() {
		this->_shaderSearchPaths.clear();
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
			slang::CompilerOptionEntry{
				.name = slang::CompilerOptionName::BindlessSpaceIndex,
				.value = {
					.kind = slang::CompilerOptionValueKind::Int,
					.intValue0 = 0
				}
			},

			slang::CompilerOptionEntry{
				.name = slang::CompilerOptionName::VulkanUseEntryPointName,
				.value = {
					.kind = slang::CompilerOptionValueKind::Int,
					.intValue0 = true
				}
			},
			slang::CompilerOptionEntry{
				.name = slang::CompilerOptionName::Optimization,
				.value = {
					.kind = slang::CompilerOptionValueKind::Int,
					.intValue0 = SLANG_OPTIMIZATION_LEVEL_DEFAULT
				}
			},
			slang::CompilerOptionEntry{
				.name = slang::CompilerOptionName::VulkanInvertY,
				.value = {
					.kind = slang::CompilerOptionValueKind::Int,
					.intValue0 = true
				}
			},
			slang::CompilerOptionEntry{
				.name = slang::CompilerOptionName::DebugInformation,
				.value = {
					.kind = slang::CompilerOptionValueKind::Int,
					.intValue0 = SLANG_DEBUG_INFO_LEVEL_MAXIMAL
				}
			},
				// forces scalar layout which is awesome, means we can have a header file that is shared by c++ and slang
			slang::CompilerOptionEntry{
				.name = slang::CompilerOptionName::ForceCLayout,
				.value = {
					.kind = slang::CompilerOptionValueKind::Int,
					.intValue0 = true
				}
			},
		};

		ASSERT_MSG(this->_shaderSearchPaths.size() <= 16, "You cannot have more than 16 searchpaths in SlangCompiler, this is arbitrary btw lol");
		std::array<const char*, 16> sp_cstrings = {};
		int64_t sp_count = 0;
		for (const std::filesystem::path& path : this->_shaderSearchPaths) {
			sp_cstrings[sp_count++] = path.string().c_str();
		}

		const slang::SessionDesc sessionDesc = {
				.targets = &targetDesc,
				.targetCount = 1,

				.defaultMatrixLayoutMode = SLANG_MATRIX_LAYOUT_COLUMN_MAJOR,

				.searchPaths = sp_cstrings.data(),
				.searchPathCount = sp_count,

				.compilerOptionEntries = entries.data(),
				.compilerOptionEntryCount = entries.size(),
		};
		const SlangResult session_result = this->_globalSlangSession->createSession(sessionDesc, this->_slangSession.writeRef());
		ASSERT_MSG(SLANG_SUCCEEDED(session_result), "Slang failed to create session!");
	}

	static const char* ResolveDiagnosticsMessage(const Slang::ComPtr<slang::IBlob>& diagnostics_blob) {
		return diagnostics_blob ? static_cast<const char*>(diagnostics_blob->getBufferPointer()) : "(no diagnostics available)";
	}
	static std::string JoinActiveSearchPathsLog(const std::vector<std::filesystem::path>& paths) {
		std::string message;
		for (const auto& path : paths) {
			message += "'" + path.string() + "'" + "\n";
		}
		return message;
	}

	CompileResult SlangCompiler::compileSlangFile(const std::filesystem::path& filepath) {
		MYTH_PROFILER_FUNCTION();
		ASSERT(exists(filepath));
		// 1. load module
		Slang::ComPtr<slang::IModule> slang_module;
		Slang::ComPtr<slang::IBlob> diagnostics_blob;
		slang_module = this->_slangSession->loadModule(filepath.string().c_str(), diagnostics_blob.writeRef());
		ASSERT_MSG(slang_module,
			"FILE: \n'{}'\nSEARCH_PATHS: \n{}ERROR:\nSlang failed to load module, this might happen for a bunch of different reasons like filepath couldnt be found or the shader is invalid.\nDiagnostics Below:\n{}",
			filepath.string().c_str(),
			JoinActiveSearchPathsLog(this->_shaderSearchPaths),
			ResolveDiagnosticsMessage(diagnostics_blob));
		diagnostics_blob.setNull();

		// 2. query entry points
		// if you have more than 8 components we want to link than whatever man
		std::vector<Slang::ComPtr<slang::IComponentType>> componentTypes = {};
		componentTypes.emplace_back(slang_module);
		int definedEntryPointCount = slang_module->getDefinedEntryPointCount();
		for (int i = 0; i < definedEntryPointCount; i++) {
			Slang::ComPtr<slang::IEntryPoint> entryPoint;
			SlangResult entry_point_result = slang_module->getDefinedEntryPoint(i, entryPoint.writeRef());
			ASSERT_MSG(SLANG_SUCCEEDED(entry_point_result), "Entry point retrieval failed!");
			componentTypes.emplace_back(entryPoint.get());
		}

		// 3. compose module
		Slang::ComPtr<slang::IComponentType> composed_program;
		SlangResult module_result = this->_slangSession->createCompositeComponentType(
				(slang::IComponentType**)componentTypes.data(),
				(int)componentTypes.size(),
				composed_program.writeRef(),
				diagnostics_blob.writeRef());
		ASSERT_MSG(SLANG_SUCCEEDED(module_result), "Composition failed! Diagnostics Below:\n{}", ResolveDiagnosticsMessage(diagnostics_blob));
		diagnostics_blob.setNull();

		// 4. linking
		// linked_program needs to be kept alive as we want to access entrypoints, and therefore we store linked_program in the CompileResult return
		Slang::ComPtr<slang::IComponentType> linked_program;
		SlangResult compose_result = composed_program->link(linked_program.writeRef(), diagnostics_blob.writeRef());
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
		SlangResult code_result = linked_program->getTargetCode(0, spirvBlob.writeRef(), diagnostics_blob.writeRef());
		ASSERT_MSG(SLANG_SUCCEEDED(code_result), "Code retrieval failed! Diagnostics Below:\n{}", ResolveDiagnosticsMessage(diagnostics_blob));


		// 5+. bonus: retreive metadata to determine if parameters are used or not
		Slang::ComPtr<slang::IMetadata> target_metadata;
		SlangResult metadata_result = linked_program->getTargetMetadata(0, target_metadata.writeRef(), diagnostics_blob.writeRef());
		ASSERT_MSG(SLANG_SUCCEEDED(metadata_result), "Metadata retrieval Failed! Diagnostics Below:\n{}", ResolveDiagnosticsMessage(diagnostics_blob));

		CompileResult result;
		result._linkedProgram = linked_program;
		result._spirvBlob = spirvBlob;
		result._programLayout = program_layout;
		result.success = true;
		return result;
	}
}
