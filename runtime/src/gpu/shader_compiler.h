// Translated + compiled shaders (Xenos microcode -> SM5 bytecode), cached in memory and on disk.
#pragma once
#include <bitset>
#include <cstdint>
#include <filesystem>
#include <vector>
#include <gpu/shader_cache.h>

struct CompiledShader
{
    std::vector<uint8_t> bytecode;
    bool ok = false;
};

// Translate + compile once per blob; memory cache, disk cache in cacheDir (tests pass a temp dir).
// Failures are cached too (logged once). Returned references stay valid until ShaderCompilerResetMemory.
const CompiledShader& GetCompiledShader(const ShaderBlob& blob, const std::filesystem::path& cacheDir);

// Vertex fetch constant slots the vertex shader reads (translated once per blob, cached).
std::bitset<96> ShaderVertexFetchSlots(const ShaderBlob& blob);
// Texture fetch constant slots (bit n = slot n) the shader samples (translated once per blob, cached).
uint32_t ShaderTextureFetchSlots(const ShaderBlob& blob);

// %LOCALAPPDATA%\SCDARecomp\shadercache (falls back to .\shadercache).
std::filesystem::path DefaultShaderCacheDir();

// Translate+compile attempts so far (disk hits not counted).
uint32_t ShaderCompilerCompileCount();
uint32_t ShaderCompilerFailedCount();
// Shaders ready to bind (compiled or loaded from disk).
uint32_t ShaderCompilerReadyCount();
// Drops the memory cache (tests: simulate a new process).
void ShaderCompilerResetMemory();
