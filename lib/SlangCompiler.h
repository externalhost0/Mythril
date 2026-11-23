//
// Created by Hayden Rivas on 11/7/25.
//

#pragma once
#include <slang/slang-com-ptr.h>
#include <slang/slang.h>

#include "mythril/CTXBuilder.h"
#include "mythril/Shader.h"

#include <deque>
#include <filesystem>
#include <span>
#include <string>
#include <vector>

namespace mythril {

	struct CompileResult {
		const uint32_t* getSpirvCode() const { return reinterpret_cast<const uint32_t*>(_spirvBlob->getBufferPointer()); };
		size_t getSpirvSize() const { return _spirvBlob->getBufferSize(); };
		slang::ProgramLayout* getSlangProgramLayout() { return _programLayout; }

		explicit operator bool() const { return success; }

	private:
		Slang::ComPtr<slang::IBlob> _spirvBlob = nullptr;
		slang::ProgramLayout* _programLayout = nullptr;

		// needs to be stored to keep the lifetime of _programLayout
		Slang::ComPtr<slang::IComponentType> _linkedProgram = nullptr;
		bool success = false;

		friend class SlangCompiler;
	};

	std::vector<PushResourceField> CollectPushResources(slang::ProgramLayout* layout);

	class SlangCompiler {
	public:
		SlangCompiler() = default;
		~SlangCompiler() = default;

		void create();
		void destroy();

		void addSearchPath(const std::filesystem::path& searchPath);
		void clearSearchPaths();
		void addCompilerOptions(std::span<const slang::CompilerOptionEntry> options);
		void setMatrixLayoutMode(SlangCfg::MatrixMode mode);

		CompileResult compileSlangFromDisk(const std::filesystem::path& filepath);
		CompileResult compileSlangFromSourceString(const std::string& code);

		bool sessionExists() const { return _sessionExists; }

	private:
		CompileResult compileSlangImpl(Slang::ComPtr<slang::IModule>& slang_module, Slang::ComPtr<slang::IBlob>& diagnostics_blob) const;
		void createSessionImpl();
		void validateNoBindlessOverride() const;

		Slang::ComPtr<slang::ISession> _slangSession = nullptr;
		Slang::ComPtr<slang::IGlobalSession> _globalSlangSession = nullptr;
		Slang::ComPtr<slang::IModule> _bindlessModule = nullptr;
		bool _sessionExists = false;

		std::vector<std::filesystem::path> _shaderSearchPaths = {};
		SlangCfg::MatrixMode _matrixLayoutMode = SlangCfg::MatrixMode::ColMajor;
		std::vector<slang::CompilerOptionEntry> _userOptions = {};
		// owns the option strings, deque keeps them in place as it grows
		std::deque<std::string> _userOptionStrings = {};
	};
} // namespace mythril
