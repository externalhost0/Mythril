//
// Created by Hayden Rivas on 11/1/25.
//

#pragma once

#include "Constants.h"

#include <cstdint>
#include <span>
#include <string>
#include <unordered_map>
#include <vector>

#include <volk.h>

namespace mythril {
    class CTX;
    class CommandBuffer;

    enum class ScalarKind {
        None,
        Int,
        UInt,
        Float,
        Double,
        Bool
    };

    // a specialization constant can only ever be a Scalar
    struct SpecializationConstant {
        std::string varName;
        ScalarKind scalarKind;
        uint32_t id;
        // bytes a pipeline must give, a bool takes a VkBool32
        uint32_t size;
    };

    // a field of a push block or of what a pointer in it points to, Slang's array and matrix wrapper structs are folded away
    struct PushField {
        enum class Kind : uint8_t { Scalar,
            Vector,
            Matrix,
            Array,
            Struct,
            Pointer,
            SampledTexture,
            StorageTexture,
            Sampler,
            AccelerationStructure,
            Unknown };
        // what the shader does through the field, Unknown when the SPIR-V walk lost the trace
        enum class Access : uint8_t { None = 0,
            Read = 1,
            Write = 2,
            ReadWrite = 3,
            Unknown = 4 };
        std::string name;
        // struct name without Slang's layout suffix
        std::string typeName;
        // from the start of the push block, or of the pointee for a field behind a pointer
        uint32_t offset = 0;
        uint32_t size = 0;
        Kind kind = Kind::Unknown;
        ScalarKind scalar = ScalarKind::None;
        uint8_t bits = 0;
        uint8_t rows = 0;
        uint8_t columns = 0;
        bool rowMajor = false;
        // array element count, 0 when unsized, and element or pointee stride
        uint32_t count = 0;
        uint32_t stride = 0;
        // what the shader does through a pointer or handle of the push block, Unknown in release builds and behind pointers
        Access access = Access::Unknown;
        // index into PushBlockLayout::pointees, a struct that points to itself points back to its own entry
        uint32_t pointee = UINT32_MAX;
        // struct members, or the one element of an array laid out at the array's offset
        std::vector<PushField> fields;
    };
    // a pointer or handle leaf of the push block, one per array element
    struct PushResourceField {
        std::string varName;
        uint32_t offset;
        PushField::Kind kind;
        PushField::Access access = PushField::Access::None;
    };
    struct PushBlockLayout {
        std::string variable;
        PushField root;
        std::vector<PushField> pointees;
        uint64_t hash;
        std::string description;
    };
    struct PipelineLayoutSignature {
        // the shader reads Slang's __slang_resource_heap, which the shader rules keep at set 0
        bool usesBindless = false;
        std::vector<VkPushConstantRange> pushes;
        std::vector<PushResourceField> pushResources;
        std::vector<PushBlockLayout> pushBlocks;
    };
    struct ShaderEntryPoint {
        std::string name;
        VkShaderStageFlagBits stage;
    };
    struct SpecializationInfo {
        std::vector<SpecializationConstant> specializationConstants;
        std::unordered_map<std::string, int> nameToID;
    };

    namespace detail {
        class AllocatedShader {
        public:
            // replaced on hot reload
            std::span<const PushBlockLayout> viewPushBlocks() const { return _pipelineSignature.pushBlocks; }
            const PipelineLayoutSignature& getPipelineLayoutSignature() const { return _pipelineSignature; }
            std::string_view getDebugName() const { return _debugName; }

        private:
            // a shader defines not only the module obviously, but a pipelineLayout
            VkShaderModule _vkShaderModule{};
            PipelineLayoutSignature _pipelineSignature;
            SpecializationInfo _specializationInfo; // holds both info and a map when varName is used to resolve ID
#ifdef DEBUG
            std::vector<ShaderEntryPoint> _entryPoints;
#endif

            char _debugName[kMaxDebugNameLength] = { 0 };

            friend class ::mythril::CTX;
            friend class ::mythril::CommandBuffer;
        };
    } // namespace detail

} // namespace mythril
