// SCDA Recomp addition to the XenosRecomp fork (MIT): translation of raw Xenos microcode, as
// uploaded by IM_LOAD, without the D3D shader container. Metadata comes from the GPU registers
// at draw time instead: constants are the register file, vertex data is fetched from guest
// memory, interpolators are passed by register number (Xenos convention).
#include "shader_recompiler.h"
#include "shader_common.h"

static constexpr uint32_t kTempRegisters = 128;
static constexpr uint32_t kInterpolators = 16;
static constexpr uint32_t kSamplers = 32;

void ShaderRecompiler::recompileRaw(const be<uint32_t>* code, uint32_t shaderSize, bool pixel, const std::string_view& include)
{
    raw = true;
    isPixelShader = pixel;

    static const std::string kSamplerNames[kSamplers] = {
        "s0", "s1", "s2", "s3", "s4", "s5", "s6", "s7", "s8", "s9", "s10", "s11", "s12", "s13", "s14", "s15",
        "s16", "s17", "s18", "s19", "s20", "s21", "s22", "s23", "s24", "s25", "s26", "s27", "s28", "s29", "s30", "s31",
    };
    for (uint32_t i = 0; i < kSamplers; i++)
        samplers.emplace(i, kSamplerNames[i].c_str());

    out += include;
    out += '\n';

    if (!pixel)
    {
        for (uint32_t i = 0; i < kInterpolators; i++)
            interpolators.emplace(i, fmt::format("oT[{}]", i));
        interpolators.emplace(uint32_t(ExportRegister::VSPointSizeEdgeFlagKillVertex), "oPts");
        for (uint32_t i = 32; i <= 36; i++)
            interpolators.emplace(i, fmt::format("oMemExport{}", i));

        out += "void main(in uint iVertexId : SV_VertexID,\n";
        out += "\tout float4 oPos : SV_Position,\n";
        println("\tout float4 oT[{}] : TEXCOORD0)", kInterpolators);
        out += "{\n";
        out += "\toPos = 0.0;\n";
        println("\t[unroll] for (uint t = 0; t < {}; t++) oT[t] = 0.0;", kInterpolators);
        out += "\tfloat4 oPts = 0.0;\n";
        for (uint32_t i = 32; i <= 36; i++)
            println("\tfloat4 oMemExport{} = 0.0;", i);
        // Xenos: r0.x holds the vertex index on entry.
        out += "\tfloat4 r0 = float4(float(iVertexId), 0.0, 0.0, 0.0);\n";
    }
    else
    {
        out += "void main(in float4 iPos : SV_Position,\n";
        println("\tin float4 iT[{}] : TEXCOORD0,", kInterpolators);
        out += "\tin bool iFace : SV_IsFrontFace,\n";
        out += "\tout float4 oC0 : SV_Target0,\n";
        out += "\tout float4 oC1 : SV_Target1,\n";
        out += "\tout float4 oC2 : SV_Target2,\n";
        out += "\tout float4 oC3 : SV_Target3)\n";
        out += "{\n";
        out += "\toC0 = 0.0; oC1 = 0.0; oC2 = 0.0; oC3 = 0.0;\n";
        out += "\tfloat oDepth = 0.0;\n";
        // Interpolators arrive in r0..r15.
        for (uint32_t i = 0; i < kInterpolators; i++)
            println("\tfloat4 r{} = iT[{}];", i, i);
    }

    for (uint32_t i = pixel ? kInterpolators : 1; i < kTempRegisters; i++)
        println("\tfloat4 r{} = 0.0;", i);

    out += "\tint a0 = 0;\n";
    out += "\tint aL = 0;\n";
    out += "\tbool p0 = false;\n";
    out += "\tfloat ps = 0.0;\n";
    if (pixel)
        out += "\tCubeMapData cubeMapData = (CubeMapData)0;\n";

    recompileBody(code, shaderSize);
}
