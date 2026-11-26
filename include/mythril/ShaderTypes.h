#pragma once

namespace mythril::shader {
	struct float2 { float x, y; };
	struct float3 { float x, y, z; };
	struct float4 { float x, y, z, w; };
	struct int2 { int x, y; };
	struct int3 { int x, y, z; };
	struct int4 { int x, y, z, w; };
	struct uint2 { unsigned x, y; };
	struct uint3 { unsigned x, y, z; };
	struct uint4 { unsigned x, y, z, w; };

	// storage order follows the Slang matrix layout mode
	struct float2x2 { float m[4]; };
	struct float2x3 { float m[6]; };
	struct float2x4 { float m[8]; };
	struct float3x2 { float m[6]; };
	struct float3x3 { float m[9]; };
	struct float3x4 { float m[12]; };
	struct float4x2 { float m[8]; };
	struct float4x3 { float m[12]; };
	struct float4x4 { float m[16]; };
} // namespace mythril::shader
