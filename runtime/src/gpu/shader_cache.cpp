#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <memory>
#include <mutex>
#include <unordered_map>
#include <gpu/shader_cache.h>

static std::mutex g_shaderMutex;
static std::unordered_map<uint64_t, std::unique_ptr<ShaderBlob>> g_shaders;
static const ShaderBlob* g_current[2];

static uint64_t Fnv1a(const uint8_t* p, size_t n, uint64_t seed)
{
    uint64_t h = 1469598103934665603ull ^ seed;
    for (size_t i = 0; i < n; i++)
        h = (h ^ p[i]) * 1099511628211ull;
    return h;
}

const ShaderBlob* ShaderCacheInsert(ShaderType type, const uint32_t* be, uint32_t dwords)
{
    if (!dwords || !be)
        return nullptr;
    uint64_t hash = Fnv1a((const uint8_t*)be, dwords * 4ull, uint64_t(type));
    std::lock_guard lock(g_shaderMutex);
    auto it = g_shaders.find(hash);
    if (it != g_shaders.end())
        return it->second.get();
    auto blob = std::make_unique<ShaderBlob>();
    blob->type = type;
    blob->hash = hash;
    blob->ucode.resize(dwords);
    for (uint32_t i = 0; i < dwords; i++)
        blob->ucode[i] = _byteswap_ulong(be[i]);
    // SCDA_DUMP_SHADERS=<dir>: one file per new blob (big-endian, as uploaded).
    if (const char* dir = getenv("SCDA_DUMP_SHADERS"))
    {
        std::filesystem::create_directories(dir);
        char name[64];
        snprintf(name, sizeof(name), "%016llx.%s.bin", (unsigned long long)hash, type == ShaderType::Vertex ? "vs" : "ps");
        std::ofstream(std::filesystem::path(dir) / name, std::ios::binary).write((const char*)be, dwords * 4ull);
    }
    const ShaderBlob* out = blob.get();
    g_shaders.emplace(hash, std::move(blob));
    return out;
}

const ShaderBlob* ShaderCacheFind(uint64_t hash)
{
    std::lock_guard lock(g_shaderMutex);
    auto it = g_shaders.find(hash);
    return it == g_shaders.end() ? nullptr : it->second.get();
}

size_t ShaderCacheSize()
{
    std::lock_guard lock(g_shaderMutex);
    return g_shaders.size();
}

const ShaderBlob* CurrentShader(ShaderType type) { return g_current[int(type) & 1]; }
void SetCurrentShader(const ShaderBlob* blob)
{
    if (blob)
        g_current[int(blob->type) & 1] = blob;
}
