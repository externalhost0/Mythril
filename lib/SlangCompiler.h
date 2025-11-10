//
// Created by Hayden Rivas on 11/7/25.
//

#pragma once
#include <slang/slang.h>
#include <slang/slang-com-ptr.h>

#include <vector>
#include <string>
#include <filesystem>

namespace mythril {

	struct CompileResult {
		const uint32_t* getSpirvCode() const { return reinterpret_cast<const uint32_t*>(_spirvBlob->getBufferPointer()); };
		size_t getSpirvSize() const { return _spirvBlob->getBufferSize(); };

		slang::ProgramLayout* getProgramLayout() { return _programLayout; }

		explicit operator bool() const { return success; }
	private:
		Slang::ComPtr<slang::IBlob> _spirvBlob = nullptr;
		slang::ProgramLayout* _programLayout = nullptr;

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

		CompileResult compileFile(const std::filesystem::path& filepath);

		bool sessionExists() const { return _sessionExists; }
	private:
		void createSessionImpl();

		Slang::ComPtr<slang::ISession> _slangSession = nullptr;
		Slang::ComPtr<slang::IGlobalSession> _globalSlangSession = nullptr;
		bool _sessionExists = false;

		std::vector<std::filesystem::path> _shaderSearchPaths = {};


	};
}
