#include <windows.h>
#include <d3dcompiler.h>
#include <gpu/xenos/shader_recompiler.h>
#include <gpu/xenos/translate.h>

// SM5 replacement for XenosRecomp's bindless/Vulkan shader_common.h. Texture helpers are macros:
// SM5 only indexes resource arrays with literals, and the translator passes literal slots.
static const char kCommonSm5[] = R"HLSL(
#define FLT_MIN asfloat(0xff7fffff)
#define FLT_MAX asfloat(0x7f7fffff)
#define SPEC_CONSTANT_R11G11B10_NORMAL (1 << 0)
#define SPEC_CONSTANT_ALPHA_TEST       (1 << 1)
#define select(c, a, b) ((c) ? (a) : (b))

cbuffer VertexShaderConstants : register(b0) { float4 g_VSConstants[256]; };
cbuffer PixelShaderConstants : register(b1) { float4 g_PSConstants[256]; };
cbuffer SharedConstants : register(b2)
{
    uint4 g_BoolWords[2];    // bool constants (0x4900), 256 bits
    uint4 g_Loops[8];        // loop constants (0x4908)
    uint4 g_Fetch[48];       // vertex fetch constants (0x4800), 2 dwords per slot
    uint g_SpecConstantsValue;
    float g_AlphaThreshold;
    float2 g_HalfPixelOffset;
};
uint g_SpecConstants() { return g_SpecConstantsValue; }

Texture2D<float4> g_Tex2D[32] : register(t0);
Texture3D<float4> g_Tex3D[32] : register(t32);
TextureCube<float4> g_TexCube[32] : register(t64);
ByteAddressBuffer g_Memory : register(t96); // guest physical memory from address 0
SamplerState g_Samp[16] : register(s0);

float2 Dims2D(Texture2D<float4> t)
{
    uint w, h;
    t.GetDimensions(w, h);
    return float2(w, h);
}

#define tfetch2D(t, s, uv, off) g_Tex2D[t].Sample(g_Samp[(s) & 15], (uv) + (off) / Dims2D(g_Tex2D[t]))
#define tfetch2DBicubic(t, s, uv, off) tfetch2D(t, s, uv, off)
#define getWeights2D(t, s, uv, off) (isnan(uv) ? 0.0 : frac((uv) * Dims2D(g_Tex2D[t]) + (off) - 0.5))
#define tfetch3D(t, s, uvw) g_Tex3D[t].Sample(g_Samp[(s) & 15], uvw)
#define tfetchCube(t, s, uvw, cmd) g_TexCube[t].Sample(g_Samp[(s) & 15], (cmd).cubeMapDirections[(uvw).z])
#define getPixelCoord(t, uv) (Dims2D(g_Tex2D[t]) * (uv))

struct CubeMapData
{
    float3 cubeMapDirections[2];
    uint cubeMapIndex;
};

float4 cube(float4 value, inout CubeMapData cubeMapData)
{
    uint index = cubeMapData.cubeMapIndex;
    cubeMapData.cubeMapDirections[index] = value.xyz;
    ++cubeMapData.cubeMapIndex;
    return float4(0.0, 0.0, 0.0, index);
}

float4 dst(float4 src0, float4 src1)
{
    return float4(1.0, src0.y * src1.y, src0.z, src1.w);
}

float4 max4(float4 src0)
{
    return max(max(src0.x, src0.y), max(src0.z, src0.w));
}

float computeMipLevel(float2 pixelCoord)
{
    float2 dx = ddx(pixelCoord);
    float2 dy = ddy(pixelCoord);
    return max(0.0, 0.5 * log2(max(dot(dx, dx), dot(dy, dy))));
}

// GPU endian modes: 0 none, 1 8-in-16, 2 8-in-32, 3 16-in-32.
uint SwapDword(uint v, uint e)
{
    if (e == 1) return ((v >> 8) & 0x00FF00FF) | ((v << 8) & 0xFF00FF00);
    if (e == 2) return (v >> 24) | ((v >> 8) & 0xFF00) | ((v << 8) & 0xFF0000) | (v << 24);
    if (e == 3) return (v >> 16) | (v << 16);
    return v;
}

float Unorm(uint v, uint bits) { return float(v) / float((1u << bits) - 1u); }
float Snorm(uint v, uint bits)
{
    int s = int(v << (32 - bits)) >> (32 - bits);
    return max(float(s) / float((1u << (bits - 1)) - 1u), -1.0);
}
float Sint(uint v, uint bits) { return float(int(v << (32 - bits)) >> (32 - bits)); }
float Comp(uint v, uint bits, uint isSigned, uint normalized)
{
    if (normalized) return isSigned ? Snorm(v, bits) : Unorm(v, bits);
    return isSigned ? Sint(v, bits) : float(v);
}

// Xenos vertex fetch from guest memory. Fetch constant: dword0 = byte address | type,
// dword1 = size | endian. stride/offset are in dwords. Formats: Xenia's VertexFormat values.
float4 FetchVertex(uint slot, float index, int offset, uint stride, uint format, uint isSigned, uint normalized)
{
    uint4 g = g_Fetch[slot >> 1];
    uint2 fc = (slot & 1) ? g.zw : g.xy;
    uint endian = fc.y & 3;
    uint addr = (fc.x & 0xFFFFFFFC) + (uint(max(index, 0.0) + 0.5) * stride + uint(offset)) * 4;
    uint4 d;
    d.x = SwapDword(g_Memory.Load(addr), endian);
    d.y = SwapDword(g_Memory.Load(addr + 4), endian);
    d.z = SwapDword(g_Memory.Load(addr + 8), endian);
    d.w = SwapDword(g_Memory.Load(addr + 12), endian);
    switch (format)
    {
    case 6:  // 8_8_8_8
        return float4(Comp(d.x & 0xFF, 8, isSigned, normalized), Comp((d.x >> 8) & 0xFF, 8, isSigned, normalized),
                      Comp((d.x >> 16) & 0xFF, 8, isSigned, normalized), Comp(d.x >> 24, 8, isSigned, normalized));
    case 7:  // 2_10_10_10
        return float4(Comp(d.x & 0x3FF, 10, isSigned, normalized), Comp((d.x >> 10) & 0x3FF, 10, isSigned, normalized),
                      Comp((d.x >> 20) & 0x3FF, 10, isSigned, normalized), Comp(d.x >> 30, 2, isSigned, normalized));
    case 16: // 10_11_11
        return float4(Comp(d.x & 0x3FF, 10, isSigned, normalized), Comp((d.x >> 10) & 0x7FF, 11, isSigned, normalized),
                      Comp(d.x >> 21, 11, isSigned, normalized), 1.0);
    case 17: // 11_11_10
        return float4(Comp(d.x & 0x7FF, 11, isSigned, normalized), Comp((d.x >> 11) & 0x7FF, 11, isSigned, normalized),
                      Comp(d.x >> 22, 10, isSigned, normalized), 1.0);
    case 25: // 16_16
        return float4(Comp(d.x & 0xFFFF, 16, isSigned, normalized), Comp(d.x >> 16, 16, isSigned, normalized), 0.0, 1.0);
    case 26: // 16_16_16_16
        return float4(Comp(d.x & 0xFFFF, 16, isSigned, normalized), Comp(d.x >> 16, 16, isSigned, normalized),
                      Comp(d.y & 0xFFFF, 16, isSigned, normalized), Comp(d.y >> 16, 16, isSigned, normalized));
    case 31: // 16_16_FLOAT
        return float4(f16tof32(d.x), f16tof32(d.x >> 16), 0.0, 1.0);
    case 32: // 16_16_16_16_FLOAT
        return float4(f16tof32(d.x), f16tof32(d.x >> 16), f16tof32(d.y), f16tof32(d.y >> 16));
    case 33: // 32
        return float4(Comp(d.x, 32, isSigned, 0), 0.0, 0.0, 1.0);
    case 34: // 32_32
        return float4(Comp(d.x, 32, isSigned, 0), Comp(d.y, 32, isSigned, 0), 0.0, 1.0);
    case 35: // 32_32_32_32
        return float4(Comp(d.x, 32, isSigned, 0), Comp(d.y, 32, isSigned, 0), Comp(d.z, 32, isSigned, 0), Comp(d.w, 32, isSigned, 0));
    case 36: // 32_FLOAT
        return float4(asfloat(d.x), 0.0, 0.0, 1.0);
    case 37: // 32_32_FLOAT
        return float4(asfloat(d.x), asfloat(d.y), 0.0, 1.0);
    case 57: // 32_32_32_FLOAT
        return float4(asfloat(d.x), asfloat(d.y), asfloat(d.z), 1.0);
    case 38: // 32_32_32_32_FLOAT
    default:
        return asfloat(d);
    }
}
)HLSL";

static std::string BuildInclude()
{
    std::string s = kCommonSm5;
    char line[512];
    // Bool constants b0..b255 and literal texture/sampler slots used by the translator.
    for (int n = 0; n < 256; n++)
    {
        snprintf(line, sizeof(line), "#define b%d ((g_BoolWords[%d][%d] >> %d) & 1)\n", n, n >> 7, (n >> 5) & 3, n & 31);
        s += line;
    }
    for (int n = 0; n < 32; n++)
    {
        snprintf(line, sizeof(line),
            "#define s%d_Texture2DDescriptorIndex %d\n#define s%d_Texture3DDescriptorIndex %d\n"
            "#define s%d_TextureCubeDescriptorIndex %d\n#define s%d_SamplerDescriptorIndex %d\n",
            n, n, n, n, n, n, n, n);
        s += line;
    }
    return s;
}

TranslateResult TranslateShader(const ShaderBlob& blob)
{
    TranslateResult r;
    size_t dwords = blob.ucode.size();
    // Control flow comes in 3-dword pairs; anything shorter or ragged is not valid microcode.
    if (dwords < 3 || dwords % 3)
    {
        r.error = "microcode size " + std::to_string(dwords) + " dwords is not a whole number of 3-dword groups";
        return r;
    }
    // Big-endian copy. recompileBody rejects exec addresses past the end; the CF walk stays inside.
    std::vector<be<uint32_t>> code(dwords);
    for (size_t i = 0; i < dwords; i++)
        code[i].value = byteSwap(blob.ucode[i]);
    static const std::string include = BuildInclude();
    ShaderRecompiler rec;
    rec.recompileRaw(code.data(), uint32_t(dwords * 4), blob.type == ShaderType::Pixel, include);
    if (!rec.error.empty())
    {
        r.error = rec.error;
        return r;
    }
    r.hlsl = std::move(rec.out);
    r.ok = true;
    return r;
}

std::vector<uint8_t> CompileHlsl(const std::string& hlsl, bool pixel, std::string* error)
{
    ID3DBlob* code = nullptr;
    ID3DBlob* errors = nullptr;
    HRESULT hr = D3DCompile(hlsl.data(), hlsl.size(), pixel ? "ps.hlsl" : "vs.hlsl", nullptr, nullptr, "main",
        pixel ? "ps_5_0" : "vs_5_0", D3DCOMPILE_OPTIMIZATION_LEVEL1, 0, &code, &errors);
    std::vector<uint8_t> out;
    if (SUCCEEDED(hr) && code)
        out.assign((uint8_t*)code->GetBufferPointer(), (uint8_t*)code->GetBufferPointer() + code->GetBufferSize());
    if (error)
        *error = errors ? std::string((const char*)errors->GetBufferPointer(), errors->GetBufferSize()) : "";
    if (code)
        code->Release();
    if (errors)
        errors->Release();
    return out;
}
