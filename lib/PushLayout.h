#pragma once

#include "SlangCompiler.h"
#include "mythril/Shader.h"

#include <slang/slang.h>

#include <cstdint>
#include <map>
#include <span>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace mythril {
    struct ReflectedType {
        enum class Kind : uint8_t { Scalar,
            Vector,
            Matrix,
            Struct,
            SampledTexture,
            StorageTexture,
            Sampler,
            AccelerationStructure,
            Unsupported };
        Kind kind = Kind::Unsupported;
        slang::TypeReflection::ScalarType scalar = slang::TypeReflection::ScalarType::None;
        uint32_t rows = 0;
        uint32_t columns = 0;
        bool rowMajor = false;
        std::string name;
        bool pointer = false;
        bool unsizedArray = false;
        uint32_t arrayCount = 0;
        uint32_t arrayStride = 0;
        // namespace the struct lives in under HeaderOptions::fileNamespaces, empty otherwise
        std::string scope;
    };
    struct ReflectedField {
        std::string name;
        uint32_t offset;
        uint32_t size;
        ReflectedType type;
    };
    struct ReflectedStruct {
        std::string name;
        std::string scope;
        uint32_t size;
        uint32_t stride;
        uint32_t alignment;
        std::vector<ReflectedField> fields;
    };
    struct ReflectedPushBlock {
        std::string variable;
        std::string type;
    };
    struct ReflectedTypes {
        std::vector<ReflectedStruct> structs;
        std::vector<ReflectedPushBlock> pushBlocks;
        std::vector<std::string> conflicts;
        // entry point uniform parameters, which Slang puts in a push block of their own
        std::vector<std::string> entryPointUniforms;

        const ReflectedStruct* find(std::string_view name) const;
    };
    // the file stem a struct is generated under, byName maps a struct to its declaring file and fallback covers the rest, both empty when there are no file namespaces
    struct StructScopes {
        std::map<std::string, std::string, std::less<>> byName;
        std::string fallback;
    };
    inline std::string QualifiedName(const std::string_view scope, const std::string_view name) { return scope.empty() ? std::string(name) : std::string(scope) + "::" + std::string(name); }
    // qualified struct name to its layout description and the shader that first declared it, shared by every shader of one run
    using StructRegistry = std::map<std::string, std::pair<std::string, std::string>, std::less<>>;
    void ReflectPushTypes(slang::ProgramLayout* layout, std::string_view origin, StructRegistry& registry, ReflectedTypes& out, const StructScopes& scopes = {});
    // adds the named structs that the push blocks do not reach, returns a message per struct that cannot be generated
    std::vector<std::string> ReflectDeclaredStructs(slang::ProgramLayout* layout, std::span<const std::string> names, std::string_view origin, StructRegistry& registry, ReflectedTypes& out, const StructScopes& scopes = {});
    std::string DescribeStruct(const ReflectedTypes& types, const ReflectedStruct& type);

    struct HeaderOptions {
        std::string typesHeader = "mythril/ShaderTypes.h";
        std::string typesNamespace = "mythril::shader";
        std::string ns = "shaders";
        // generate the structs a file declares in <ns>::<file stem> instead of <ns>, so two files can declare the same name
        bool fileNamespaces = false;
        SlangMatrixLayoutMode matrixMode = SLANG_MATRIX_LAYOUT_ROW_MAJOR;
    };
    using PushLayouts = std::map<std::string, PushBlockLayout, std::less<>>;
    class SpirvLayout;
    std::vector<std::string> CheckHeader(const ReflectedTypes& types, const SpirvLayout& layout, const HeaderOptions& options, PushLayouts& pushLayouts);
    std::string HeaderBanner(std::string_view sourceName);
    std::string EmitHeader(std::string_view sourceName, const ReflectedTypes& types, std::span<const std::string> includes, const PushLayouts& pushLayouts, const HeaderOptions& options);
    struct ModuleData {
        std::string name;
        // empty for a module that is only imported
        std::string spirvFile;
        std::vector<SlangCompiler::Constant> constants;
        std::vector<uint32_t> threadGroupSize;
    };
    std::string EmitModuleData(const ModuleData& data, const HeaderOptions& options);
} // namespace mythril
