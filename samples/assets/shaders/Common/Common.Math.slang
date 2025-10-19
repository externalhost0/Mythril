

// math constants //

static constexpr float PI = 3.14159265f;
static constexpr float PI2 = 6.28318530f;
static constexpr float PI4 = 12.5663706f;
static constexpr float INV_PI = 0.31830988f;

// some operators for glsl parity //

float2   operator*(float2x2 matrix, float2 vec) { return mul(matrix, vec); }
float2   operator*(float2 vec, float2x2 matrix) { return mul(vec, matrix); }
float2x2 operator*(float2x2 matrixLeft, float2x2 matrixRight) { return mul(matrixLeft, matrixRight); }

float4   operator*(float4x4 matrix, float4 vec) { return mul(matrix, vec); }
float4   operator*(float4 vec, float4x4 matrix) { return mul(vec, matrix); }
float4x4 operator*(float4x4 matrixLeft, float4x4 matrixRight) { return mul(matrixLeft, matrixRight); }

float3   operator*(float3x3 matrix, float3 vec) { return mul(matrix, vec); }
float3   operator*(float3 vec, float3x3 matrix) { return mul(vec, matrix); }
float3x3 operator*(float3x3 matrixLeft, float3x3 matrixRight) { return mul(matrixLeft, matrixRight); }


// fast math helper functions //

float fast_sqrt(float x) {
    int i = asint(x);
    i = 0x1FBD1DF5 + (i >> 1);
    return asfloat(i);
}

float fast_length(float3 v) {
    float length_squared = dot(v, v);
    return fast_sqrt(length_squared);
}

float fast_sin(float x) {
    const float B = 4 / PI;
    const float C = -4 / PI2;
    const float P = 0.225;

    float y = B * x + C * x * abs(x);
    y = P * (y * abs(y) - y) + y;
    
    return y;
}

float fast_cos(float x) {
   return abs(abs(x) / PI2 % 4 - 2) - 1;
}
