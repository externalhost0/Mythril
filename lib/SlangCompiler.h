//
// Created by Hayden Rivas on 11/7/25.
//

#pragma once
#include <slang/slang-com-ptr.h>
#include <slang/slang.h>

#include <filesystem>
#include <optional>
#include <string>
#include <utility>
#include <vector>

namespace mythril {

    class SlangCompiler {
    public:
        struct Options {
            std::vector<std::filesystem::path> searchPaths;
            SlangMatrixLayoutMode matrixLayout = SLANG_MATRIX_LAYOUT_ROW_MAJOR;
            // full debug info and default optimization, otherwise maximal optimization
            bool debug = false;
        };
        static std::optional<SlangCompiler> Create(Options options, std::string& diagnostics);

        Slang::ComPtr<slang::IComponentType> linkFromDisk(const std::filesystem::path& filepath, std::string& diagnostics);
        std::vector<std::filesystem::path> loadedDependencyFiles() const;
        struct Constant {
            std::string name;
            bool isSigned;
            int64_t value;
            std::filesystem::path file;
        };
        struct Declarations {
            std::vector<std::pair<std::string, std::filesystem::path>> structs;
            // module scope static const integers
            std::vector<Constant> constants;
        };
        Declarations loadedDeclarations() const;
        // only the modules a loaded shader file reaches, so same-named structs of unrelated shaders stay apart
        Declarations declarationsReachableFrom(const std::filesystem::path& shader) const;

    private:
        SlangCompiler() = default;
        std::string findBindlessOverride() const;

        Slang::ComPtr<slang::IGlobalSession> _globalSlangSession;
        Slang::ComPtr<slang::ISession> _slangSession;
        Slang::ComPtr<slang::IModule> _bindlessModule;
        std::vector<std::filesystem::path> _shaderSearchPaths;
    };
} // namespace mythril
