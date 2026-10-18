#ifndef SHADER_COMMON_H_INCLUDED
#define SHADER_COMMON_H_INCLUDED

#define SPEC_CONSTANT_R11G11B10_NORMAL  (1 << 0)
#define SPEC_CONSTANT_ALPHA_TEST        (1 << 1)

#ifdef CONAN_RECOMP
    // Alpha test converted to alpha-to-coverage (foliage antialiasing option).
    #define SPEC_CONSTANT_ALPHA_TO_COVERAGE (1 << 3)
    #define SPEC_CONSTANT_SOFT_PARTICLE_RGBA (1 << 4)
    #define SPEC_CONSTANT_SOFT_PARTICLE_ALPHA (1 << 5)
#endif
#ifdef UNLEASHED_RECOMP
    #define SPEC_CONSTANT_BICUBIC_GI_FILTER (1 << 2)
    #define SPEC_CONSTANT_ALPHA_TO_COVERAGE (1 << 3)
    #define SPEC_CONSTANT_REVERSE_Z         (1 << 4)
#endif

// SPIR-V vertex input locations, shared with reblue's host input-layout
// builder. A semantic missing here gets no location from the emitter.
#define REBLUE_VERTEX_INPUT_LOCATIONS(X) \
    X(Position,  0,  0) \
    X(Position,  1,  1) \
    X(Position,  2,  2) \
    X(Position,  3,  3) \
    X(Position,  4,  4) \
    X(Normal,    0,  5) \
    X(Tangent,   0,  6) \
    X(TexCoord,  0,  7) \
    X(TexCoord,  1,  8) \
    X(TexCoord,  2,  9) \
    X(Color,     0, 10)

// SPEC_CONSTANTS_ONLY keeps the HLSL below out of host C++ TUs, which IntelliSense would otherwise parse.
#if (!defined(__cplusplus) || defined(__INTELLISENSE__)) && !defined(SHADER_COMMON_SPEC_CONSTANTS_ONLY)

#define FLT_MIN asfloat(0xff7fffff)
#define FLT_MAX asfloat(0x7f7fffff)

#ifdef __spirv__

#ifdef SR_VULKAN_BUFFERS
[[vk::binding(0,0)]] ByteAddressBuffer g_VertexShaderConstants;
[[vk::binding(1,0)]] ByteAddressBuffer g_PixelShaderConstants;
[[vk::binding(2,0)]] ByteAddressBuffer g_SharedConstants;
#define SR_LOAD_FLOAT4(STAGE,OFFSET) asfloat(g_ ## STAGE ## ShaderConstants.Load4(OFFSET))
#define SR_SHARED_UINT(OFFSET) g_SharedConstants.Load(OFFSET)
#define SR_SHARED_UINT4(OFFSET) g_SharedConstants.Load4(OFFSET)
#define SR_SHARED_FLOAT(OFFSET) asfloat(g_SharedConstants.Load(OFFSET))
#define SR_SHARED_FLOAT2(OFFSET) asfloat(g_SharedConstants.Load2(OFFSET))
#define SR_SHARED_FLOAT4(OFFSET) asfloat(g_SharedConstants.Load4(OFFSET))
#else

struct PushConstants
{
    uint64_t VertexShaderConstants;
    uint64_t PixelShaderConstants;
    uint64_t SharedConstants;
};

[[vk::push_constant]] ConstantBuffer<PushConstants> g_PushConstants;

#define SR_LOAD_FLOAT4(STAGE,OFFSET) vk::RawBufferLoad<float4>(g_PushConstants.STAGE ## ShaderConstants + (OFFSET), 0x10)
#define SR_SHARED_UINT(OFFSET) vk::RawBufferLoad<uint>(g_PushConstants.SharedConstants + (OFFSET))
#define SR_SHARED_UINT4(OFFSET) vk::RawBufferLoad<uint4>(g_PushConstants.SharedConstants + (OFFSET))
#define SR_SHARED_FLOAT(OFFSET) vk::RawBufferLoad<float>(g_PushConstants.SharedConstants + (OFFSET))
#define SR_SHARED_FLOAT2(OFFSET) vk::RawBufferLoad<float2>(g_PushConstants.SharedConstants + (OFFSET))
#define SR_SHARED_FLOAT4(OFFSET) vk::RawBufferLoad<float4>(g_PushConstants.SharedConstants + (OFFSET))
#endif

#ifdef REBLUE_RECOMP
// 256-bit boolean register file (BD bool addresses reach ~158), then per-usage 16-bit-pair swap masks.
#define g_Booleans(i)              SR_SHARED_UINT(256 + (i)*4)
#define g_SwappedTexcoords         SR_SHARED_UINT(288)
#define g_HalfPixelOffset          SR_SHARED_FLOAT2(292)
#define g_AlphaThreshold           SR_SHARED_FLOAT(300)
#define g_SwappedNormals           SR_SHARED_UINT(304)
#define g_SwappedBinormals         SR_SHARED_UINT(308)
#define g_SwappedTangents          SR_SHARED_UINT(312)
#define g_SwappedBlendWeights      SR_SHARED_UINT(316)
#define g_SwappedPositions         SR_SHARED_UINT(320)
#define g_SintTexcoords            SR_SHARED_UINT(324)
#define g_ShadowPcfScale           SR_SHARED_FLOAT(328)
#elif defined(CONAN_RECOMP)
// Conan layout (tools/xenosrecomp/patches): c16-c17 = the 256-bit Xenos bool file
// (VS 0..127, PS 128..255, LSB-first per dword like SHADER_CONSTANT_BOOL_*),
// c18 = misc, c19-c26 = the 32 Xenos loop constants (SHADER_CONSTANT_LOOP_00..31:
// count bits 0-7, start bits 8-15, signed step bits 16-23; VS i0-15 = 0-15,
// PS i0-15 = 16-31).
#define g_Booleans(i)              SR_SHARED_UINT(256 + (i)*4)
#define g_SwappedTexcoords         SR_SHARED_UINT(288)
#define g_HalfPixelOffset          SR_SHARED_FLOAT2(292)
#define g_AlphaThreshold           SR_SHARED_FLOAT(300)
#define g_VertexFetch(i)           SR_SHARED_UINT4(512 + (i)*16)
#define g_LoopConstant(i)          SR_SHARED_UINT(304 + (i)*4)
#define g_ScreenXform              SR_SHARED_FLOAT4(432)
#define g_SpecConstantsRuntime     SR_SHARED_UINT(448)
#else
#define g_Booleans                 SR_SHARED_UINT(256)
#define g_SwappedTexcoords         SR_SHARED_UINT(260)
#define g_HalfPixelOffset          SR_SHARED_FLOAT2(264)
#define g_AlphaThreshold           SR_SHARED_FLOAT(272)
#endif

#ifdef SR_VULKAN_BUFFERS
#define g_PixelPosScale SR_SHARED_FLOAT(452)
#define g_ShadowAtlasTexelScale SR_SHARED_FLOAT(456)
#define g_ShadowSoftness SR_SHARED_FLOAT(460)
#define g_SoftParticleW SR_SHARED_FLOAT2(464)
#define g_SoftParticleDistance SR_SHARED_FLOAT(472)
#define g_SoftParticleTexelScale SR_SHARED_FLOAT(476)
#define g_SoftParticleDepth SR_SHARED_UINT(480)
#define g_SpecConstants() g_SpecConstantsRuntime
#else
[[vk::constant_id(0)]] const uint g_SpecConstants = 0;
#define g_SpecConstants() g_SpecConstants
#endif

#else

#ifdef REBLUE_RECOMP
#define DEFINE_SHARED_CONSTANTS() \
    uint4 g_BooleansArr[2] : packoffset(c16); \
    uint g_SwappedTexcoords : packoffset(c18.x); \
    float2 g_HalfPixelOffset : packoffset(c18.y); \
    float g_AlphaThreshold : packoffset(c18.w); \
    uint g_SwappedNormals : packoffset(c19.x); \
    uint g_SwappedBinormals : packoffset(c19.y); \
    uint g_SwappedTangents : packoffset(c19.z); \
    uint g_SwappedBlendWeights : packoffset(c19.w); \
    uint g_SwappedPositions : packoffset(c20.x); \
    uint g_SintTexcoords : packoffset(c20.y); \
    float g_ShadowPcfScale : packoffset(c20.z);

#define g_Booleans(i) (g_BooleansArr[(i) / 4][(i) % 4])
#elif defined(CONAN_RECOMP)
#define DEFINE_SHARED_CONSTANTS() \
    uint4 g_BooleansArr[2] : packoffset(c16); \
    uint g_SwappedTexcoords : packoffset(c18.x); \
    float2 g_HalfPixelOffset : packoffset(c18.y); \
    float g_AlphaThreshold : packoffset(c18.w); \
    uint4 g_VertexFetchArr[224] : packoffset(c32); \
    uint4 g_LoopConstantsArr[8] : packoffset(c19); \
    float4 g_ScreenXform : packoffset(c27);     uint g_SpecConstantsRuntime : packoffset(c28.x); float g_PixelPosScale : packoffset(c28.y); float g_ShadowAtlasTexelScale : packoffset(c28.z); float g_ShadowSoftness : packoffset(c28.w); float2 g_SoftParticleW : packoffset(c29.x); float g_SoftParticleDistance : packoffset(c29.z); float g_SoftParticleTexelScale : packoffset(c29.w); uint g_SoftParticleDepth : packoffset(c30.x);

#define g_Booleans(i) (g_BooleansArr[(i) / 4][(i) % 4])
#define g_VertexFetch(i) (g_VertexFetchArr[i])
#define g_LoopConstant(i) (g_LoopConstantsArr[(i) / 4][(i) % 4])
#else
#define DEFINE_SHARED_CONSTANTS() \
    uint g_Booleans : packoffset(c16.x); \
    uint g_SwappedTexcoords : packoffset(c16.y); \
    float2 g_HalfPixelOffset : packoffset(c16.z); \
    float g_AlphaThreshold : packoffset(c17.x);
#endif

uint g_SpecConstants();

#endif

#if defined(REBLUE_RECOMP) || defined(CONAN_RECOMP)
// Test Xenos boolean register N in the unified VS(0..127)/PS(128..255) file.
#define BOOL_BIT(n) ((g_Booleans((n) / 32u) & (1u << ((n) & 31u))) != 0)
#endif

#ifdef CONAN_RECOMP
// xenos::LoopConstant: aL = start + iteration * step.
// Macros take the 32-bit loop constant value: g_LoopConstant(id), or a literal
// from the shader's definition table (defi).
#define LOOP_COUNT(c) ((c) & 0xFFu)
#define LOOP_START(c) int(((c) >> 8) & 0xFFu)
#define LOOP_STEP(c)  (int((c) << 8) >> 24)
#endif

#ifdef CONAN_RECOMP
// Bit 31 of a texture descriptor index = the fetch constant's RGB sign is GAMMA:
// Xenos returns linear values, converted with its piecewise-linear curve
// (xenos::PWLGammaToLinear, applied after filtering like the Xenia/ReXGlue
// Xenos backend).
// Bits 16-27 = host scale of a texture rendered larger than its guest size
// (render_scale resolve targets, 8.8 fixed point, 0 = 1:1): texel offsets and
// filter weights stay in guest texels, like on Xenos. SRV indices use bits 0-14.
#define TEX_INDEX(i) ((i) & 0x7FFFu)
#define TEX_SCALE(i) ((((i) >> 16) & 0xFFFu) ? float(((i) >> 16) & 0xFFFu) / 256.0 : 1.0)
float3 conanPwlGammaToLinear(float3 g)
{
    g = saturate(g);
    float3 hi = select(g >= 192.0f / 255.0f, 8.0f / 1024.0f, 4.0f / 1024.0f);
    float3 lo = select(g >= 64.0f / 255.0f, 2.0f / 1024.0f, 1.0f / 1024.0f);
    float3 scale = select(g >= 96.0f / 255.0f, hi, lo);
    float3 offset = select(g >= 96.0f / 255.0f, select(g >= 192.0f / 255.0f, -1024.0f, -256.0f),
                           select(g >= 64.0f / 255.0f, -64.0f, 0.0f));
    float3 l = g * ((255.0 * 1024.0) * scale) + offset;
    l += trunc(l * scale);
    return l * (1.0 / 1023.0);
}
float4 conanGamma(uint resourceDescriptorIndex, float4 value)
{
    if (resourceDescriptorIndex & 0x80000000u)
        value.rgb = conanPwlGammaToLinear(value.rgb);
    return value;
}
#else
#define TEX_INDEX(i) (i)
#define TEX_SCALE(i) 1.0
#define conanGamma(i, v) (v)
#endif

#ifdef SR_VULKAN_BUFFERS
[[vk::binding(0,1)]] Texture2D<float4> g_Texture2DDescriptorHeap[32];
#else
Texture2D<float4> g_Texture2DDescriptorHeap[] : register(t0, space0);
#endif
#ifdef SR_VULKAN_BUFFERS
[[vk::binding(1,1)]] Texture3D<float4> g_Texture3DDescriptorHeap[32];
#else
Texture3D<float4> g_Texture3DDescriptorHeap[] : register(t0, space1);
#endif
#ifdef SR_VULKAN_BUFFERS
[[vk::binding(2,1)]] TextureCube<float4> g_TextureCubeDescriptorHeap[32];
#else
TextureCube<float4> g_TextureCubeDescriptorHeap[] : register(t0, space2);
#endif
// Stacked (2D array) textures fetched through a 3D instruction. Xenos reads
// array layer z * layers there (the hardware rounds the layer coordinate), not
// a volume; the renderer sets TEX_STACKED_BIT on the 3D index of such a slot
// and points it at an array view of the texture.
#ifdef SR_VULKAN_BUFFERS
[[vk::binding(3,1)]] Texture2DArray<float4> g_Texture2DArrayDescriptorHeap[32];
#else
Texture2DArray<float4> g_Texture2DArrayDescriptorHeap[] : register(t0, space6);
#endif
#define TEX_STACKED_BIT 0x10000000u
#ifdef SR_VULKAN_BUFFERS
[[vk::binding(0,2)]] SamplerState g_SamplerDescriptorHeap[32];
#else
SamplerState g_SamplerDescriptorHeap[] : register(s0, space3);
#endif

uint2 getTexture2DDimensions(Texture2D<float4> texture)
{
    uint2 dimensions;
    texture.GetDimensions(dimensions.x, dimensions.y);
    return dimensions;
}

// Texel grid the guest shader addresses (the guest texture size).
float2 getGuestTexture2DDimensions(uint resourceDescriptorIndex, Texture2D<float4> texture)
{
    return float2(getTexture2DDimensions(texture)) / TEX_SCALE(resourceDescriptorIndex);
}

float4 tfetch2DBicubic(uint resourceDescriptorIndex, uint samplerDescriptorIndex, float2 texCoord, float2 offset);

float4 tfetch2D(uint resourceDescriptorIndex, uint samplerDescriptorIndex, float2 texCoord, float2 offset)
{
#ifdef CONAN_RECOMP
    // Bit 28: smooth (cubic B-spline) magnification, set by the host for small
    // textures of blended effects (particles, glows) - bilinear magnification
    // of those shows diamond/block patterns at high render resolutions.
    if (resourceDescriptorIndex & 0x10000000u)
        return conanGamma(resourceDescriptorIndex, tfetch2DBicubic(resourceDescriptorIndex, samplerDescriptorIndex, texCoord, offset));
#endif
    Texture2D<float4> texture = g_Texture2DDescriptorHeap[TEX_INDEX(resourceDescriptorIndex)];
    return conanGamma(resourceDescriptorIndex, texture.Sample(g_SamplerDescriptorHeap[samplerDescriptorIndex], texCoord + offset / getGuestTexture2DDimensions(resourceDescriptorIndex, texture)));
}

float2 getWeights2D(uint resourceDescriptorIndex, uint samplerDescriptorIndex, float2 texCoord, float2 offset)
{
    Texture2D<float4> texture = g_Texture2DDescriptorHeap[TEX_INDEX(resourceDescriptorIndex)];
    return select(isnan(texCoord), 0.0f, frac(texCoord * getGuestTexture2DDimensions(resourceDescriptorIndex, texture) + offset - 0.5));
}

#ifdef REBLUE_RECOMP
// Bilinear-filtered shadow compare over the four neighboring depth texels.
float shadowCmp2D(uint resourceDescriptorIndex, float2 texCoord, float ref)
{
    Texture2D<float4> texture = g_Texture2DDescriptorHeap[TEX_INDEX(resourceDescriptorIndex)];
    int2 dimensions = int2(getTexture2DDimensions(texture));
    float2 coord = texCoord * dimensions - 0.5;
    float2 weights = frac(coord);
    int2 base = int2(floor(coord));
    int2 c0 = clamp(base, int2(0, 0), dimensions - 1);
    int2 c1 = clamp(base + 1, int2(0, 0), dimensions - 1);
    float4 taps = float4(
        texture.Load(int3(c0, 0)).x,
        texture.Load(int3(c1.x, c0.y, 0)).x,
        texture.Load(int3(c0.x, c1.y, 0)).x,
        texture.Load(int3(c1, 0)).x) > ref;
    return lerp(lerp(taps.x, taps.y, weights.x), lerp(taps.z, taps.w, weights.x), weights.y);
}
#endif

float w0(float a)
{
    return (1.0f / 6.0f) * (a * (a * (-a + 3.0f) - 3.0f) + 1.0f);
}

float w1(float a)
{
    return (1.0f / 6.0f) * (a * a * (3.0f * a - 6.0f) + 4.0f);
}

float w2(float a)
{
    return (1.0f / 6.0f) * (a * (a * (-3.0f * a + 3.0f) + 3.0f) + 1.0f);
}

float w3(float a)
{
    return (1.0f / 6.0f) * (a * a * a);
}

float g0(float a)
{
    return w0(a) + w1(a);
}

float g1(float a)
{
    return w2(a) + w3(a);
}

float h0(float a)
{
    return -1.0f + w1(a) / (w0(a) + w1(a)) + 0.5f;
}

float h1(float a)
{
    return 1.0f + w3(a) / (w2(a) + w3(a)) + 0.5f;
}

float4 tfetch2DBicubic(uint resourceDescriptorIndex, uint samplerDescriptorIndex, float2 texCoord, float2 offset)
{
    Texture2D<float4> texture = g_Texture2DDescriptorHeap[TEX_INDEX(resourceDescriptorIndex)];
    SamplerState samplerState = g_SamplerDescriptorHeap[samplerDescriptorIndex];
    float2 dimensions = getGuestTexture2DDimensions(resourceDescriptorIndex, texture);
    
    float x = texCoord.x * dimensions.x + offset.x;
    float y = texCoord.y * dimensions.y + offset.y;

    x -= 0.5f;
    y -= 0.5f;
    float px = floor(x);
    float py = floor(y);
    float fx = x - px;
    float fy = y - py;

    float g0x = g0(fx);
    float g1x = g1(fx);
    float h0x = h0(fx);
    float h1x = h1(fx);
    float h0y = h0(fy);
    float h1y = h1(fy);

    float4 r =
        g0(fy) * (g0x * texture.Sample(samplerState, float2(px + h0x, py + h0y) / float2(dimensions)) +
            g1x * texture.Sample(samplerState, float2(px + h1x, py + h0y) / float2(dimensions))) +
        g1(fy) * (g0x * texture.Sample(samplerState, float2(px + h0x, py + h1y) / float2(dimensions)) +
            g1x * texture.Sample(samplerState, float2(px + h1x, py + h1y) / float2(dimensions)));

    return r;
}

float4 tfetch3D(uint resourceDescriptorIndex, uint samplerDescriptorIndex, float3 texCoord)
{
    if (resourceDescriptorIndex & TEX_STACKED_BIT)
    {
        Texture2DArray<float4> stack = g_Texture2DArrayDescriptorHeap[TEX_INDEX(resourceDescriptorIndex)];
        uint width, height, layers;
        stack.GetDimensions(width, height, layers);
        return conanGamma(resourceDescriptorIndex, stack.Sample(g_SamplerDescriptorHeap[samplerDescriptorIndex],
            float3(texCoord.xy, texCoord.z * float(layers))));
    }
    return conanGamma(resourceDescriptorIndex, g_Texture3DDescriptorHeap[TEX_INDEX(resourceDescriptorIndex)].Sample(g_SamplerDescriptorHeap[samplerDescriptorIndex], texCoord));
}

struct CubeMapData
{
	#ifdef REBLUE_RECOMP
    // BD's cube-shadow PCF issues up to 9 cube ops per shader (bd_mirror_cs_ps /
    // bd_glass_cs_ps); overflowing this array drops the stored directions and
    // every tfetchCube reads OOB -> point-light shadows vanish.
    float3 cubeMapDirections[9];
    #else
    float3 cubeMapDirections[16];
    #endif
    uint cubeMapIndex;
};

float4 tfetchCube(uint resourceDescriptorIndex, uint samplerDescriptorIndex, float3 texCoord, inout CubeMapData cubeMapData)
{
    return conanGamma(resourceDescriptorIndex, g_TextureCubeDescriptorHeap[TEX_INDEX(resourceDescriptorIndex)].Sample(g_SamplerDescriptorHeap[samplerDescriptorIndex], cubeMapData.cubeMapDirections[texCoord.z]));
}

#ifdef CONAN_RECOMP
// D3D12 has no unsigned scaled 16-bit IA format. Bind USHORT2 as UNORM
// and restore its original integer values before the guest VS uses them.
#define srVertexInput(semantic, value) srVertexInputImpl(g_VertexFetch(semantic).w, value)
float4 srVertexInputImpl(uint type, float4 value)
{
    if (type == 0x2C2259)
        return float4(round(value.xy * 65535.0f), 0.0f, 1.0f);
    return value;
}
// Dynamically indexed Xenos fetches (instancing): host-order, interleaved
// vertex streams. Descriptor metadata comes from the actual D3D declaration.
#ifdef SR_VULKAN_BUFFERS
[[vk::binding(0,3)]] ByteAddressBuffer g_VertexBufferHeap[32];
#else
ByteAddressBuffer g_VertexBufferHeap[] : register(t0, space5);
#endif
#define srVertexFetch(semantic, index, packed) srVertexFetchImpl(g_VertexFetch(semantic), index, packed)
float4 srVertexFetchImpl(uint4 meta, float index, bool packed)
{
    // meta: SRV, byte offset, stride, XDK type
    if (meta.z == 0 || index < 0.0f) return 0.0f;
    uint address = meta.y + uint(index) * meta.z;
    uint bytes;
    g_VertexBufferHeap[meta.x].GetDimensions(bytes);
    uint type = meta.w;
    uint count = (type == 0x1A23A6 || type == 0x1A235A || type == 0x1A215A ||
                  type == 0x1A205A || type == 0x1A2360) ? 4u :
                 (type == 0x2A23B9 ? 3u :
                 (type == 0x2C23A5 || type == 0x2C2359 || type == 0x2C2159 ||
                  type == 0x2C2059 || type == 0x2C2259 || type == 0x2C235F) ? 2u : 1u);
    bool half = type == 0x1A235A || type == 0x1A215A || type == 0x1A205A ||
                type == 0x1A2360 || type == 0x2C2359 || type == 0x2C2159 ||
                type == 0x2C2059 || type == 0x2C2259 || type == 0x2C235F;
    uint required = count * (half ? 2u : 4u);
    if (address > bytes || required > bytes - address) return 0.0f;
    float4 value = float4(0.0f, 0.0f, 0.0f, 1.0f);
    for (uint c = 0; c < count; ++c)
    {
        uint at = address + c * (half ? 2u : 4u);
        uint word = g_VertexBufferHeap[meta.x].Load(at & ~3u);
        if (half)
        {
            uint h = (word >> ((at & 2u) * 8u)) & 65535u;
            int signedH = int(h << 16u) >> 16;
            if (type == 0x2C235F || type == 0x1A2360) value[c] = f16tof32(h);
            else if (type == 0x2C2059 || type == 0x1A205A) value[c] = float(h) / 65535.0f;
            else if (type == 0x2C2259) value[c] = float(h);
            else if (type == 0x2C2359) value[c] = float(signedH);
            else value[c] = max(float(signedH) / 32767.0f, -1.0f);
        }
        else if (type == 0x2C82A1 || type == 0x2A2187 || type == 0x2A2190 || type == 0x2A2390)
            value[c] = packed ? asfloat(word) : float(word);
        else if (type == 0x182886 || type == 0x1A2286 || type == 0x1A2386 || type == 0x1A2086 || type == 0x1A2186)
        {
            value = float4(word & 255u, (word >> 8u) & 255u, (word >> 16u) & 255u, word >> 24u);
            if (type == 0x182886) value = value.zyxw;
            if (type != 0x1A2286 && type != 0x1A2386) value /= 255.0f;
        }
        else value[c] = asfloat(word);
    }
    return value;
}
#endif

#ifdef REBLUE_RECOMP
// DEC3N normal decode; IA binds as R32_UINT so lane .x carries the raw bits (asuint recovers them).

float4 tfetchR11G11B10(float4 value)
{
#ifdef CONAN_RECOMP
    if (g_SpecConstants() & (1u << 6))
    {
        uint v = asuint(value.x);
        int3 n = int3(int(v << 22) >> 22,
                      int(v << 12) >> 22,
                      int(v << 2) >> 22);
        return float4(max(float3(n) / 511.0f, -1.0f), 1.0f);
    }
#endif
    if (g_SpecConstants() & SPEC_CONSTANT_R11G11B10_NORMAL)
    {
        uint v = asuint(value.x);
        return float4(
            (v & 0x00000400 ? -1.0 : 0.0) + ((v & 0x3FF) / 1024.0),
            (v & 0x00200000 ? -1.0 : 0.0) + (((v >> 11) & 0x3FF) / 1024.0),
            (v & 0x80000000 ? -1.0 : 0.0) + (((v >> 22) & 0x1FF) / 512.0),
            0.0);
    }
    return value;
}

// Undo the engine bswap32 16-bit-pair swap (.yxwz) for any 16-bit-packed semantic flagged in the mask.
float4 swapFloats(uint swappedMask, float4 value, uint semanticIndex)
{
    return (swappedMask & (1u << semanticIndex)) != 0 ? value.yxwz : value;
}

// Recover X360 integer-cast-to-float TEXCOORDs from R16G16(B16A16)_UINT bindings (sign-extend low 16 bits).
float4 sintTexcoord(uint mask, float4 value, uint semanticIndex)
{
    if ((mask & (1u << semanticIndex)) != 0)
    {
        int4 si = (int4(asuint(value)) << 16) >> 16;
        return float4(si);
    }
    return value;
}
#else
float4 tfetchR11G11B10(uint4 value)
{
#ifdef CONAN_RECOMP
    if (g_SpecConstants() & (1u << 6))
    {
        uint v = value.x;
        int3 n = int3(int(v << 22) >> 22,
                      int(v << 12) >> 22,
                      int(v << 2) >> 22);
        return float4(max(float3(n) / 511.0f, -1.0f), 1.0f);
    }
#endif
    if (g_SpecConstants() & SPEC_CONSTANT_R11G11B10_NORMAL)
    {
        return float4(
            (value.x & 0x00000400 ? -1.0 : 0.0) + ((value.x & 0x3FF) / 1024.0),
            (value.x & 0x00200000 ? -1.0 : 0.0) + (((value.x >> 11) & 0x3FF) / 1024.0),
            (value.x & 0x80000000 ? -1.0 : 0.0) + (((value.x >> 22) & 0x1FF) / 512.0),
            0.0);
    }
    else
    {
        return asfloat(value);
    }
}

float4 tfetchTexcoord(uint swappedTexcoords, float4 value, uint semanticIndex)
{
#ifdef SR_VULKAN_BUFFERS
    return semanticIndex < 32 && (swappedTexcoords & (1u << (semanticIndex & 31u))) != 0 ? value.yxwz : value;
#else
    return (swappedTexcoords & (1ull << semanticIndex)) != 0 ? value.yxwz : value;
#endif
}
#endif

float4 cube(float4 value, inout CubeMapData cubeMapData)
{
    uint index = cubeMapData.cubeMapIndex;
    cubeMapData.cubeMapDirections[index] = value.xyz;
    ++cubeMapData.cubeMapIndex;
    
    return float4(0.0, 0.0, 0.0, index);
}

float4 dst(float4 src0, float4 src1)
{
    float4 dest;
    dest.x = 1.0;
    dest.y = src0.y * src1.y;
    dest.z = src0.z;
    dest.w = src1.w;
    return dest;
}

float4 max4(float4 src0)
{
    return max(max(src0.x, src0.y), max(src0.z, src0.w));
}

float2 getPixelCoord(uint resourceDescriptorIndex, float2 texCoord)
{
    return getTexture2DDimensions(g_Texture2DDescriptorHeap[TEX_INDEX(resourceDescriptorIndex)]) * texCoord;
}

float computeMipLevel(float2 pixelCoord)
{
    float2 dx = ddx(pixelCoord);
    float2 dy = ddy(pixelCoord);
    float deltaMaxSqr = max(dot(dx, dx), dot(dy, dy));
    return max(0.0, 0.5 * log2(deltaMaxSqr));
}

#endif

#endif
