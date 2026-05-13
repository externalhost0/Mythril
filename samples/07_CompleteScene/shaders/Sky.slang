import GPUStructs;

struct v2f {
    float4 ClipPos : SV_Position;
    float2 UV      : TEXCOORD0;
};
struct FSOutput {
    float4 FragColor : SV_Target0;
};

struct SkyPushConstants {
    Ptr<FrameData> frame;
    float3 sunDirection;
    float _pad;
};
[[vk::push_constant]]
SkyPushConstants push;

[shader("vertex")]
v2f vs_main(uint VertexID : SV_VertexID) {
    v2f output;
    output.UV       = float2((VertexID << 1) & 2, VertexID & 2);
    output.ClipPos  = float4(output.UV * 2.0 - 1.0, 0.0, 1.0);
    output.UV.y     = 1.0 - output.UV.y;
    return output;
}

// Reconstruct world-space view ray from a screen UV.
// UV convention from the fullscreen triangle: (0,0) = bottom-left, (1,1) = top-right.
// uv.y=1 → Vulkan NDC y=-1 (screen top / world up).
float3 getWorldViewDir(float2 uv, CameraData cam) {
    float2 ndc     = float2(uv.x * 2.0 - 1.0, 1.0 - uv.y * 2.0);
    float4 viewH   = mul(cam.invProj, float4(ndc, 1.0, 1.0));
    float3 viewDir = normalize(viewH.xyz / viewH.w);
    // view space: right=+X, up=+Y, forward=-Z
    return normalize(cam.right * viewDir.x + cam.up * viewDir.y - cam.forward * viewDir.z);
}

float safePow(float x, float p) {
    return pow(max(x, 1e-4), p);
}

float hash21(float2 p) {
    float3 p3 = fract(float3(p.xyx) * 0.1031);
    p3 += dot(p3, p3.yzx + 33.33);
    return fract((p3.x + p3.y) * p3.z);
}

float2 hash22(float2 p) {
    float3 p3 = fract(float3(p.xyx) * float3(0.1031, 0.1030, 0.0973));
    p3 += dot(p3, p3.yzx + 33.33);
    return fract(float2((p3.x + p3.y) * p3.z, (p3.x + p3.z) * p3.y));
}

float3 computeStars(float3 viewDir, float3 sunDir, float time) {
    // stars begin ~20° below horizon, fully bright ~50° below (midnight = 90°)
    // stars/moon begin ~8° below horizon, fully bright ~45° below
    float nightFactor = smoothstep(-sin(radians(8.0)), -sin(radians(45.0)), sunDir.y);
    float aboveGround = smoothstep(0.0, sin(radians(1.0)), viewDir.y);
    if (nightFactor * aboveGround < 0.001) return float3(0.0);

    float theta = acos(clamp(viewDir.y, -1.0, 1.0));
    float phi   = atan2(viewDir.z, viewDir.x);

    float gridScale = 40.0;
    float2 gridUV = float2(phi * gridScale / kPI, theta * gridScale / kPI);
    float2 cellID = floor(gridUV);
    float2 cellUV = fract(gridUV);

    float3 stars = float3(0.0);
    for (int dy = -1; dy <= 1; dy++) {
        for (int dx = -1; dx <= 1; dx++) {
            float2 neighbor = float2(dx, dy);
            float2 cell = cellID + neighbor;
            float2 starPos = hash22(cell);
            float  starBright = hash21(cell * 17.31);

            float2 diff = neighbor + starPos - cellUV;
            float dist = length(diff);

            float size = 0.015 + starBright * 0.025; // cell-space radius, not angular
            float point = smoothstep(size, size * 0.3, dist);

            float twinkle = 0.7 + 0.3 * sin(time * (1.5 + starBright * 3.0) + starBright * 6.2831);
            point *= twinkle * (0.3 + starBright * 0.7);

            float temp = hash21(cell * 7.77);
            float3 color = lerp(float3(0.8, 0.85, 1.0), float3(1.0, 0.9, 0.7), temp);

            stars += color * point;
        }
    }

    return stars * nightFactor * aboveGround * 0.6;
}

float3 computeMoon(float3 viewDir, float3 sunDir, float time) {
    // moon begins ~20° below horizon, fully bright ~50° below
    // stars/moon begin ~8° below horizon, fully bright ~45° below
    float nightFactor = smoothstep(-sin(radians(8.0)), -sin(radians(45.0)), sunDir.y);
    float aboveGround = smoothstep(0.0, sin(radians(1.0)), viewDir.y); // ~1° above ground
    if (nightFactor * aboveGround < 0.001) return float3(0.0);

    float3 moonDir = -sunDir;
    float cosAngle = dot(viewDir, moonDir);

    float moonAngularRadius = radians(0.86); // ~0.86° apparent size
    float moonDisk = smoothstep(cos(moonAngularRadius * 1.1), cos(moonAngularRadius * 0.5), cosAngle);
    if (moonDisk < 0.001) {
        float moonHalo = hgPhase(cosAngle, 0.76) * 0.02;
        return float3(0.15, 0.18, 0.25) * moonHalo * nightFactor * aboveGround;
    }

    float3 up = abs(moonDir.y) < 0.999 ? float3(0.0, 1.0, 0.0) : float3(1.0, 0.0, 0.0);
    float3 moonRight = normalize(cross(up, moonDir));
    float3 moonUp    = cross(moonDir, moonRight);
    float2 moonUV = float2(dot(viewDir - moonDir, moonRight), dot(viewDir - moonDir, moonUp));
    moonUV = moonUV / moonAngularRadius * 0.5 + 0.5;

    float crater1 = hash21(floor(moonUV * 8.0));
    float crater2 = hash21(floor(moonUV * 16.0));
    float crater3 = hash21(floor(moonUV * 4.0));
    float surface = 1.0 - 0.15 * crater1 - 0.10 * crater2 - 0.20 * smoothstep(0.6, 0.7, crater3);

    float3 moonColor = float3(0.85, 0.88, 0.95) * surface;

    float moonHalo = hgPhase(cosAngle, 0.76) * 0.02;
    float3 haloColor = float3(0.15, 0.18, 0.25) * moonHalo;

    return (moonColor * moonDisk + haloColor) * nightFactor * aboveGround;
}

float airMass(float mu) {
    float m = max(mu, 0.035);
    return 1.0 / (m + 0.15 * safePow(93.885 - acos(m) * 180.0 / kPI, -1.253));
}

float rayleighPhase(float cosTheta) {
    return 3.0 * (1.0 + cosTheta * cosTheta) / (16.0 * kPI);
}

float hgPhase(float cosTheta, float g) {
    float g2 = g * g;
    return (1.0 - g2) / (4.0 * kPI * safePow(1.0 + g2 - 2.0 * g * cosTheta, 1.5));
}

float hosekLikeDistribution(float viewMu, float cosTheta, float sunMu) {
    float gamma = acos(clamp(cosTheta, -1.0, 1.0));

    float A = lerp(-0.95, -0.72, sunMu);
    float B = lerp(-0.10, -0.32, sunMu);
    float C = lerp( 7.50,  2.80, sunMu);
    float D = lerp(-1.25, -2.20, sunMu);
    float E = lerp( 0.18,  0.36, sunMu);

    float horizon = 1.0 + A * exp(B / max(0.04, viewMu));
    float solar   = 1.0 + C * exp(D * gamma) + E * cosTheta * cosTheta;
    return max(0.0, horizon * solar);
}

// Compact Hosek-Wilkie-inspired sky.
// sunDir: unit vector pointing FROM the ground TOWARD the sun (i.e. -directionalLight.direction).
float3 computeSky(float3 viewDir, float3 sunDir) {
    float height    = viewDir.y;
    float viewMu    = saturate(height);
    float sunMuRaw  = sunDir.y;
    float sunMu     = saturate(sunMuRaw);
    // sun visible: fades in from 5° below to 1° above horizon
    float sunAbove  = smoothstep(-sin(radians(5.0)), sin(radians(1.0)), sunMuRaw);
    float cosTheta  = clamp(dot(viewDir, sunDir), -1.0, 1.0);

    float viewAir = airMass(viewMu);
    float sunAir  = airMass(max(sunMu, 0.04));
    float lowSun  = safePow(1.0 - sunMu, 3.0) * sunAbove;
    float horizon = safePow(1.0 - viewMu, 0.38);

    float3 betaR = float3(0.42, 0.78, 1.55);
    float3 extinction = exp(-betaR * viewAir * 0.055);

    float distribution = hosekLikeDistribution(max(viewMu, 0.015), cosTheta, sunMu);
    float rayleigh = rayleighPhase(cosTheta) * distribution;
    float3 blueScatter = float3(0.18, 0.32, 0.72) * rayleigh * 5.8;

    float hazeAmount = lerp(0.34, 0.72, lowSun) * (0.55 + horizon * 1.35);
    float3 hazeColor = lerp(float3(0.62, 0.76, 0.92), float3(1.0, 0.43, 0.12), lowSun);
    float3 horizonHaze = hazeColor * hazeAmount * exp(-sunAir * 0.022);

    float mieG = lerp(0.78, 0.90, sunMu);
    float mie = min(hgPhase(cosTheta, mieG), 18.0);
    float3 mieColor = lerp(float3(1.0, 0.48, 0.13), float3(1.0, 0.94, 0.82), sunMu);
    float3 sunGlow = mieColor * mie * lerp(0.23, 0.15, sunMu) * sunAbove;

    float3 sky = (blueScatter * extinction + horizonHaze * horizon) * sunAbove + sunGlow;

    // zenith tint kicks in ~13° above horizon
    float zenithFade = smoothstep(0.0, sin(radians(13.0)), viewMu);
    sky = lerp(sky, sky * float3(0.68, 0.78, 1.0), zenithFade * 0.25);
    float luma = dot(sky, float3(0.2126, 0.7152, 0.0722));
    sky = lerp(sky, float3(luma, luma, luma), horizon * 0.18);

    // twilight glow from 16° below to 3° above horizon
    float twilight = smoothstep(-sin(radians(16.0)), sin(radians(3.0)), sunMuRaw) * (1.0 - sunAbove);
    float3 nightZenith  = float3(0.002, 0.004, 0.018);
    float3 nightHorizon = float3(0.012, 0.014, 0.038);
    float3 nightSky = lerp(nightHorizon, nightZenith, safePow(viewMu, 0.55));
    nightSky += float3(0.16, 0.07, 0.22) * twilight * horizon;
    sky = lerp(nightSky, sky, sunAbove);

    // sun disk: ~1.4° outer edge, ~0.7° bright core
    float sunDisk = smoothstep(cos(radians(1.4)), cos(radians(0.7)), cosTheta) * sunAbove;
    sky += lerp(float3(5.0, 1.5, 0.28), float3(7.0, 6.4, 5.5), sunMu) * sunDisk;

    if (height < 0.0) {
        float gt = saturate(-height * 4.0);
        float3 ground = lerp(sky, float3(0.045, 0.038, 0.030), gt);
        sky = ground;
    }

    return max(sky, float3(0.0, 0.0, 0.0));
}

[shader("pixel")]
FSOutput fs_main(v2f input) {
    FSOutput output;
    float3 worldDir = getWorldViewDir(input.UV, push.frame.camera);
    float3 sunDir   = normalize(-push.sunDirection);
    float3 sky = computeSky(worldDir, sunDir);
    float t = push.frame.time;
    sky += computeStars(worldDir, sunDir, t);
    sky += computeMoon(worldDir, sunDir, t);
    output.FragColor = float4(sky, 1.0);
    return output;
}
