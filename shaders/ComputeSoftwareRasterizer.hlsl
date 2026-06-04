/// \file ComputeSoftwareRasterizer.hlsl
/// Compute-only software rasterizer and experimental history/acoustics pipeline.
/// Target profile: cs_6_6

#define TILE_SIZE 16
#define TILE_THREAD_COUNT 256
#define CLEAR_GROUP_SIZE 256
#define VERTEX_GROUP_SIZE 128
#define TRIANGLE_GROUP_SIZE 128
#define BIN_GROUP_SIZE 64
#define MAX_TILE_TRIANGLES 256
#define SHARED_BANK_STRIDE 32
#define SHARED_BANK_PAD (MAX_TILE_TRIANGLES / SHARED_BANK_STRIDE)
#define INVALID_TRIANGLE 0xffffffffu

/// Packed object-space vertex layout mirrored by the host StructuredBuffer.
struct Vertex
{
    float3 position;
    float3 normal;
    float2 uv;
};

struct VertexProjected
{
    float4 clip;
    float2 screen;
    float  depth;
    float  invW;
    float3 normalOverW;
    float  pad0;
    float2 uvOverW;
    float2 pad1;
};

/// Triangle setup payload cached per visible primitive and re-used during fine raster.
struct TriangleSetup
{
    float2 p0;
    float2 p1;
    float2 p2;
    float  z0;
    float  z1;
    float  z2;
    float  invW0;
    float  invW1;
    float  invW2;
    float2 uv0OverW;
    float2 uv1OverW;
    float2 uv2OverW;
    float3 n0OverW;
    float  pad0;
    float3 n1OverW;
    float  pad1;
    float3 n2OverW;
    float  pad2;
    float  area2;
    float  invArea2;
    uint2  tileMin;
    uint2  tileMax;
    uint   materialIndex;
    uint   pad3;
};

struct ClipVertex
{
    float4 clip;
    float3 normal;
    float2 uv;
};

cbuffer RasterizerConstants : register(b0)
{
    float4x4 gMVP;
    uint gViewportWidth;
    uint gViewportHeight;
    uint gTileCountX;
    uint gTileCountY;
    uint gVertexCount;
    uint gTriangleCount;
    uint gMaxVisibleTriangles;
    uint gMaxTrianglesPerTile;
    float gInvViewportWidth;
    float gInvViewportHeight;
    float gGuardBandPixelsX;
    float gGuardBandPixelsY;
    float3 gLightDirection;
    float gAlphaBlendEnable;
    float gUsePacked32Path;
    float gDebugHeatmapMode;
    float gHeatmapScale;
    uint  gMsaaSampleCount;
    uint  gProjectionMode;
    float gProjectionStrength;
    float gMipBias;
    float gPad0;
    float gMicroTriangleAreaThreshold;
    uint  gPortalTriangleIndex;
    uint  gTopologyTriangleIndex;
    uint  gEnablePortalTraversal;
    uint  gEnableTopologyRemap;
    float2 gPortalViewShift;
    float gTopologyFoldStrength;
    uint  gFrameIndex;
    uint  gEnableProceduralClusters;
    uint  gEnableStochasticSampling;
    uint  gAudioDebugMode;
    float gStochasticJitterStrength;
    float gAcousticOcclusionScale;
    float gAcousticDiffractionScale;
    uint  gProceduralClusterSeed;
    uint  gEnableTemporalReprojection;
    float gTemporalBlendWeight;
    float gTemporalDepthThreshold;
    float gTemporalMotionScale;
};

StructuredBuffer<Vertex> gVertexBuffer : register(t0);
StructuredBuffer<uint> gIndexBuffer : register(t1);
Texture2D<float4> gTexture : register(t2);

RWStructuredBuffer<VertexProjected> gProjectedVertices : register(u0);
RWStructuredBuffer<TriangleSetup> gVisibleTriangles : register(u1);
RWStructuredBuffer<uint> gVisibleCounter : register(u2);       // [0] only
RWStructuredBuffer<uint> gTileCounters : register(u3);         // tileCount
RWStructuredBuffer<uint> gTileOverflow : register(u4);         // tileCount
RWStructuredBuffer<uint> gTileTriangleIndices : register(u5);  // tileCount * maxTrianglesPerTile
RWStructuredBuffer<uint64_t> gPackedDepthColor : register(u6); // [depth32 | rgba8]
RWStructuredBuffer<uint> gTileDepthHint : register(u7);        // conservative, optional
RWStructuredBuffer<uint> gPackedDepthColor32 : register(u8);   // [depth24 | luminance8]
RWStructuredBuffer<uint> gTileDebugCounts : register(u9);      // per-tile diagnostic counter
RWStructuredBuffer<uint> gPortalMask : register(u10);          // per-pixel portal target
RWStructuredBuffer<uint> gProfilingCounters : register(u11);   // global diagnostics
RWStructuredBuffer<uint> gMaterialDensity : register(u12);     // per-pixel material density id
RWStructuredBuffer<uint> gTileAcousticOcclusion : register(u13);
RWStructuredBuffer<uint> gTileAcousticDiffraction : register(u14);
RWStructuredBuffer<uint> gPreviousPackedTemporal : register(u15);
RWTexture2D<float4> gHistoryTarget : register(u16);
RWTexture2D<float4> gPreviousHistoryTarget : register(u17);
RWTexture2D<float4> gPresentTarget : register(u18);

static const float kDepthScale = 4294967295.0f;
static const float kFarDepth = 1.0f;
static const uint64_t kPackedClear = (uint64_t(0xffffffffu) << 32) | uint64_t(0x00000000u);
static const uint kDepth24Max = 0x00ffffffu;
static const uint kPackedClear32 = 0xffffff00u;
static const uint kInvalidPortal = 0xffffffffu;

static const uint TRI_FLAG_TINY     = 1u << 0;
static const uint TRI_FLAG_PORTAL   = 1u << 1;
static const uint TRI_FLAG_TOPOLOGY = 1u << 2;
static const uint TRI_FLAG_PROCEDURAL = 1u << 3;

static const uint PROFILE_TILES                = 0u;
static const uint PROFILE_TILE_TRIANGLE_REFS   = 1u;
static const uint PROFILE_MAX_TILE_TRIANGLES   = 2u;
static const uint PROFILE_WAVE_TESTS           = 3u;
static const uint PROFILE_WAVE_COVERED         = 4u;
static const uint PROFILE_COMMIT_ATTEMPTS      = 5u;
static const uint PROFILE_COMMIT_SUCCESSES     = 6u;
static const uint PROFILE_TILE_OVERFLOWS       = 7u;
static const uint PROFILE_TINY_TRIANGLES       = 8u;
static const uint PROFILE_PORTAL_PIXELS        = 9u;
static const uint PROFILE_TOPOLOGY_REMAPS      = 10u;
static const uint PROFILE_COVERED_SAMPLES      = 11u;
static const uint PROFILE_PORTAL_TRIANGLES     = 12u;
static const uint PROFILE_HIZ_TILE_SKIPS       = 13u;
static const uint PROFILE_MICRO_SPLATS         = 14u;
static const uint PROFILE_PROCEDURAL_TRIANGLES = 15u;
static const uint PROFILE_STOCHASTIC_SAMPLES   = 16u;
static const uint PROFILE_AUDIO_EDGE_HITS      = 17u;
static const uint PROFILE_MATERIAL_WRITES      = 18u;
static const uint PROFILE_TAA_HISTORY_SAMPLES  = 19u;
static const uint PROFILE_TAA_REJECTIONS       = 20u;
static const uint PROFILE_TAA_ACCEPTS          = 21u;
static const uint PROFILE_COUNTER_COUNT        = 22u;
static const float kVelocityPixelRange = 8.0f;

uint FlattenPixel(uint2 p)
{
    return p.y * gViewportWidth + p.x;
}

uint FlattenTile(uint2 tileCoord)
{
    return tileCoord.y * gTileCountX + tileCoord.x;
}

uint SharedTriangleCacheIndex(uint slot)
{
    return slot + (slot / SHARED_BANK_STRIDE);
}

uint EncodeTriangleMetadata(uint flags, uint topologyNode, uint portalTargetNode, uint materialDensity)
{
    return (flags & 0xffu) |
           ((topologyNode & 0xffu) << 8u) |
           ((portalTargetNode & 0xffu) << 16u) |
           ((materialDensity & 0xffu) << 24u);
}

uint DecodeTriangleFlags(TriangleSetup tri)
{
    return tri.materialIndex & 0xffu;
}

uint DecodeTopologyNode(TriangleSetup tri)
{
    return (tri.materialIndex >> 8u) & 0xffu;
}

uint DecodePortalTarget(TriangleSetup tri)
{
    return (tri.materialIndex >> 16u) & 0xffu;
}

uint DecodeMaterialDensity(TriangleSetup tri)
{
    return (tri.materialIndex >> 24u) & 0xffu;
}

uint HashU32(uint v)
{
    v ^= 2747636419u;
    v *= 2654435769u;
    v ^= v >> 16u;
    v *= 2654435769u;
    v ^= v >> 16u;
    v *= 2654435769u;
    return v;
}

float Hash01(uint v)
{
    return float(HashU32(v) & 0x00ffffffu) * (1.0f / 16777215.0f);
}

float2 Hash02(uint v)
{
    return float2(Hash01(v * 1664525u + 1013904223u),
                  Hash01(v * 22695477u + 1u));
}

ClipVertex MakeGeneratedClipVertex(float2 xy, float z, float2 uv)
{
    ClipVertex v;
    v.clip = float4(xy, z, 1.0f);
    v.normal = float3(0.0f, 0.0f, -1.0f);
    v.uv = uv;
    return v;
}

uint DepthToUint(float depth)
{
    return (uint)clamp(depth * kDepthScale, 0.0f, kDepthScale);
}

uint DepthToUint24(float depth)
{
    return (uint)clamp(depth * float(kDepth24Max), 0.0f, float(kDepth24Max));
}

uint PackRGBA8(float4 color)
{
    uint4 c = (uint4)round(saturate(color) * 255.0f);
    return c.x | (c.y << 8) | (c.z << 16) | (c.w << 24);
}

float4 UnpackRGBA8(uint packed)
{
    return float4(
        (packed & 255u),
        ((packed >> 8) & 255u),
        ((packed >> 16) & 255u),
        ((packed >> 24) & 255u)) / 255.0f;
}

uint64_t PackDepthColor(uint depthU32, uint rgba8)
{
    return (uint64_t(depthU32) << 32) | uint64_t(rgba8);
}

void UnpackDepthColor(uint64_t packed, out uint depthU32, out uint rgba8)
{
    depthU32 = (uint)(packed >> 32);
    rgba8 = (uint)(packed & 0xffffffffu);
}

uint PackDepthColor32(uint depth24, float4 color)
{
    float luma = dot(saturate(color.rgb), float3(0.2126f, 0.7152f, 0.0722f));
    uint color8 = (uint)round(luma * 255.0f);
    return ((depth24 & kDepth24Max) << 8) | (color8 & 0xffu);
}

void UnpackDepthColor32(uint packed, out uint depth24, out float4 color)
{
    depth24 = (packed >> 8) & kDepth24Max;
    float luma = float(packed & 0xffu) / 255.0f;
    color = float4(luma.xxx, 1.0f);
}

uint DepthToUint16(float depth)
{
    return (uint)clamp(depth * 65535.0f, 0.0f, 65535.0f);
}

uint PackTemporalMetadata(uint depth16, float2 velocityPx)
{
    int2 velocitySnorm = int2(round(clamp(velocityPx / kVelocityPixelRange, -1.0f.xx, 1.0f.xx) * 127.0f));
    uint vx = uint(velocitySnorm.x & 0xff);
    uint vy = uint(velocitySnorm.y & 0xff);
    return ((depth16 & 0xffffu) << 16u) | (vx << 8u) | vy;
}

void UnpackTemporalMetadata(uint packed, out uint depth16, out float2 velocityPx)
{
    depth16 = (packed >> 16u) & 0xffffu;
    int vx = int((packed >> 8u) & 0xffu);
    int vy = int(packed & 0xffu);
    if (vx > 127) vx -= 256;
    if (vy > 127) vy -= 256;
    velocityPx = float2(float(vx), float(vy)) * (kVelocityPixelRange / 127.0f);
}

float4 HeatmapColor(uint count, float scale)
{
    float t = saturate(float(count) * scale);
    float3 cold = lerp(float3(0.0f, 0.2f, 0.9f), float3(0.0f, 0.9f, 0.1f), saturate(t * 2.0f));
    float3 hot = lerp(float3(0.0f, 0.9f, 0.1f), float3(0.95f, 0.1f, 0.0f), saturate(t * 2.0f - 1.0f));
    return float4((t < 0.5f) ? cold : hot, 1.0f);
}

float2 ApplyProjectionWarp(float2 ndc)
{
    if (gProjectionMode == 0u)
    {
        return ndc;
    }

    float strength = max(gProjectionStrength, 0.001f);

    if (gProjectionMode == 1u)
    {
        float denom = atan(strength);
        return float2(atan(ndc.x * strength) / denom, ndc.y);
    }

    if (gProjectionMode == 2u)
    {
        float r = length(ndc);
        if (r < 1e-6f)
        {
            return ndc;
        }

        float warpedRadius = atan(r * strength) / atan(strength);
        return ndc * (warpedRadius / r);
    }

    if (gProjectionMode == 3u)
    {
        float scale = rsqrt(1.0f + strength * dot(ndc, ndc));
        return ndc * scale;
    }

    return ndc;
}

ClipVertex MakeClipVertex(VertexProjected projected)
{
    ClipVertex v;
    v.clip = projected.clip;
    float invInvW = rcp(projected.invW);
    v.normal = projected.normalOverW * invInvW;
    v.uv = projected.uvOverW * invInvW;
    return v;
}

ClipVertex LerpClipVertex(ClipVertex a, ClipVertex b, float t)
{
    ClipVertex v;
    v.clip = lerp(a.clip, b.clip, t);
    v.normal = lerp(a.normal, b.normal, t);
    v.uv = lerp(a.uv, b.uv, t);
    return v;
}

void GenerateProceduralClusterTriangle(uint triIndex, out ClipVertex c0, out ClipVertex c1, out ClipVertex c2)
{
    uint clusterIndex = triIndex >> 2u;
    uint localIndex = triIndex & 3u;
    uint seed = clusterIndex ^ (gProceduralClusterSeed * 747796405u);

    float2 center = Hash02(seed) * 1.7f - 0.85f.xx;
    float depth = lerp(0.15f, 0.88f, Hash01(seed ^ 0x9e3779b9u));
    float lod = Hash01(seed ^ 0x85ebca6bu);
    float radius = lerp(0.26f, 0.035f, lod);
    float angle = Hash01(seed ^ 0xc2b2ae35u) * 6.2831853f;

    float2 axis0 = float2(cos(angle), sin(angle));
    float2 axis1 = float2(-axis0.y, axis0.x);
    float phase = (6.2831853f / 4.0f) * float(localIndex);

    float2 a = center + (axis0 * cos(phase) + axis1 * sin(phase)) * radius;
    float2 b = center + (axis0 * cos(phase + 1.2f) + axis1 * sin(phase + 1.2f)) * (radius * 0.92f);
    float2 c = center + (axis0 * cos(phase + 2.4f) + axis1 * sin(phase + 2.4f)) * (radius * 0.74f);

    c0 = MakeGeneratedClipVertex(a, depth, saturate(a * 0.5f + 0.5f));
    c1 = MakeGeneratedClipVertex(b, depth, saturate(b * 0.5f + 0.5f));
    c2 = MakeGeneratedClipVertex(c, depth, saturate(c * 0.5f + 0.5f));
}

float2 ClipToScreen(float4 clip)
{
    float invW = rcp(clip.w);
    float2 ndc = ApplyProjectionWarp(clip.xy * invW);
    return float2(
        (ndc.x * 0.5f + 0.5f) * gViewportWidth,
        (1.0f - (ndc.y * 0.5f + 0.5f)) * gViewportHeight);
}

bool SetupTriangleFromClipVertices(ClipVertex c0,
                                   ClipVertex c1,
                                   ClipVertex c2,
                                   uint packedMetadata,
                                   uint sourceTriangleIndex,
                                   out TriangleSetup tri)
{
    float2 p0 = ClipToScreen(c0.clip);
    float2 p1 = ClipToScreen(c1.clip);
    float2 p2 = ClipToScreen(c2.clip);

    float2 bbMin = min(p0, min(p1, p2));
    float2 bbMax = max(p0, max(p1, p2));
    if (OutsideGuardBand(bbMin, bbMax))
    {
        return false;
    }

    float area2 = (p1.x - p0.x) * (p2.y - p0.y) - (p2.x - p0.x) * (p1.y - p0.y);
    if (area2 <= 0.0f)
    {
        return false;
    }

    float invW0 = rcp(c0.clip.w);
    float invW1 = rcp(c1.clip.w);
    float invW2 = rcp(c2.clip.w);

    float2 screenMin = floor(clamp(bbMin, 0.0f.xx, float2(gViewportWidth - 1, gViewportHeight - 1)));
    float2 screenMax = ceil(clamp(bbMax, 0.0f.xx, float2(gViewportWidth - 1, gViewportHeight - 1)));

    tri.p0 = p0;
    tri.p1 = p1;
    tri.p2 = p2;
    tri.z0 = saturate(c0.clip.z * invW0);
    tri.z1 = saturate(c1.clip.z * invW1);
    tri.z2 = saturate(c2.clip.z * invW2);
    tri.invW0 = invW0;
    tri.invW1 = invW1;
    tri.invW2 = invW2;
    tri.uv0OverW = c0.uv * invW0;
    tri.uv1OverW = c1.uv * invW1;
    tri.uv2OverW = c2.uv * invW2;
    tri.n0OverW = c0.normal * invW0;
    tri.pad0 = 0.0f;
    tri.n1OverW = c1.normal * invW1;
    tri.pad1 = 0.0f;
    tri.n2OverW = c2.normal * invW2;
    tri.pad2 = 0.0f;
    tri.area2 = area2;
    tri.invArea2 = rcp(area2);
    tri.tileMin = (uint2)(screenMin / TILE_SIZE);
    tri.tileMax = min((uint2)(screenMax / TILE_SIZE), uint2(gTileCountX - 1, gTileCountY - 1));
    float2 extent = bbMax - bbMin;
    uint flags = packedMetadata & 0xffu;
    if (abs(area2) <= gMicroTriangleAreaThreshold || max(extent.x, extent.y) <= 1.0f)
    {
        flags |= TRI_FLAG_TINY;
    }
    tri.materialIndex = (packedMetadata & 0xffffff00u) | flags;
    tri.pad3 = sourceTriangleIndex;
    return true;
}

void AppendVisibleTriangle(TriangleSetup tri)
{
    uint visibleIndex;
    InterlockedAdd(gVisibleCounter[0], 1, visibleIndex);
    if (visibleIndex < gMaxVisibleTriangles)
    {
        gVisibleTriangles[visibleIndex] = tri;
    }
}

uint ClipTriangleAgainstNearPlane(ClipVertex v0, ClipVertex v1, ClipVertex v2, out ClipVertex outVerts[4])
{
    ClipVertex inputVerts[4];
    inputVerts[0] = v0;
    inputVerts[1] = v1;
    inputVerts[2] = v2;

    uint inputCount = 3;
    uint outputCount = 0;

    [unroll]
    for (uint i = 0; i < 3; ++i)
    {
        ClipVertex current = inputVerts[i];
        ClipVertex previous = inputVerts[(i + inputCount - 1) % inputCount];
        bool currentInside = current.clip.z >= 0.0f;
        bool previousInside = previous.clip.z >= 0.0f;

        if (currentInside != previousInside)
        {
            float t = previous.clip.z / (previous.clip.z - current.clip.z);
            outVerts[outputCount++] = LerpClipVertex(previous, current, t);
        }

        if (currentInside)
        {
            outVerts[outputCount++] = current;
        }
    }

    return outputCount;
}

bool TrivialRejectClip(float4 a, float4 b, float4 c)
{
    bool xNeg = (a.x < -a.w) && (b.x < -b.w) && (c.x < -c.w);
    bool xPos = (a.x >  a.w) && (b.x >  b.w) && (c.x >  c.w);
    bool yNeg = (a.y < -a.w) && (b.y < -b.w) && (c.y < -c.w);
    bool yPos = (a.y >  a.w) && (b.y >  b.w) && (c.y >  c.w);
    bool zNeg = (a.z <  0.0f) && (b.z <  0.0f) && (c.z <  0.0f);
    bool zPos = (a.z >  a.w) && (b.z >  b.w) && (c.z >  c.w);
    return xNeg || xPos || yNeg || yPos || zNeg || zPos;
}

bool OutsideGuardBand(float2 bbMin, float2 bbMax)
{
    float2 gbMin = float2(-gGuardBandPixelsX, -gGuardBandPixelsY);
    float2 gbMax = float2(gViewportWidth + gGuardBandPixelsX, gViewportHeight + gGuardBandPixelsY);
    return (bbMax.x < gbMin.x) || (bbMax.y < gbMin.y) || (bbMin.x > gbMax.x) || (bbMin.y > gbMax.y);
}

float EdgeFunction(float2 a, float2 b, float2 p)
{
    return (p.x - a.x) * (b.y - a.y) - (p.y - a.y) * (b.x - a.x);
}

float2 EvaluatePerspectiveUV(TriangleSetup tri, float2 samplePos)
{
    float e12 = EdgeFunction(tri.p1, tri.p2, samplePos);
    float e20 = EdgeFunction(tri.p2, tri.p0, samplePos);
    float e01 = EdgeFunction(tri.p0, tri.p1, samplePos);

    float alpha = e12 * tri.invArea2;
    float beta = e20 * tri.invArea2;
    float gamma = e01 * tri.invArea2;
    float invWPixel = alpha * tri.invW0 + beta * tri.invW1 + gamma * tri.invW2;
    float wPixel = rcp(invWPixel);
    return (alpha * tri.uv0OverW +
            beta * tri.uv1OverW +
            gamma * tri.uv2OverW) * wPixel;
}

float ComputeMipLevel(Texture2D<float4> tex, TriangleSetup tri, float2 samplePos)
{
    uint baseW, baseH, mipCount;
    tex.GetDimensions(0, baseW, baseH, mipCount);

    float2 uvCenter = EvaluatePerspectiveUV(tri, samplePos);
    float2 uvDx = EvaluatePerspectiveUV(tri, samplePos + float2(1.0f, 0.0f)) - uvCenter;
    float2 uvDy = EvaluatePerspectiveUV(tri, samplePos + float2(0.0f, 1.0f)) - uvCenter;

    float2 texelDx = uvDx * float2(baseW, baseH);
    float2 texelDy = uvDy * float2(baseW, baseH);
    float rho = max(length(texelDx), length(texelDy));
    float mip = log2(max(rho, 1.0f)) + gMipBias;
    return clamp(mip, 0.0f, float(max(int(mipCount) - 1, 0)));
}

float4 SampleBilinear(Texture2D<float4> tex, float2 uv, uint mipLevel)
{
    uint w, h, mipCount;
    tex.GetDimensions(mipLevel, w, h, mipCount);

    float2 texel = saturate(uv) * float2(w, h) - 0.5f;
    float2 basef = floor(texel);
    float2 fracf = texel - basef;

    int2 p00 = clamp((int2)basef, int2(0, 0), int2((int)w - 1, (int)h - 1));
    int2 p10 = clamp(p00 + int2(1, 0), int2(0, 0), int2((int)w - 1, (int)h - 1));
    int2 p01 = clamp(p00 + int2(0, 1), int2(0, 0), int2((int)w - 1, (int)h - 1));
    int2 p11 = clamp(p00 + int2(1, 1), int2(0, 0), int2((int)w - 1, (int)h - 1));

    float4 c00 = tex.Load(int3(p00, mipLevel));
    float4 c10 = tex.Load(int3(p10, mipLevel));
    float4 c01 = tex.Load(int3(p01, mipLevel));
    float4 c11 = tex.Load(int3(p11, mipLevel));

    float4 cx0 = lerp(c00, c10, fracf.x);
    float4 cx1 = lerp(c01, c11, fracf.x);
    return lerp(cx0, cx1, fracf.y);
}

float4 ShadeFragment(float2 uv, float3 normal, uint mipLevel)
{
    float4 albedo = SampleBilinear(gTexture, uv, mipLevel);
    float3 L = normalize(gLightDirection);
    float NdotL = saturate(dot(normalize(normal), L));
    float3 lit = albedo.rgb * (0.12f + 0.88f * NdotL);
    return float4(lit, albedo.a);
}

float2 ApplyTopologyTraversal(float2 uv, float2 samplePos, uint topologyNode)
{
    float foldStrength = max(gTopologyFoldStrength, 1.0f);
    float2 folded = frac(uv * foldStrength + samplePos * float2(gInvViewportWidth, gInvViewportHeight));
    if ((topologyNode & 1u) != 0u)
    {
        folded.x = 1.0f - folded.x;
    }
    if ((topologyNode & 2u) != 0u)
    {
        folded.y = 1.0f - folded.y;
    }
    return abs(folded * 2.0f - 1.0f);
}

float4 ApplyPortalComposite(float4 color, uint portalTarget, float2 pixelUV)
{
    float phase = (pixelUV.x + pixelUV.y) * 24.0f + float(portalTarget) * 1.37f;
    float band = 0.55f + 0.45f * sin(phase);
    float3 tintA = float3(0.18f, 0.82f, 1.0f);
    float3 tintB = float3(0.95f, 0.25f, 0.82f);
    float3 tint = lerp(tintA, tintB, frac(float(portalTarget) * 0.31f + pixelUV.x));
    color.rgb = lerp(color.rgb, tint * band, 0.35f);
    return color;
}

float4 BlendSourceOver(float4 src, float4 dst)
{
    float oneMinusA = 1.0f - src.a;
    float3 rgb = src.rgb + dst.rgb * oneMinusA;
    float a = src.a + dst.a * oneMinusA;
    return float4(rgb, a);
}

[numthreads(CLEAR_GROUP_SIZE, 1, 1)]
void CS_Clear(uint3 dispatchThreadID : SV_DispatchThreadID)
{
    uint linear = dispatchThreadID.x;
    uint pixelCount = gViewportWidth * gViewportHeight;
    uint tileCount = gTileCountX * gTileCountY;

    if (linear == 0)
    {
        gVisibleCounter[0] = 0;
    }

    if (linear < pixelCount)
    {
        gPackedDepthColor[linear] = kPackedClear;
        gPackedDepthColor32[linear] = kPackedClear32;
        gPortalMask[linear] = kInvalidPortal;
        gMaterialDensity[linear] = 0u;
    }

    if (linear < tileCount)
    {
        gTileCounters[linear] = 0;
        gTileOverflow[linear] = 0;
        gTileDepthHint[linear] = 0xffffffffu;
        gTileDebugCounts[linear] = 0;
        gTileAcousticOcclusion[linear] = 0u;
        gTileAcousticDiffraction[linear] = 0u;
    }

    if (linear < PROFILE_COUNTER_COUNT)
    {
        gProfilingCounters[linear] = 0;
    }
}

[numthreads(VERTEX_GROUP_SIZE, 1, 1)]
void CS_TransformVertices(uint3 dispatchThreadID : SV_DispatchThreadID)
{
    uint vertexIndex = dispatchThreadID.x;
    if (vertexIndex >= gVertexCount)
    {
        return;
    }

    Vertex vertex = gVertexBuffer[vertexIndex];
    float4 clip = mul(float4(vertex.position, 1.0f), gMVP);
    float invW = rcp(clip.w);
    float2 ndc = ApplyProjectionWarp(clip.xy * invW);

    VertexProjected projected;
    projected.clip = clip;
    projected.screen.x = (ndc.x * 0.5f + 0.5f) * gViewportWidth;
    projected.screen.y = (1.0f - (ndc.y * 0.5f + 0.5f)) * gViewportHeight;
    projected.depth = saturate(clip.z * invW);
    projected.invW = invW;
    projected.normalOverW = vertex.normal * invW;
    projected.pad0 = 0.0f;
    projected.uvOverW = vertex.uv * invW;
    projected.pad1 = 0.0f.xx;
    gProjectedVertices[vertexIndex] = projected;
}

[numthreads(TRIANGLE_GROUP_SIZE, 1, 1)]
void CS_SetupAndCull(uint3 dispatchThreadID : SV_DispatchThreadID)
{
    uint triIndex = dispatchThreadID.x;
    if (triIndex >= gTriangleCount)
    {
        return;
    }

    ClipVertex c0;
    ClipVertex c1;
    ClipVertex c2;
    uint flags = 0u;
    uint materialDensity = 96u + (triIndex * 53u) % 128u;

    if (gEnableProceduralClusters != 0u)
    {
        GenerateProceduralClusterTriangle(triIndex, c0, c1, c2);
        flags |= TRI_FLAG_PROCEDURAL;
        materialDensity = 64u + (triIndex * 29u) % 160u;
        InterlockedAdd(gProfilingCounters[PROFILE_PROCEDURAL_TRIANGLES], 1u);
    }
    else
    {
        uint i0 = gIndexBuffer[triIndex * 3 + 0];
        uint i1 = gIndexBuffer[triIndex * 3 + 1];
        uint i2 = gIndexBuffer[triIndex * 3 + 2];

        VertexProjected pv0 = gProjectedVertices[i0];
        VertexProjected pv1 = gProjectedVertices[i1];
        VertexProjected pv2 = gProjectedVertices[i2];

        if (TrivialRejectClip(pv0.clip, pv1.clip, pv2.clip))
        {
            return;
        }

        c0 = MakeClipVertex(pv0);
        c1 = MakeClipVertex(pv1);
        c2 = MakeClipVertex(pv2);
    }

    ClipVertex clippedVerts[4];
    uint clippedCount = ClipTriangleAgainstNearPlane(c0, c1, c2, clippedVerts);
    if (clippedCount < 3)
    {
        return;
    }

    uint topologyNode = 0u;
    uint portalTargetNode = 0u;
    if (gEnablePortalTraversal != 0u && triIndex == gPortalTriangleIndex)
    {
        flags |= TRI_FLAG_PORTAL;
        portalTargetNode = 1u;
        InterlockedAdd(gProfilingCounters[PROFILE_PORTAL_TRIANGLES], 1u);
    }
    if (gEnableTopologyRemap != 0u && triIndex == gTopologyTriangleIndex)
    {
        flags |= TRI_FLAG_TOPOLOGY;
        topologyNode = 3u;
    }
    uint packedMetadata = EncodeTriangleMetadata(flags, topologyNode, portalTargetNode, materialDensity);

    TriangleSetup tri;
    if (SetupTriangleFromClipVertices(clippedVerts[0], clippedVerts[1], clippedVerts[2], packedMetadata, triIndex, tri))
    {
        AppendVisibleTriangle(tri);
    }

    if (clippedCount == 4)
    {
        TriangleSetup tri2;
        if (SetupTriangleFromClipVertices(clippedVerts[0], clippedVerts[2], clippedVerts[3], packedMetadata, triIndex, tri2))
        {
            AppendVisibleTriangle(tri2);
        }
    }
}

[numthreads(BIN_GROUP_SIZE, 1, 1)]
void CS_BinTriangles(uint3 dispatchThreadID : SV_DispatchThreadID)
{
    uint visibleIndex = dispatchThreadID.x;
    uint visibleCount = min(gVisibleCounter[0], gMaxVisibleTriangles);
    if (visibleIndex >= visibleCount)
    {
        return;
    }

    TriangleSetup tri = gVisibleTriangles[visibleIndex];
    uint triangleMinDepth = DepthToUint(min(tri.z0, min(tri.z1, tri.z2)));

    for (uint ty = tri.tileMin.y; ty <= tri.tileMax.y; ++ty)
    {
        for (uint tx = tri.tileMin.x; tx <= tri.tileMax.x; ++tx)
        {
            uint tileIndex = FlattenTile(uint2(tx, ty));
            uint coarseHint = gTileDepthHint[tileIndex];
            if (triangleMinDepth > coarseHint)
            {
                InterlockedAdd(gProfilingCounters[PROFILE_HIZ_TILE_SKIPS], 1u);
                continue;
            }

            uint listIndex;
            InterlockedAdd(gTileCounters[tileIndex], 1, listIndex);

            if (listIndex < gMaxTrianglesPerTile)
            {
                uint dst = tileIndex * gMaxTrianglesPerTile + listIndex;
                gTileTriangleIndices[dst] = visibleIndex;
            }
            else
            {
                InterlockedAdd(gTileOverflow[tileIndex], 1);
                InterlockedAdd(gProfilingCounters[PROFILE_TILE_OVERFLOWS], 1u);
            }
        }
    }
}

groupshared TriangleSetup sTriangle;
groupshared uint sTileTriangleCache[MAX_TILE_TRIANGLES + SHARED_BANK_PAD];
groupshared uint sTileFarthestDepth;
groupshared uint sCoveredCount;

float2 GetMSAASampleOffset(uint sampleIndex, uint sampleCount)
{
    if (sampleCount <= 1u)
    {
        return float2(0.5f, 0.5f);
    }

    if (sampleCount == 2u)
    {
        return (sampleIndex == 0u) ? float2(0.25f, 0.25f) : float2(0.75f, 0.75f);
    }

    if (sampleCount == 4u)
    {
        static const float2 offsets4[4] =
        {
            float2(0.375f, 0.125f),
            float2(0.875f, 0.375f),
            float2(0.125f, 0.625f),
            float2(0.625f, 0.875f)
        };
        return offsets4[min(sampleIndex, 3u)];
    }

    static const float2 offsets8[8] =
    {
        float2(0.5625f, 0.3125f),
        float2(0.4375f, 0.6875f),
        float2(0.8125f, 0.5625f),
        float2(0.3125f, 0.1875f),
        float2(0.1875f, 0.8125f),
        float2(0.0625f, 0.4375f),
        float2(0.6875f, 0.9375f),
        float2(0.9375f, 0.0625f)
    };
    return offsets8[min(sampleIndex, 7u)];
}

float2 GetStochasticSampleOffset(uint2 pixel, uint triSeed, uint sampleIndex, uint sampleCount)
{
    float2 sampleOffset = GetMSAASampleOffset(sampleIndex, sampleCount);
    if (gEnableStochasticSampling == 0u)
    {
        return sampleOffset;
    }

    uint hashSeed = pixel.x * 73856093u ^ pixel.y * 19349663u ^ triSeed * 83492791u ^ gFrameIndex * 2654435761u ^ sampleIndex * 374761393u;
    float2 jitter = Hash02(hashSeed) - 0.5f.xx;
    return saturate(sampleOffset + jitter * gStochasticJitterStrength);
}

float2 ComputeTemporalVelocityPx(TriangleSetup tri, uint2 pixel)
{
    uint seed = tri.pad3 ^ (DecodeTriangleFlags(tri) << 24u) ^ gFrameIndex * 2246822519u;
    float2 currJitter = (gEnableStochasticSampling != 0u) ? ((Hash02(seed) - 0.5f.xx) * (gStochasticJitterStrength * 2.0f)) : 0.0f.xx;
    float2 prevJitter = (gEnableStochasticSampling != 0u && gFrameIndex > 0u)
        ? ((Hash02(seed ^ 0x9e3779b9u) - 0.5f.xx) * (gStochasticJitterStrength * 2.0f))
        : 0.0f.xx;

    float2 velocityPx = (currJitter - prevJitter) * gTemporalMotionScale;
    uint flags = DecodeTriangleFlags(tri);
    if ((flags & TRI_FLAG_PORTAL) != 0u)
    {
        velocityPx += gPortalViewShift * (gTemporalMotionScale * 4.0f);
    }
    if ((flags & TRI_FLAG_TOPOLOGY) != 0u)
    {
        velocityPx += (Hash02(seed ^ 0x85ebca6bu) - 0.5f.xx) * gTopologyFoldStrength;
    }
    if ((flags & TRI_FLAG_PROCEDURAL) != 0u)
    {
        velocityPx += (Hash02(seed ^ 0xc2b2ae35u) - 0.5f.xx) * (gTemporalMotionScale * 1.5f);
    }
    return velocityPx;
}

float4 ResolveTemporalHistory(uint2 pixel, float4 currentColor, uint currentPackedTemporal)
{
    gHistoryTarget[pixel] = currentColor;
    if (gEnableTemporalReprojection == 0u || gUsePacked32Path > 0.5f || gFrameIndex == 0u)
    {
        return currentColor;
    }

    uint currentDepth16;
    float2 velocityPx;
    UnpackTemporalMetadata(currentPackedTemporal, currentDepth16, velocityPx);

    int2 reprojectedPixel = clamp(int2(round(float2(pixel) - velocityPx)), int2(0, 0), int2(int(gViewportWidth) - 1, int(gViewportHeight) - 1));
    float4 historyColor = gPreviousHistoryTarget[reprojectedPixel];
    uint previousPackedTemporal = gPreviousPackedTemporal[FlattenPixel(uint2(reprojectedPixel))];
    uint previousDepth16;
    float2 previousVelocityPx;
    UnpackTemporalMetadata(previousPackedTemporal, previousDepth16, previousVelocityPx);
    InterlockedAdd(gProfilingCounters[PROFILE_TAA_HISTORY_SAMPLES], 1u);

    float depthDelta = abs(float(currentDepth16) - float(previousDepth16));
    bool rejectHistory = depthDelta > gTemporalDepthThreshold || length(velocityPx) > kVelocityPixelRange;
    if (rejectHistory)
    {
        InterlockedAdd(gProfilingCounters[PROFILE_TAA_REJECTIONS], 1u);
        gHistoryTarget[pixel] = currentColor;
        return currentColor;
    }

    float historyWeight = saturate(gTemporalBlendWeight - depthDelta / max(gTemporalDepthThreshold, 1.0f) * 0.35f);
    float predictionBias = saturate(length(velocityPx) / max(gTemporalMotionScale, 0.001f));
    float4 predictedColor = lerp(historyColor, currentColor, predictionBias * 0.4f + 0.2f);
    float4 resolvedColor = lerp(currentColor, predictedColor, historyWeight);
    InterlockedAdd(gProfilingCounters[PROFILE_TAA_ACCEPTS], 1u);
    gHistoryTarget[pixel] = resolvedColor;
    return resolvedColor;
}

[allow_uav_condition]
bool DepthColorCommit(uint pixelIndex, uint incomingDepth, float4 sourceColor)
{
    while (true)
    {
        uint64_t currentPacked = gPackedDepthColor[pixelIndex];
        uint currentDepth;
        uint currentColorPacked;
        UnpackDepthColor(currentPacked, currentDepth, currentColorPacked);

        if (incomingDepth >= currentDepth)
        {
            return false;
        }

        float4 destinationColor = UnpackRGBA8(currentColorPacked);
        float4 outColor = (gAlphaBlendEnable > 0.5f) ? BlendSourceOver(sourceColor, destinationColor) : sourceColor;
        uint nextColorPacked = PackRGBA8(outColor);
        uint64_t nextPacked = PackDepthColor(incomingDepth, nextColorPacked);

        uint64_t observedPacked;
        InterlockedCompareExchange(gPackedDepthColor[pixelIndex], currentPacked, nextPacked, observedPacked);

        if (observedPacked == currentPacked)
        {
            return true;
        }
    }
}

[allow_uav_condition]
bool DepthColorCommit32(uint pixelIndex, uint incomingDepth24, float4 sourceColor)
{
    while (true)
    {
        uint currentPacked = gPackedDepthColor32[pixelIndex];
        uint currentDepth24 = (currentPacked >> 8) & kDepth24Max;
        if (incomingDepth24 >= currentDepth24)
        {
            return false;
        }

        uint nextPacked = PackDepthColor32(incomingDepth24, sourceColor);
        uint observedPacked;
        InterlockedCompareExchange(gPackedDepthColor32[pixelIndex], currentPacked, nextPacked, observedPacked);
        if (observedPacked == currentPacked)
        {
            return true;
        }
    }
}

[numthreads(TILE_SIZE, TILE_SIZE, 1)]
void CS_FineRasterize(uint3 groupID : SV_GroupID,
                      uint3 groupThreadID : SV_GroupThreadID,
                      uint3 dispatchThreadID : SV_DispatchThreadID)
{
    uint2 tileCoord = groupID.xy;
    if (tileCoord.x >= gTileCountX || tileCoord.y >= gTileCountY)
    {
        return;
    }

    uint tileIndex = FlattenTile(tileCoord);
    uint lane = groupThreadID.y * TILE_SIZE + groupThreadID.x;
    uint2 pixel = tileCoord * TILE_SIZE + groupThreadID.xy;
    bool pixelCovered = false;

    if (lane == 0)
    {
        sTileFarthestDepth = 0u;
        sCoveredCount = 0;
        InterlockedAdd(gProfilingCounters[PROFILE_TILES], 1u);
    }
    GroupMemoryBarrierWithGroupSync();

    uint triangleCountForTile = min(gTileCounters[tileIndex], min(gMaxTrianglesPerTile, MAX_TILE_TRIANGLES));
    if (lane == 0)
    {
        InterlockedAdd(gProfilingCounters[PROFILE_TILE_TRIANGLE_REFS], triangleCountForTile);
        InterlockedMax(gProfilingCounters[PROFILE_MAX_TILE_TRIANGLES], triangleCountForTile);
    }
    if (lane < triangleCountForTile)
    {
        uint srcIndex = tileIndex * gMaxTrianglesPerTile + lane;
        sTileTriangleCache[SharedTriangleCacheIndex(lane)] = gTileTriangleIndices[srcIndex];
    }
    GroupMemoryBarrierWithGroupSync();

    if (lane == 0)
    {
        for (uint i = 1; i < triangleCountForTile; ++i)
        {
            uint keyTriIndex = sTileTriangleCache[SharedTriangleCacheIndex(i)];
            float keyArea = abs(gVisibleTriangles[keyTriIndex].area2);
            int j = int(i) - 1;

            while (j >= 0)
            {
                uint prevTriIndex = sTileTriangleCache[SharedTriangleCacheIndex((uint)j)];
                float prevArea = abs(gVisibleTriangles[prevTriIndex].area2);
                if (prevArea >= keyArea)
                {
                    break;
                }

                sTileTriangleCache[SharedTriangleCacheIndex((uint)j + 1u)] = prevTriIndex;
                --j;
            }

            sTileTriangleCache[SharedTriangleCacheIndex((uint)j + 1u)] = keyTriIndex;
        }
    }
    GroupMemoryBarrierWithGroupSync();

    for (uint triListIndex = 0; triListIndex < triangleCountForTile; ++triListIndex)
    {
        if (lane == 0)
        {
            uint triIndex = sTileTriangleCache[SharedTriangleCacheIndex(triListIndex)];
            sTriangle = gVisibleTriangles[triIndex];
            InterlockedAdd(gTileDebugCounts[tileIndex], 1);
        }
        GroupMemoryBarrierWithGroupSync();

        uint triangleMinDepth = DepthToUint(min(sTriangle.z0, min(sTriangle.z1, sTriangle.z2)));
        if (sCoveredCount == TILE_SIZE * TILE_SIZE && triangleMinDepth > sTileFarthestDepth)
        {
            GroupMemoryBarrierWithGroupSync();
            continue;
        }

        bool inBounds = (pixel.x < gViewportWidth && pixel.y < gViewportHeight);
        bool covered = false;
        float alpha = 0.0f;
        float beta = 0.0f;
        float gamma = 0.0f;
        float2 samplePosForMip = 0.0f.xx;
        uint sampleCount = min(max(gMsaaSampleCount, 1u), 8u);
        uint coveredSamples = 0u;
        uint triangleFlags = DecodeTriangleFlags(sTriangle);

        if (inBounds)
        {
            if ((triangleFlags & TRI_FLAG_TINY) != 0u)
            {
                float2 centroid = (sTriangle.p0 + sTriangle.p1 + sTriangle.p2) * (1.0f / 3.0f);
                uint2 splatPixel = (uint2)clamp(centroid, 0.0f.xx, float2(gViewportWidth - 1, gViewportHeight - 1));
                if (all(pixel == splatPixel))
                {
                    covered = true;
                    coveredSamples = sampleCount;
                    float2 samplePos = float2(splatPixel) + 0.5f.xx;
                    float e12 = EdgeFunction(sTriangle.p1, sTriangle.p2, samplePos);
                    float e20 = EdgeFunction(sTriangle.p2, sTriangle.p0, samplePos);
                    float e01 = EdgeFunction(sTriangle.p0, sTriangle.p1, samplePos);
                    alpha = e12 * sTriangle.invArea2;
                    beta = e20 * sTriangle.invArea2;
                    gamma = e01 * sTriangle.invArea2;
                    samplePosForMip = samplePos;
                    InterlockedAdd(gProfilingCounters[PROFILE_MICRO_SPLATS], 1u);
                }
            }
            else
            {
                [loop]
                for (uint sampleIndex = 0u; sampleIndex < sampleCount; ++sampleIndex)
                {
                    float2 samplePos = float2(pixel) + GetStochasticSampleOffset(pixel, sTriangle.pad3, sampleIndex, sampleCount);
                    float e12 = EdgeFunction(sTriangle.p1, sTriangle.p2, samplePos);
                    float e20 = EdgeFunction(sTriangle.p2, sTriangle.p0, samplePos);
                    float e01 = EdgeFunction(sTriangle.p0, sTriangle.p1, samplePos);
                    bool sampleCovered = (e12 >= 0.0f && e20 >= 0.0f && e01 >= 0.0f);
                    if (sampleCovered)
                    {
                        covered = true;
                        coveredSamples++;
                        alpha += e12 * sTriangle.invArea2;
                        beta += e20 * sTriangle.invArea2;
                        gamma += e01 * sTriangle.invArea2;
                        samplePosForMip += samplePos;
                    }
                }
                if (gEnableStochasticSampling != 0u && coveredSamples > 0u)
                {
                    InterlockedAdd(gProfilingCounters[PROFILE_STOCHASTIC_SAMPLES], coveredSamples);
                }
            }
        }

        if (WaveIsFirstLane())
        {
            InterlockedAdd(gProfilingCounters[PROFILE_WAVE_TESTS], 1u);
        }
        bool waveHasCoverage = WaveActiveAnyTrue(covered);
        if (waveHasCoverage && WaveIsFirstLane())
        {
            InterlockedAdd(gProfilingCounters[PROFILE_WAVE_COVERED], 1u);
        }
        if (waveHasCoverage && covered && coveredSamples > 0u)
        {
            if ((triangleFlags & TRI_FLAG_TINY) == 0u)
            {
                float invCoveredSamples = rcp(float(coveredSamples));
                alpha *= invCoveredSamples;
                beta *= invCoveredSamples;
                gamma *= invCoveredSamples;
                samplePosForMip *= invCoveredSamples;
            }

            float invWPixel = alpha * sTriangle.invW0 + beta * sTriangle.invW1 + gamma * sTriangle.invW2;
            float wPixel = rcp(invWPixel);

            float2 uv = (alpha * sTriangle.uv0OverW +
                         beta * sTriangle.uv1OverW +
                         gamma * sTriangle.uv2OverW) * wPixel;

            float3 normal = (alpha * sTriangle.n0OverW +
                             beta * sTriangle.n1OverW +
                             gamma * sTriangle.n2OverW) * wPixel;

            if ((triangleFlags & TRI_FLAG_TOPOLOGY) != 0u)
            {
                uv = ApplyTopologyTraversal(uv, samplePosForMip, DecodeTopologyNode(sTriangle));
                InterlockedAdd(gProfilingCounters[PROFILE_TOPOLOGY_REMAPS], 1u);
            }

            if ((triangleFlags & TRI_FLAG_PORTAL) != 0u)
            {
                uv = frac(uv + gPortalViewShift * float(DecodePortalTarget(sTriangle) + 1u));
                normal = normalize(float3(normal.xy, abs(normal.z) + 0.25f));
            }

            float depth = alpha * sTriangle.z0 + beta * sTriangle.z1 + gamma * sTriangle.z2;
            uint depthU32 = DepthToUint(depth);
            uint depthU24 = DepthToUint24(depth);
            uint mipLevel = (uint)ComputeMipLevel(gTexture, sTriangle, samplePosForMip);
            float coverageScale = float(coveredSamples) * rcp(float(sampleCount));
            float4 shaded = ShadeFragment(uv, normal, mipLevel);
            shaded.rgb *= coverageScale;
            shaded.a *= coverageScale;

            uint pixelIndex = FlattenPixel(pixel);
            uint materialDensity = DecodeMaterialDensity(sTriangle);
            InterlockedAdd(gProfilingCounters[PROFILE_COMMIT_ATTEMPTS], 1u);
            InterlockedAdd(gProfilingCounters[PROFILE_COVERED_SAMPLES], coveredSamples);
            bool committed = (gUsePacked32Path > 0.5f)
                ? DepthColorCommit32(pixelIndex, depthU24, shaded)
                : DepthColorCommit(pixelIndex, depthU32, shaded);

            if (committed)
            {
                InterlockedAdd(gProfilingCounters[PROFILE_COMMIT_SUCCESSES], 1u);
                InterlockedMax(sTileFarthestDepth, depthU32);
                if (!pixelCovered)
                {
                    pixelCovered = true;
                    InterlockedAdd(sCoveredCount, 1);
                }
                if ((triangleFlags & TRI_FLAG_TINY) != 0u)
                {
                    InterlockedAdd(gProfilingCounters[PROFILE_TINY_TRIANGLES], 1u);
                }
                if ((triangleFlags & TRI_FLAG_PORTAL) != 0u)
                {
                    gPortalMask[pixelIndex] = DecodePortalTarget(sTriangle);
                    InterlockedAdd(gProfilingCounters[PROFILE_PORTAL_PIXELS], 1u);
                }
                if (gUsePacked32Path <= 0.5f)
                {
                    float2 velocityPx = ComputeTemporalVelocityPx(sTriangle, pixel);
                    gPackedDepthColor32[pixelIndex] = PackTemporalMetadata(DepthToUint16(depth), velocityPx);
                }
                gMaterialDensity[pixelIndex] = materialDensity;
                InterlockedAdd(gProfilingCounters[PROFILE_MATERIAL_WRITES], 1u);

                float densityScale = 0.35f + float(materialDensity) / 255.0f;
                uint occlusionContribution = (uint)max(1.0f, depth * coverageScale * densityScale * gAcousticOcclusionScale);
                InterlockedAdd(gTileAcousticOcclusion[tileIndex], occlusionContribution);

                bool isDiffractionEdge = (coveredSamples < sampleCount) || min(alpha, min(beta, gamma)) < 0.04f;
                if (isDiffractionEdge)
                {
                    uint diffractionContribution = (uint)max(1.0f, gAcousticDiffractionScale * densityScale);
                    InterlockedAdd(gTileAcousticDiffraction[tileIndex], diffractionContribution);
                    InterlockedAdd(gProfilingCounters[PROFILE_AUDIO_EDGE_HITS], 1u);
                }
            }
        }

        GroupMemoryBarrierWithGroupSync();
    }

    if (lane == 0)
    {
        gTileDepthHint[tileIndex] = (sCoveredCount == TILE_SIZE * TILE_SIZE) ? sTileFarthestDepth : 0xffffffffu;
    }
}

[numthreads(TILE_SIZE, TILE_SIZE, 1)]
void CS_Present(uint3 dispatchThreadID : SV_DispatchThreadID)
{
    uint2 pixel = dispatchThreadID.xy;
    if (pixel.x >= gViewportWidth || pixel.y >= gViewportHeight)
    {
        return;
    }

    uint tileIndex = FlattenTile(pixel / TILE_SIZE);
    if (gDebugHeatmapMode > 0.5f)
    {
        float4 debugColor = HeatmapColor(gTileDebugCounts[tileIndex], gHeatmapScale);
        gHistoryTarget[pixel] = debugColor;
        gPresentTarget[pixel] = debugColor;
        return;
    }

    if (gAudioDebugMode == 1u)
    {
        float occlusion = saturate(float(gTileAcousticOcclusion[tileIndex]) / max(gAcousticOcclusionScale * 12.0f, 1.0f));
        float diffraction = saturate(float(gTileAcousticDiffraction[tileIndex]) / max(gAcousticDiffractionScale * 6.0f, 1.0f));
        float4 debugColor = float4(occlusion, diffraction, 1.0f - occlusion, 1.0f);
        gHistoryTarget[pixel] = debugColor;
        gPresentTarget[pixel] = debugColor;
        return;
    }

    uint pixelIndex = FlattenPixel(pixel);
    if (gAudioDebugMode == 2u)
    {
        float density = float(gMaterialDensity[pixelIndex] & 0xffu) / 255.0f;
        float4 debugColor = float4(density, density * 0.5f, 1.0f - density, 1.0f);
        gHistoryTarget[pixel] = debugColor;
        gPresentTarget[pixel] = debugColor;
        return;
    }

    uint portalTarget = gPortalMask[pixelIndex];
    if (gUsePacked32Path > 0.5f)
    {
        uint depth24;
        float4 color32;
        UnpackDepthColor32(gPackedDepthColor32[pixelIndex], depth24, color32);
        float4 outputColor = (portalTarget != kInvalidPortal)
            ? ApplyPortalComposite(color32, portalTarget, (float2(pixel) + 0.5f) * float2(gInvViewportWidth, gInvViewportHeight))
            : color32;
        gHistoryTarget[pixel] = outputColor;
        gPresentTarget[pixel] = outputColor;
    }
    else
    {
        uint depthPacked;
        uint colorPacked;
        UnpackDepthColor(gPackedDepthColor[pixelIndex], depthPacked, colorPacked);
        float4 color64 = UnpackRGBA8(colorPacked);
        float4 portalComposite = (portalTarget != kInvalidPortal)
            ? ApplyPortalComposite(color64, portalTarget, (float2(pixel) + 0.5f) * float2(gInvViewportWidth, gInvViewportHeight))
            : color64;
        float4 temporalResolved = ResolveTemporalHistory(pixel, portalComposite, gPackedDepthColor32[pixelIndex]);
        gPresentTarget[pixel] = temporalResolved;
    }
}
