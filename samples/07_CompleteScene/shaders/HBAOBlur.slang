// Minimal HBAO Blur - Start with this to verify dispatch works
import GPUStructs;

struct BlurPushConstants {
    DescriptorHandle<Texture2D> aoInput;
    DescriptorHandle<Texture2D> depthTexture;
    DescriptorHandle<RWTexture2D> aoOutput;
    DescriptorHandle<SamplerState> pointSampler;
    float sharpness;
    float pad;
}

[SpecializationConstant]
const bool scIsHorizontal;

[vk::push_constant]
BlurPushConstants push;

#define GROUP_SIZE 256
#define KERNEL_RADIUS 4
#define CACHE_SIZE (GROUP_SIZE + 2 * KERNEL_RADIUS)

groupshared float sharedAO[CACHE_SIZE];
groupshared float sharedDepth[CACHE_SIZE];

static const float GAUSSIAN_WEIGHTS[5] = { 0.38774, 0.24477, 0.06136, 0.00598, 0.00023 };

[shader("compute")]
[numthreads(GROUP_SIZE, 1, 1)]
void cs_main(uint groupThreadID : SV_GroupThreadID, uint3 groupID : SV_GroupID) {
    int2 dims;
    push.aoOutput.GetDimensions(dims.x, dims.y);
    
    // Calculate pixel coordinate
    int2 coord;
    if (scIsHorizontal) {
        coord.x = int(groupID.x * GROUP_SIZE + groupThreadID);
        coord.y = int(groupID.y);
    } else {
        coord.x = int(groupID.x);
        coord.y = int(groupID.y * GROUP_SIZE + groupThreadID);
    }
    
    // Early exit if out of bounds (but still participate in loading)
    bool isValid = (coord.x < dims.x && coord.y < dims.y);
    
    int loadIdx = int(groupThreadID) + KERNEL_RADIUS;
    int2 blurDir = scIsHorizontal ? int2(1, 0) : int2(0, 1);
    
    // Load center
    if (isValid) {
        float2 uv = (float2(coord) + 0.5) / float2(dims);
        sharedAO[loadIdx] = push.aoInput.SampleLevel(push.pointSampler, uv, 0).r;
        sharedDepth[loadIdx] = push.depthTexture.SampleLevel(push.pointSampler, uv, 0).r;
    } else {
        sharedAO[loadIdx] = 1.0;
        sharedDepth[loadIdx] = 1.0;
    }
    
    // Load left/top border
    if (groupThreadID < KERNEL_RADIUS) {
        int2 borderCoord = coord - blurDir * KERNEL_RADIUS;
        bool borderValid = (borderCoord.x >= 0 && borderCoord.x < dims.x && 
                           borderCoord.y >= 0 && borderCoord.y < dims.y);
        
        if (borderValid) {
            float2 uv = (float2(borderCoord) + 0.5) / float2(dims);
            sharedAO[groupThreadID] = push.aoInput.SampleLevel(push.pointSampler, uv, 0).r;
            sharedDepth[groupThreadID] = push.depthTexture.SampleLevel(push.pointSampler, uv, 0).r;
        } else {
            sharedAO[groupThreadID] = sharedAO[KERNEL_RADIUS];
            sharedDepth[groupThreadID] = sharedDepth[KERNEL_RADIUS];
        }
    }
    
    // Load right/bottom border
    if (groupThreadID >= GROUP_SIZE - KERNEL_RADIUS) {
        int borderIdx = int(groupThreadID) + 2 * KERNEL_RADIUS;
        int2 borderCoord = coord + blurDir * KERNEL_RADIUS;
        bool borderValid = (borderCoord.x >= 0 && borderCoord.x < dims.x && 
                           borderCoord.y >= 0 && borderCoord.y < dims.y);
        
        if (borderValid) {
            float2 uv = (float2(borderCoord) + 0.5) / float2(dims);
            sharedAO[borderIdx] = push.aoInput.SampleLevel(push.pointSampler, uv, 0).r;
            sharedDepth[borderIdx] = push.depthTexture.SampleLevel(push.pointSampler, uv, 0).r;
        } else {
            int lastValidIdx = GROUP_SIZE + KERNEL_RADIUS - 1;
            sharedAO[borderIdx] = sharedAO[lastValidIdx];
            sharedDepth[borderIdx] = sharedDepth[lastValidIdx];
        }
    }
    
    GroupMemoryBarrierWithGroupSync();
    
    if (!isValid) return;
    
    float centerDepth = sharedDepth[loadIdx];
    float centerAO = sharedAO[loadIdx];
    
    // Skybox early out
    if (centerDepth >= 0.9999) {
        push.aoOutput[coord] = float4(1, 1, 1, 1);
        return;
    }
    
    // Bilateral blur
    float totalAO = centerAO * GAUSSIAN_WEIGHTS[0];
    float totalWeight = GAUSSIAN_WEIGHTS[0];
    
    [unroll]
    for (int i = 1; i <= KERNEL_RADIUS; i++) {
        // Positive direction
        float sampleAO = sharedAO[loadIdx + i];
        float sampleDepth = sharedDepth[loadIdx + i];
        float depthDiff = abs(centerDepth - sampleDepth);
        float bilateral = exp(-depthDiff * push.sharpness);
        float weight = GAUSSIAN_WEIGHTS[i] * bilateral;
        totalAO += sampleAO * weight;
        totalWeight += weight;
        
        // Negative direction
        sampleAO = sharedAO[loadIdx - i];
        sampleDepth = sharedDepth[loadIdx - i];
        depthDiff = abs(centerDepth - sampleDepth);
        bilateral = exp(-depthDiff * push.sharpness);
        weight = GAUSSIAN_WEIGHTS[i] * bilateral;
        totalAO += sampleAO * weight;
        totalWeight += weight;
    }
    
    float result = totalAO / totalWeight;
    push.aoOutput[coord] = float4(result, result, result, 1.0);
}