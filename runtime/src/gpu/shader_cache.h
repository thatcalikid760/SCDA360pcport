// Xenos shader microcode uploaded through IM_LOAD / IM_LOAD_IMMEDIATE, deduplicated by content.
#pragma once
#include <cstddef>
#include <cstdint>
#include <vector>

enum class ShaderType : uint8_t { Vertex = 0, Pixel = 1 };

struct ShaderBlob
{
    ShaderType type;
    uint64_t hash;              // FNV-1a 64 over the big-endian microcode bytes
    std::vector<uint32_t> ucode; // host-order dwords
};

// Returns the cached blob for this microcode (inserting it); nullptr when dwords == 0.
const ShaderBlob* ShaderCacheInsert(ShaderType type, const uint32_t* bigEndianDwords, uint32_t dwords);
const ShaderBlob* ShaderCacheFind(uint64_t hash);
size_t ShaderCacheSize();

// The blob most recently loaded for each stage: what the next draw uses.
const ShaderBlob* CurrentShader(ShaderType type);
void SetCurrentShader(const ShaderBlob* blob);
