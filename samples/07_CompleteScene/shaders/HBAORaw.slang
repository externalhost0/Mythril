// HBAO+ Compute Shader
// This demonstrates compute shader concepts while implementing HBAO+

import GPUStructs;

struct HBAOPushConstants {
    DescriptorHandle<Texture2D> depthTexture;      // Linear depth in view space
    DescriptorHandle<Texture2D> normalTexture;     // View-space normals
    DescriptorHandle<RWTexture2D> aoOutput;        // Output AO texture
    DescriptorHandle<SamplerState> pointSampler;   // Point sampling
    DescriptorHandle<SamplerState> linearSampler;  // Linear sampling for normals
    
    float radius;           // AO radius in world space (e.g., 0.5)
    float angleBias;        // Bias angle to reduce self-occlusion (e.g., 0.1)
    float aoMultiplier;     // AO intensity (e.g., 2.0)
    int numDirections;      // Typically 8
    int numSteps;           // Typically 4
    uint frameCount;        // For temporal dithering
}

[vk::push_constant]
HBAOPushConstants push;

ParameterBlock<FrameData> frame;

// COMPUTE SHADER CONCEPT 1: Thread Group Size
// [numthreads(x, y, z)] defines how many threads run per workgroup
// - Common choices: (8,8,1), (16,16,1), (32,32,1)
// - Total threads per group = x * y * z (max usually 1024)
// - Trade-off: larger groups = better occupancy, but less flexibility
#define GROUP_SIZE 8

// COMPUTE SHADER CONCEPT 2: Shared Memory (LDS - Local Data Share)
// Shared memory is visible to all threads in a workgroup
// - Much faster than global memory (texture reads)
// - Limited size (typically 16-64 KB per workgroup)
// - Used for caching frequently accessed data
groupshared float sharedDepth[GROUP_SIZE + 2][GROUP_SIZE + 2];  // Extra border for sampling
groupshared float3 sharedNormal[GROUP_SIZE + 2][GROUP_SIZE + 2];

static const float PI = 3.14159265359;

// Reconstruct view-space position from LINEAR depth
// Input: linearDepth should be actual view-space Z distance (not hardware depth!)
float3 GetViewPosition(float2 uv, float linearDepth) {
    // IMPORTANT: This assumes linearDepth is ALREADY in view space
    // Not hardware depth buffer values!
    
    // Method 1: If linearDepth is actual view Z distance
    // We can reconstruct X,Y from UV and known Z
    float2 ndc = uv * 2.0 - 1.0;  // UV to NDC
    
    // For perspective projection:
    // x_view = x_ndc * z_view / projection[0][0]
    // y_view = y_ndc * z_view / projection[1][1]
    
    float3 viewPos;
    viewPos.z = -linearDepth;  // Negate because camera looks down -Z
    viewPos.x = ndc.x * linearDepth * frame.camera.invProj[0][0];
    viewPos.y = ndc.y * linearDepth * frame.camera.invProj[1][1];
    
    return viewPos;
}

// Interleaved gradient noise for temporal dithering
float InterleavedGradientNoise(uint2 pos, uint frameCount) {
    // CONCEPT: Deterministic pseudo-random per pixel
    // Changes per frame to reduce banding with temporal filtering
    pos += uint2(frameCount % 4, frameCount / 4);
    float3 magic = float3(0.06711056, 0.00583715, 52.9829189);
    return frac(magic.z * frac(dot(float2(pos), magic.xy)));
}

// COMPUTE SHADER CONCEPT 3: Thread IDs
// - SV_GroupThreadID: Position within workgroup (0 to GROUP_SIZE-1)
// - SV_GroupID: Which workgroup this is
// - SV_DispatchThreadID: Global thread ID = GroupID * GROUP_SIZE + GroupThreadID
[shader("compute")]
[numthreads(GROUP_SIZE, GROUP_SIZE, 1)]
void cs_main(
    uint3 groupThreadID : SV_GroupThreadID,      // Local position in group
    uint3 groupID : SV_GroupID,                  // Which workgroup
    uint3 dispatchThreadID : SV_DispatchThreadID // Global thread position
) {
    // Get output dimensions
    uint2 outputCoord = dispatchThreadID.xy;
    int2 dimensions;
    push.aoOutput.GetDimensions(dimensions.x, dimensions.y);
    
    // COMPUTE SHADER CONCEPT 4: Bounds Checking
    // Not all threads may map to valid pixels (if image size isn't multiple of GROUP_SIZE)
    if (outputCoord.x >= uint(dimensions.x) || outputCoord.y >= uint(dimensions.y)) {
        return;
    }
    
    // COMPUTE SHADER CONCEPT 5: Cooperative Loading to Shared Memory
    // Strategy: Each thread loads one pixel into shared memory
    // We load a (GROUP_SIZE+2)x(GROUP_SIZE+2) region to handle borders
    
    // Calculate which tile of shared memory to fill
    uint2 sharedCoord = groupThreadID.xy + uint2(1, 1); // +1 for border
    uint2 loadCoord = groupID.xy * GROUP_SIZE + groupThreadID.xy;
    
    // Load center data
    if (loadCoord.x < uint(dimensions.x) && loadCoord.y < uint(dimensions.y)) {
        float2 uv = (float2(loadCoord) + 0.5) / dimensions;
        sharedDepth[sharedCoord.y][sharedCoord.x] = push.depthTexture.SampleLevel(push.pointSampler, uv, 0).r;
        sharedNormal[sharedCoord.y][sharedCoord.x] = push.normalTexture.SampleLevel(push.linearSampler, uv, 0).xyz * 2.0 - 1.0;
    }
    
    // Load border pixels (only some threads do this)
    // Left border
    if (groupThreadID.x == 0 && groupID.x > 0) {
        uint2 borderCoord = loadCoord - uint2(1, 0);
        float2 uv = (float2(borderCoord) + 0.5) / dimensions;
        sharedDepth[sharedCoord.y][0] = push.depthTexture.SampleLevel(push.pointSampler, uv, 0).r;
        sharedNormal[sharedCoord.y][0] = push.normalTexture.SampleLevel(push.linearSampler, uv, 0).xyz * 2.0 - 1.0;
    }
    
    // Right border
    if (groupThreadID.x == GROUP_SIZE - 1 && loadCoord.x + 1 < uint(dimensions.x)) {
        uint2 borderCoord = loadCoord + uint2(1, 0);
        float2 uv = (float2(borderCoord) + 0.5) / dimensions;
        sharedDepth[sharedCoord.y][GROUP_SIZE + 1] = push.depthTexture.SampleLevel(push.pointSampler, uv, 0).r;
        sharedNormal[sharedCoord.y][GROUP_SIZE + 1] = push.normalTexture.SampleLevel(push.linearSampler, uv, 0).xyz * 2.0 - 1.0;
    }
    
    // Top/bottom borders (similar logic)
    if (groupThreadID.y == 0 && groupID.y > 0) {
        uint2 borderCoord = loadCoord - uint2(0, 1);
        float2 uv = (float2(borderCoord) + 0.5) / dimensions;
        sharedDepth[0][sharedCoord.x] = push.depthTexture.SampleLevel(push.pointSampler, uv, 0).r;
        sharedNormal[0][sharedCoord.x] = push.normalTexture.SampleLevel(push.linearSampler, uv, 0).xyz * 2.0 - 1.0;
    }
    
    if (groupThreadID.y == GROUP_SIZE - 1 && loadCoord.y + 1 < uint(dimensions.y)) {
        uint2 borderCoord = loadCoord + uint2(0, 1);
        float2 uv = (float2(borderCoord) + 0.5) / dimensions;
        sharedDepth[GROUP_SIZE + 1][sharedCoord.x] = push.depthTexture.SampleLevel(push.pointSampler, uv, 0).r;
        sharedNormal[GROUP_SIZE + 1][sharedCoord.x] = push.normalTexture.SampleLevel(push.linearSampler, uv, 0).xyz * 2.0 - 1.0;
    }
    
    // COMPUTE SHADER CONCEPT 6: Synchronization
    // GroupMemoryBarrierWithGroupSync() ensures:
    // 1. All memory writes are visible to all threads
    // 2. All threads reach this point before continuing
    GroupMemoryBarrierWithGroupSync();
    
    // Now all threads can safely read from shared memory
    float centerDepth = sharedDepth[sharedCoord.y][sharedCoord.x];
    float3 centerNormal = normalize(sharedNormal[sharedCoord.y][sharedCoord.x]);
    
    // Skip skybox/far plane
    if (centerDepth >= 0.9999) {
        push.aoOutput[outputCoord] = float4(1, 1, 1, 1);
        return;
    }
    
    // Reconstruct view position
    float2 uv = (float2(outputCoord) + 0.5) / dimensions;
    float3 origin = GetViewPosition(uv, centerDepth);
    
    // Build tangent space for sampling
    float3 tangent;
    float3 c1 = cross(centerNormal, float3(0, 0, 1));
    float3 c2 = cross(centerNormal, float3(0, 1, 0));
    tangent = length(c1) > length(c2) ? c1 : c2;
    tangent = normalize(tangent);
    float3 bitangent = cross(centerNormal, tangent);
    
    // Random rotation per pixel (temporally varying)
    float randomAngle = InterleavedGradientNoise(outputCoord, push.frameCount) * 2.0 * PI;
    
    float ao = 0.0;
    float angleStep = (2.0 * PI) / float(push.numDirections);
    
    // HBAO+ Algorithm: Sample in multiple directions
    for (int dir = 0; dir < push.numDirections; dir++) {
        float angle = angleStep * float(dir) + randomAngle;
        float2 direction = float2(cos(angle), sin(angle));
        
        // Convert to view-space direction
        float3 sliceDir = direction.x * tangent + direction.y * bitangent;
        
        float maxHorizonAngle = -PI * 0.5;
        
        // March along direction
        for (int step = 1; step <= push.numSteps; step++) {
            float stepScale = float(step) / float(push.numSteps);
            float3 samplePos = origin + sliceDir * push.radius * stepScale;
            
            // Project to screen space
            float4 projPos = mul(frame.camera.proj, float4(samplePos, 1.0));
            projPos.xyz /= projPos.w;
            float2 sampleUV = projPos.xy * 0.5 + 0.5;
            
            // Bounds check
            if (any(sampleUV < 0.0) || any(sampleUV > 1.0))
                continue;
            
            // Sample depth (from texture, not shared memory as it may be outside workgroup)
            float sampleDepth = push.depthTexture.SampleLevel(push.pointSampler, sampleUV, 0).r;
            float3 sampleViewPos = GetViewPosition(sampleUV, sampleDepth);
            
            // Calculate horizon angle
            float3 horizonVec = sampleViewPos - origin;
            float horizonDist = length(horizonVec);
            float horizonAngle = asin(clamp(dot(horizonVec / horizonDist, centerNormal), -1.0, 1.0));
            
            // Distance attenuation
            float attenuation = 1.0 - clamp(horizonDist / push.radius, 0.0, 1.0);
            attenuation *= attenuation;
            
            // Update max angle with attenuation
            horizonAngle = lerp(-PI * 0.5, horizonAngle, attenuation);
            maxHorizonAngle = max(maxHorizonAngle, horizonAngle);
        }
        
        // Accumulate occlusion
        float sinAngle = sin(maxHorizonAngle + push.angleBias);
        ao += clamp(sinAngle, 0.0, 1.0);
    }
    
    // Normalize and invert (1.0 = no occlusion, 0.0 = full occlusion)
    ao /= float(push.numDirections);
    ao = 1.0 - ao * push.aoMultiplier;
    ao = clamp(ao, 0.0, 1.0);
    
    // COMPUTE SHADER CONCEPT 7: Writing Output
    // RWTexture2D allows arbitrary write access (unlike fragment shader output)
    push.aoOutput[outputCoord] = float4(ao, ao, ao, 1.0);
}