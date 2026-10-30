//
// Created by Hayden Rivas on 9/27/26.
//

#pragma once
#include <string_view>

#include "Constants.h"

#include <volk.h>

namespace mythril {
	class CTX;
	class CommandBuffer;

	struct AccelerationStructureGeometry {
		BufferHandle vertexBuffer;
		size_t vertexStride;
		VkFormat vertexFormat;
		uint32_t numVertices;

		BufferHandle indexBuffer;
		VkIndexType indexType = VK_INDEX_TYPE_UINT32;

		std::optional<BufferHandle> transformBuffer;
	};
	struct BottomLevelBuildDesc {
		VkGeometryTypeKHR geometryType;
		AccelerationStructureGeometry geometry;
		VkGeometryFlagsKHR geometryFlags = 0;
		uint32_t maxPrimitiveCount = 0;
		VkBuildAccelerationStructureFlagsKHR buildFlags = 0;
	};
	struct TopLevelBuildDesc {
		BufferHandle instancesBuffer;
		VkGeometryFlagsKHR geometryFlags = 0;
		uint32_t maxPrimitiveCount = 0;
		VkBuildAccelerationStructureFlagsKHR buildFlags = 0;
	};

	struct AccelerationStructureSpec {
		enum class Type {
			INVALID,
			BLAS,
			TLAS
		};
		Type type = Type::INVALID;
		enum class GeometryType {
			INVALID,
			Triangle,
			AABB,
			Instances
		};
		GeometryType geometryType = GeometryType::INVALID;
		VkGeometryFlagsKHR geometryFlags = 0;
		struct BuildRange {
			uint32_t primitiveCount = 0;
			uint32_t primitiveOffset = 0;
			uint32_t firstVertex = 0;
			uint32_t transformOffset = 0;
		};
		BufferHandle vertexBuffer;
		uint32_t numVertices;
		size_t vertexStride;
		VkFormat vertexFormat;

		BufferHandle indexBuffer;
		VkIndexType indexType = VK_INDEX_TYPE_UINT32;

		std::optional<BufferHandle> transformBuffer;
		BufferHandle instancesBuffer;

		BuildRange buildRange;
		VkBuildAccelerationStructureFlagsKHR buildFlags = 0;
		const char* debugName = "Unnamed Acceleration Structure";
	};


	namespace detail {
		class AllocatedAccelerationStructure {
		public:

			std::string_view getDebugName() const { return _debugName; }
		private:

			VkAccelerationStructureKHR _vkAccelerationStructure = VK_NULL_HANDLE;
			Buffer _buffer;
			Buffer _scratchBuffer;
			VkDeviceAddress _vkDeviceAddress;
			bool _isTLAS = false;

			char _debugName[kMaxDebugNameLength] = {0};
			friend class ::mythril::CTX;
			friend class ::mythril::CommandBuffer;
		};
	}
}
