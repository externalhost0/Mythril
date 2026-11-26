struct Vertex {
    float3 position;
};

struct Geometry {
    Ptr<Vertex> vertices;
    Ptr<uint> indices;
    float4 albedo;
    uint vertexCount;
    uint padding;
};

// VkAccelerationStructureInstanceKHR, the transform is 3 rows of a row-major 3x4
struct Instance {
    float4 row0;
    float4 row1;
    float4 row2;
    uint customIndexAndMask;
    uint sbtOffsetAndFlags;
    uint64_t blas;
};

struct Bounds {
    float3 lo;
    uint geometry;
    float3 hi;
    float padding;
};

static const uint kTreeCount = 2;
static const uint kGeometryGround = 0;
static const uint kGeometryTreeFirst = 1;
static const uint kGeometryLeaf = kGeometryTreeFirst + kTreeCount;
