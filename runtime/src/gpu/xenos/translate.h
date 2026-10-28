// Raw Xenos microcode -> Shader Model 5 HLSL (XenosRecomp fork, raw mode) and D3DCompile.
#pragma once
#include <cstdint>
#include <string>
#include <vector>
#include <gpu/shader_cache.h>

struct TranslateResult
{
    bool ok = false;
    std::string hlsl;
    std::string error;
};

// Resource bindings the translated shaders expect (the renderer binds to these).
constexpr uint32_t kHlslTex2DSlot = 0, kHlslTex3DSlot = 32, kHlslTexCubeSlot = 64, kHlslMemorySlot = 96;
constexpr uint32_t kHlslVSConstantsSlot = 0, kHlslPSConstantsSlot = 1, kHlslSharedSlot = 2;

TranslateResult TranslateShader(const ShaderBlob& blob);

// D3DCompile for vs_5_0 / ps_5_0; empty on failure with the compiler message in *error.
std::vector<uint8_t> CompileHlsl(const std::string& hlsl, bool pixel, std::string* error);
// Any profile ("gs_5_0", ...); SCDA_VERTEX is defined for vs_* only.
std::vector<uint8_t> CompileHlsl(const std::string& hlsl, const char* profile, std::string* error);
