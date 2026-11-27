#include "PushLayout.h"
#include "ShaderReflection.h"
#include "SlangCompiler.h"
#include "SpirvLayout.h"

#include "accel.shadergen.h"
#include "spec.shadergen.h"
#include "valid.shadergen.h"

#include <doctest/doctest.h>
#include <fmt/format.h>

#include <algorithm>
#include <cstdlib>
#include <filesystem>
#include <optional>
#include <fstream>
#include <sstream>
#include <string>
#include <string_view>
#include <tuple>
#include <vector>

using namespace mythril;

namespace {
    const std::filesystem::path kCorpus = MYTH_PUSH_LAYOUT_DIR;

    struct Reflected {
        ReflectedTypes types;
        std::vector<uint32_t> spirv;
        std::vector<std::string> errors;
        std::string header;
    };

    Slang::ComPtr<slang::IComponentType> Link(const std::filesystem::path& path) {
        std::string diagnostics;
        static std::optional<SlangCompiler> compiler = SlangCompiler::Create({}, diagnostics);
        REQUIRE_MESSAGE(compiler, diagnostics);
        Slang::ComPtr<slang::IComponentType> linked = compiler->linkFromDisk(path, diagnostics);
        REQUIRE_MESSAGE(linked, diagnostics);
        return linked;
    }

    Reflected Reflect(const std::string_view shader) {
        const std::filesystem::path path = kCorpus / shader;
        const Slang::ComPtr<slang::IComponentType> linked = Link(path);
        Slang::ComPtr<slang::IBlob> spirv;
        Slang::ComPtr<slang::IBlob> codeDiagnostics;
        REQUIRE(SLANG_SUCCEEDED(linked->getTargetCode(0, spirv.writeRef(), codeDiagnostics.writeRef())));
        REQUIRE(spirv);

        Reflected out;
        StructRegistry structs;
        ReflectPushTypes(linked->getLayout(0), shader, structs, out.types);
        const auto* words = static_cast<const uint32_t*>(spirv->getBufferPointer());
        out.spirv.assign(words, words + spirv->getBufferSize() / sizeof(uint32_t));
        PushLayouts layouts;
        out.errors = CheckHeader(out.types, SpirvLayout(out.spirv), {}, layouts);
        out.header = EmitHeader(path.filename().string(), out.types, {}, layouts, {});
        return out;
    }

#ifdef DEBUG
    std::vector<PushBlockLayout> PushBlocks(const SpirvLayout& layout) {
        std::vector<PushBlockLayout> blocks;
        for (SpirvPushBlock& block: DescribeSpirvPushBlocks(layout))
            blocks.push_back(std::move(block.layout));
        return blocks;
    }
    using PushResource = std::tuple<std::string_view, uint32_t, PushField::Kind, PushField::Access>;
    void CheckPushResources(const std::span<const uint32_t> spirv, const std::vector<PushResource>& expected) {
        const SpirvLayout layout({ spirv.begin(), spirv.end() });
        std::vector<PushBlockLayout> blocks = PushBlocks(layout);
        const std::vector<PushResourceField> fields = AnalyzePushResources(layout, blocks);
        REQUIRE(fields.size() == expected.size());
        for (size_t i = 0; i < fields.size(); ++i) {
            CAPTURE(fields[i].varName);
            CHECK(PushResource(fields[i].varName, fields[i].offset, fields[i].kind, fields[i].access) == expected[i]);
        }
    }
#endif

    std::string ReadFile(const std::filesystem::path& path) {
        std::ifstream file(path, std::ios::binary);
        REQUIRE_MESSAGE(file, path.string());
        std::stringstream text;
        text << file.rdbuf();
        return text.str();
    }

    std::string Joined(const std::vector<std::string>& errors) {
        std::string out;
        for (const std::string& error: errors)
            out += "\n  " + error;
        return out;
    }
} // namespace

TEST_CASE("valid push layouts pass every check and generate their golden header") {
    for (const std::string_view shader: { "valid.slang", "spec.slang", "accel.slang" }) {
        CAPTURE(shader);
        const Reflected reflected = Reflect(shader);
        REQUIRE_FALSE(reflected.types.structs.empty());
        CHECK_MESSAGE(reflected.errors.empty(), Joined(reflected.errors));
        const std::filesystem::path golden = kCorpus / "golden" / (std::string(shader.substr(0, shader.find('.'))) + ".shadergen.h");
        if (std::getenv("MYTH_UPDATE_GOLDEN"))
            std::ofstream(golden, std::ios::binary | std::ios::trunc) << reflected.header;
        CHECK_MESSAGE(reflected.header == ReadFile(golden), "if the change is intended, regenerate with: MYTH_UPDATE_GOLDEN=1 mythril_tests");
    }
}

TEST_CASE("generated kLayoutHash matches the push block hash createShader records") {
    const auto check = [](const std::string_view shader, const uint64_t hash) {
        CAPTURE(shader);
        const std::vector<SpirvPushBlock> blocks = DescribeSpirvPushBlocks(SpirvLayout(Reflect(shader).spirv));
        REQUIRE(blocks.size() == 1);
        CHECK(blocks[0].layout.hash == hash);
    };
    check("valid.slang", shaders::Push::kLayoutHash);
    check("spec.slang", shaders::SpecPush::kLayoutHash);
    check("accel.slang", shaders::AccelPush::kLayoutHash);
}

TEST_CASE("an id bound above the word count is valid SPIR-V, as spirv-opt leaves it without --compact-ids") {
    std::vector<uint32_t> spirv = Reflect("valid.slang").spirv;
    const uint64_t hash = DescribeSpirvPushBlocks(SpirvLayout(spirv)).at(0).layout.hash;
    spirv[3] = static_cast<uint32_t>(spirv.size()) + 1000;
    const SpirvLayout gapped(spirv);
    CHECK(gapped.valid);
    CHECK(CheckSPIRVRules(gapped).empty());
    const std::vector<SpirvPushBlock> blocks = DescribeSpirvPushBlocks(gapped);
    REQUIRE(blocks.size() == 1);
    CHECK(blocks[0].layout.hash == hash);
    spirv[3] = 4'194'304;
    CHECK_FALSE(SpirvLayout(spirv).valid);
}

TEST_CASE("push field tree keeps nesting, folds Slang wrappers and follows pointers") {
    using Kind = PushField::Kind;
    std::vector<SpirvPushBlock> blocks = DescribeSpirvPushBlocks(SpirvLayout(Reflect("valid.slang").spirv));
    REQUIRE(blocks.size() == 1);
    const PushBlockLayout& block = blocks[0].layout;
    const PushField& root = block.root;
    CHECK(block.variable == "push");
    CHECK(root.kind == Kind::Struct);
    CHECK(root.typeName == "Push");
    REQUIRE(root.fields.size() == 11);

    const PushField& pm34 = root.fields[3];
    CHECK(pm34.kind == Kind::Matrix);
    CHECK(std::tuple(pm34.rows, pm34.columns, pm34.rowMajor, pm34.scalar, pm34.size) == std::tuple(3, 4, true, ScalarKind::Float, 48u));

    const PushField& inner = root.fields[4];
    CHECK(inner.typeName == "Inner");
    REQUIRE(inner.fields.size() == 2);
    CHECK(std::tuple(inner.fields[1].name, inner.fields[1].offset) == std::tuple("s", 84u));

    const PushField& texArr = root.fields[8];
    CHECK(std::tuple(texArr.kind, texArr.count, texArr.stride, texArr.fields.size()) == std::tuple(Kind::Array, 3u, 8u, 1u));

    const PushField& target = root.fields[0];
    REQUIRE(target.kind == Kind::Pointer);
    const PushField& targetType = block.pointees.at(target.pointee);
    CHECK(targetType.typeName == "Target");
    CHECK(targetType.fields[1].bits == 16);
    CHECK(targetType.fields[5].fields.front().typeName == "Inner");

    const PushField& list = root.fields[1];
    const PushField& node = block.pointees.at(list.pointee);
    CHECK(node.typeName == "Node");
    CHECK(node.fields[0].pointee == list.pointee);

    CHECK(block.pointees.at(root.fields[2].pointee).kind == Kind::Matrix);
#ifdef DEBUG
    PushBlockLayout attached = block;
    AnalyzePushResources(SpirvLayout(Reflect("valid.slang").spirv), std::span(&attached, 1));
    using Access = PushField::Access;
    const auto kindAccess = [&](const size_t i) {
        return std::tuple(attached.root.fields[i].kind, attached.root.fields[i].access);
    };
    CHECK(kindAccess(0) == std::tuple(Kind::Pointer, Access::ReadWrite));
    CHECK(kindAccess(5) == std::tuple(Kind::SampledTexture, Access::Read));
    CHECK(kindAccess(6) == std::tuple(Kind::StorageTexture, Access::Write));
    CHECK(kindAccess(7) == std::tuple(Kind::Sampler, Access::Read));
    CHECK(attached.root.fields[8].fields.front().kind == Kind::SampledTexture);
#endif
}

TEST_CASE("push block hashes differ when only the matrix layout differs") {
    const auto hash = [](const SlangMatrixLayoutMode mode) {
        std::string diagnostics;
        std::optional<SlangCompiler> compiler = SlangCompiler::Create({
                                                                          .matrixLayout = mode,
                                                                      },
            diagnostics);
        REQUIRE_MESSAGE(compiler, diagnostics);
        const Slang::ComPtr<slang::IComponentType> linked = compiler->linkFromDisk(kCorpus / "square.slang", diagnostics);
        REQUIRE_MESSAGE(linked, diagnostics);
        Slang::ComPtr<slang::IBlob> spirv;
        Slang::ComPtr<slang::IBlob> codeDiagnostics;
        REQUIRE(SLANG_SUCCEEDED(linked->getTargetCode(0, spirv.writeRef(), codeDiagnostics.writeRef())));
        const std::vector<SpirvPushBlock> blocks = DescribeSpirvPushBlocks(SpirvLayout(std::vector<uint32_t>(static_cast<const uint32_t*>(spirv->getBufferPointer()), static_cast<const uint32_t*>(spirv->getBufferPointer()) + spirv->getBufferSize() / sizeof(uint32_t))));
        REQUIRE(blocks.size() == 1);
        return blocks[0].layout.hash;
    };
    CHECK(hash(SLANG_MATRIX_LAYOUT_ROW_MAJOR) != hash(SLANG_MATRIX_LAYOUT_COLUMN_MAJOR));
}

TEST_CASE("rejected push layouts report exactly their named errors") {
    struct Rejected {
        std::string_view shader;
        std::vector<std::string_view> errors;
    };
    const std::vector<Rejected> corpus = {
        { "edge.slang",
            {
                "entry point parameter 'cs_main::epUniform' is uniform",
                "'Target::flag' is a bool",
                "'Target::big' starts at byte 288, so bytes 284..288 are implicit padding",
                "struct 'Target' ends at byte 300 but its stride is 304",
                "'Push::target' starts at byte 8, so bytes 4..8 are implicit padding",
                "'Push::pflag' is a bool",
                "'Push::inner' starts at byte 68, so bytes 65..68 are implicit padding",
                "'Push::u64' starts at byte 88, so bytes 84..88 are implicit padding",
                "struct 'Push' ends at byte 100 but its stride is 104",
                "'Target::rm34' is column_major but the session is row-major",
            } },
        // the reflection and SPIR-V disagreements are Slang issue 13188, they go away when Slang fixes it
        { "ptrc.slang",
            {
                "'S::a' is a bool",
                "'S::b' is a bool",
                "struct 'X' ends at byte 12 but its stride is 16",
                "'Push::pa' is a bool",
                "'Push::pb' is a bool",
                "'Push::pc' starts at byte 20, so bytes 18..20 are implicit padding",
                "'Push::s' points to elements 12 bytes apart in reflection but 8 in the SPIR-V",
                "'S::b' is at byte 4 in reflection but 1 in the SPIR-V",
                "'S::c' is at byte 8 in reflection but 4 in the SPIR-V",
                "'Push::y' points to elements 16 bytes apart in reflection but 24 in the SPIR-V",
            } },
        { "names.slang",
            {
                "two different structs are named 'Light'",
                "two different structs are named 'Pair'",
            } },
        { "generic.slang", { "two different structs are named 'Pair'" } },
        { "unsized.slang", { "'Buf::data' is the unsized array 'float[]'" } },
        { "vectors.slang",
            {
                "'Push::h' has a type with no C++ equivalent",
                "'Push::s' has a type with no C++ equivalent",
                "'Push::d' has a type with no C++ equivalent",
                "'Push::m' has a type with no C++ equivalent",
            } },
    };
    for (const Rejected& rejected: corpus) {
        CAPTURE(rejected.shader);
        const Reflected reflected = Reflect(rejected.shader);
        INFO("errors:", Joined(reflected.errors));
        CHECK(reflected.errors.size() == rejected.errors.size());
        for (const std::string_view expected: rejected.errors) {
            CAPTURE(expected);
            CHECK(std::ranges::count_if(reflected.errors, [&](const std::string& error) {
                return error.find(expected) != std::string::npos;
            }) == 1);
        }
    }
}

#ifdef DEBUG
TEST_CASE("push resources passed through helper functions keep their kind and access") {
    // the library session inlines every helper, raw SPIR-V from elsewhere may not
    const Slang::ComPtr<slang::IComponentType> linked = Link(kCorpus / "calls.slang");
    slang::CompilerOptionEntry noOptimization{
        .name = slang::CompilerOptionName::Optimization,
        .value = {
            .kind = slang::CompilerOptionValueKind::Int,
            .intValue0 = SLANG_OPTIMIZATION_LEVEL_NONE,
        },
    };
    Slang::ComPtr<slang::IComponentType> unoptimized;
    Slang::ComPtr<slang::IBlob> diagnostics;
    REQUIRE(SLANG_SUCCEEDED(linked->linkWithOptions(unoptimized.writeRef(), 1, &noOptimization, diagnostics.writeRef())));
    Slang::ComPtr<slang::IBlob> spirv;
    REQUIRE(SLANG_SUCCEEDED(unoptimized->getTargetCode(0, spirv.writeRef(), diagnostics.writeRef())));
    const auto* words = static_cast<const uint32_t*>(spirv->getBufferPointer());
    const std::span<const uint32_t> code(words, spirv->getBufferSize() / sizeof(uint32_t));

    size_t calls = 0;
    for (size_t i = 5; i < code.size(); i += code[i] >> 16)
        calls += (code[i] & 0xffff) == SpvOpFunctionCall;
    REQUIRE(calls == 3);

    using Kind = PushField::Kind;
    using Access = PushField::Access;
    CheckPushResources(code, {
                                 { "tex", 0, Kind::SampledTexture, Access::Read },
                                 { "img", 8, Kind::StorageTexture, Access::Write },
                                 { "smp", 16, Kind::Sampler, Access::Read },
                                 { "out", 24, Kind::Pointer, Access::Write },
                             });
}

TEST_CASE("push resource handles take the name of the field that holds them") {
    using Kind = PushField::Kind;
    using Access = PushField::Access;
    CheckPushResources(Reflect("handles.slang").spirv, {
                                                           { "smp", 0, Kind::Sampler, Access::Read },
                                                           { "arr", 16, Kind::SampledTexture, Access::Read },
                                                           { "h", 24, Kind::SampledTexture, Access::Read },
                                                           { "out", 40, Kind::Pointer, Access::Write },
                                                       });
}

TEST_CASE("push resource acceleration structure handles are found through their 64-bit address") {
    using Kind = PushField::Kind;
    using Access = PushField::Access;
    CheckPushResources(Reflect("accel.slang").spirv, {
                                                         { "tlas", 0, Kind::AccelerationStructure, Access::Read },
                                                         { "out", 16, Kind::Pointer, Access::Write },
                                                     });
}

TEST_CASE("push resource handles keep their kind through struct copies and locals") {
    using Kind = PushField::Kind;
    using Access = PushField::Access;
    CheckPushResources(Reflect("locals.slang").spirv, {
                                                          { "tex", 0, Kind::SampledTexture, Access::Read },
                                                          { "tex", 16, Kind::SampledTexture, Access::Read },
                                                          { "a", 32, Kind::SampledTexture, Access::Read },
                                                          { "b", 40, Kind::SampledTexture, Access::Read },
                                                          { "smp", 48, Kind::Sampler, Access::Read },
                                                          { "out", 56, Kind::Pointer, Access::Write },
                                                      });
}
#endif
