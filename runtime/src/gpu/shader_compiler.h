// Translated + compiled shaders (Xenos microcode -> SM5 bytecode), cached in memory and on disk.
#pragma once
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

// %LOCALAPPDATA%\SCDARecomp\shadercache (falls back to .\shadercache).
std::filesystem::path DefaultShaderCacheDir();

// Translate+compile attempts so far (disk hits not counted).
uint32_t ShaderCompilerCompileCount();
uint32_t ShaderCompilerFailedCount();
// Shaders ready to bind (compiled or loaded from disk).
uint32_t ShaderCompilerReadyCount();
// Drops the memory cache (tests: simulate a new process).
void ShaderCompilerResetMemory();
