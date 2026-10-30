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
    uint g_Vte;              // PA_CL_VTE_CNTL
    float3 g_VpScale;        // PA_CL_VPORT_X/Y/ZSCALE
    float4 g_VpOffset;       // PA_CL_VPORT_X/Y/ZOFFSET, w unused
    float4 g_WindowToNdc;    // x, y: 2 / render target size; z, w: window offset in pixels
    uint g_ColorClamp;       // bit n: oCn goes to a 7e3 target (clamped like the EDRAM store)
    uint g_AlphaFunc;        // RB_COLORCONTROL bits 0-2 (never, <, ==, <=, >, !=, >=, always)
    float g_PositionScale;   // 1 / internal resolution scale: SV_Position -> guest pixels
    uint g_Tex3DIs2D;        // bit n: fetch constant n is not a 3D texture (tfetch3D samples g_Tex2D)
};
uint g_SpecConstants() { return g_SpecConstantsValue; }
// Alpha test against RB_ALPHA_REF: < 0 = kill (fed to clip()).
float XenosAlphaTest(float a)
{
    float r = g_AlphaThreshold;
    bool ok;
    switch (g_AlphaFunc)
    {
    case 0: ok = false; break;
    case 1: ok = a < r; break;
    case 2: ok = a == r; break;
    case 3: ok = a <= r; break;
    case 4: ok = a > r; break;
    case 5: ok = a != r; break;
    case 6: ok = a >= r; break;
    default: ok = true; break;
    }
    return ok ? 1.0 : -1.0;
}

// Xenos viewport transform (PA_CL_VTE_CNTL: bits 0-5 scale/offset enables, 8 xy not divided
// by w, 9 z not divided, 10 exported w is W itself; clear = it already holds 1/W), then window
// pixels -> host clip space (y down).
float4 XenosViewport(float4 p)
{
    uint vte = g_Vte;
    float invW = (vte & 0x400) ? (p.w != 0.0 ? 1.0 / p.w : 1.0) : p.w;
    float w = invW != 0.0 ? 1.0 / invW : 1.0;
    float3 s = p.xyz;
    if (!(vte & 0x100)) s.xy *= invW;
    if (!(vte & 0x200)) s.z *= invW;
    if (vte & 0x01) s.x *= g_VpScale.x;
    if (vte & 0x02) s.x += g_VpOffset.x;
    if (vte & 0x04) s.y *= g_VpScale.y;
    if (vte & 0x08) s.y += g_VpOffset.y;
    if (vte & 0x10) s.z *= g_VpScale.z;
    if (vte & 0x20) s.z += g_VpOffset.z;
    // D3D9 half-pixel offset at internal scale > 1: exact (0.5 guest) except on integer edges,
    // which take 0.5 host pixel so integer-edged rects (the 1x1..16x16 luminance chain) cover
    // exactly the host pixels of the guest pixels they cover at scale 1. Half-integer edges (D3D9
    // quads, e.g. -0.5..255.5 per predicated tile) then keep their last host row/column.
    float2 onEdge = step(abs(s.xy - round(s.xy)), 1e-3);
    s.xy += g_HalfPixelOffset * lerp(1.0, g_PositionScale, onEdge) + g_WindowToNdc.zw;
    return float4((s.x * g_WindowToNdc.x - 1.0) * w, (1.0 - s.y * g_WindowToNdc.y) * w, s.z * w, w);
}

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

// Vertex shaders have no derivatives: sample mip 0 (SCDA_VERTEX is defined by CompileHlsl).
#ifdef SCDA_VERTEX
#define SAMPLE(tex, s, c) tex.SampleLevel(s, c, 0)
#else
#define SAMPLE(tex, s, c) tex.Sample(s, c)
#endif
#define tfetch2D(t, s, uv, off) SAMPLE(g_Tex2D[t], g_Samp[(s) & 15], (uv) + (off) / Dims2D(g_Tex2D[t]))
#define tfetch2DBicubic(t, s, uv, off) tfetch2D(t, s, uv, off)
#define getWeights2D(t, s, uv, off) (isnan(uv) ? 0.0 : frac((uv) * Dims2D(g_Tex2D[t]) + (off) - 0.5))
// A 3D fetch of a 2D texture reads slice 0: the 2D binding (g_Tex3D itself is never bound).
#define tfetch3D(t, s, uvw) ((g_Tex3DIs2D >> (t)) & 1 ? SAMPLE(g_Tex2D[t], g_Samp[(s) & 15], (uvw).xy) : SAMPLE(g_Tex3D[t], g_Samp[(s) & 15], uvw))
#define tfetchCube(t, s, uvw, cmd) SAMPLE(g_TexCube[t], g_Samp[(s) & 15], (uvw).z ? (cmd).cubeMapDirections[1] : (cmd).cubeMapDirections[0])
#define getPixelCoord(t, uv) (Dims2D(g_Tex2D[t]) * (uv))

struct CubeMapData
{
    float3 cubeMapDirections[2];
    uint cubeMapIndex;
};

float4 cube(float4 value, inout CubeMapData cubeMapData)
{
    uint index = cubeMapData.cubeMapIndex;
    // fxc cannot write a local array at a dynamic index inside branches (X3500).
    if (index == 0)
        cubeMapData.cubeMapDirections[0] = value.xyz;
    else
        cubeMapData.cubeMapDirections[1] = value.xyz;
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
// dword1 = size | endian; addresses are physical (29 bits). stride/offset are in dwords. Formats: Xenia's VertexFormat values.
float4 FetchVertex(uint slot, float index, int offset, uint stride, uint format, uint isSigned, uint normalized)
{
    uint4 g = g_Fetch[slot >> 1];
    uint2 fc = (slot & 1) ? g.zw : g.xy;
    uint endian = fc.y & 3;
    uint addr = (fc.x & 0x1FFFFFFC) + (uint(max(index, 0.0) + 0.5) * stride + uint(offset)) * 4;
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
    if (blob.type == ShaderType::Pixel)
    {
        // Wrap main: clamp outputs bound to 7e3 targets (the host target is FP16).
        size_t at = r.hlsl.find("void main(");
        if (at != std::string::npos)
        {
            r.hlsl.replace(at, 10, "void xenos_main(");
            r.hlsl += R"HLSL(
float4 Clamp7e3(float4 c, uint n) { return (g_ColorClamp >> n) & 1 ? float4(clamp(c.rgb, 0.0, 31.875), saturate(c.a)) : c; }
void main(in float4 iPos : SV_Position, in float4 iT[16] : TEXCOORD0, in bool iFace : SV_IsFrontFace,
    out float4 oC0 : SV_Target0, out float4 oC1 : SV_Target1, out float4 oC2 : SV_Target2, out float4 oC3 : SV_Target3)
{
    xenos_main(float4(iPos.xy * g_PositionScale, iPos.zw), iT, iFace, oC0, oC1, oC2, oC3);
    oC0 = Clamp7e3(oC0, 0); oC1 = Clamp7e3(oC1, 1); oC2 = Clamp7e3(oC2, 2); oC3 = Clamp7e3(oC3, 3);
}
)HLSL";
        }
    }
    r.ok = true;
    return r;
}

std::vector<uint8_t> CompileHlsl(const std::string& hlsl, bool pixel, std::string* error)
{
    return CompileHlsl(hlsl, pixel ? "ps_5_0" : "vs_5_0", error);
}

std::vector<uint8_t> CompileHlsl(const std::string& hlsl, const char* profile, std::string* error)
{
    ID3DBlob* code = nullptr;
    ID3DBlob* errors = nullptr;
    static const D3D_SHADER_MACRO kVertexDefines[] = { { "SCDA_VERTEX", "1" }, { nullptr, nullptr } };
    const bool vertex = profile[0] == 'v';
    HRESULT hr = D3DCompile(hlsl.data(), hlsl.size(), "shader.hlsl", vertex ? kVertexDefines : nullptr, nullptr, "main",
        profile, D3DCOMPILE_OPTIMIZATION_LEVEL1, 0, &code, &errors);
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
