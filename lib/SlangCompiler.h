//
// Created by Hayden Rivas on 11/7/25.
//

#pragma once
#include <slang/slang-com-ptr.h>
#include <slang/slang.h>

#include <filesystem>
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

	class SlangCompiler {
	public:
		SlangCompiler() = default;
		~SlangCompiler() = default;

		void create();
		void destroy();

		void addSearchPath(const std::filesystem::path& searchPath);
		void clearSearchPaths();

		CompileResult compileSlangFromDisk(const std::filesystem::path& filepath);
		CompileResult compileSlangFromSourceString(const std::string& code);

		bool sessionExists() const { return _sessionExists; }

	private:
		CompileResult compileSlangImpl(Slang::ComPtr<slang::IModule>& slang_module, Slang::ComPtr<slang::IBlob>& diagnostics_blob) const;
		void createSessionImpl();

		Slang::ComPtr<slang::ISession> _slangSession = nullptr;
		Slang::ComPtr<slang::IGlobalSession> _globalSlangSession = nullptr;
		bool _sessionExists = false;

		std::vector<std::filesystem::path> _shaderSearchPaths = {};
	};
} // namespace mythril
