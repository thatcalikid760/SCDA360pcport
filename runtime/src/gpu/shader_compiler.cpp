#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <memory>
#include <mutex>
#include <unordered_map>
#include <gpu/shader_compiler.h>
#include <gpu/xenos/translate.h>

// Bump when the translator or shader_common changes: old disk entries are then ignored.
static constexpr uint32_t kTranslatorVersion = 1;
static constexpr uint32_t kCacheMagic = 0x53444353; // "SCDS"

struct CacheHeader
{
    uint32_t magic, version, type, size;
    uint64_t hash, checksum;
};

static std::mutex g_mutex;
static std::unordered_map<uint64_t, std::unique_ptr<CompiledShader>> g_compiled;
static uint32_t g_compileCount, g_failedCount;

static uint64_t Fnv1a(const uint8_t* p, size_t n)
{
    uint64_t h = 1469598103934665603ull;
    for (size_t i = 0; i < n; i++)
        h = (h ^ p[i]) * 1099511628211ull;
    return h;
}

static std::filesystem::path CachePath(const ShaderBlob& blob, const std::filesystem::path& dir)
{
    char name[64];
    snprintf(name, sizeof(name), "%016llx.%s.v%u.dxbc", (unsigned long long)blob.hash,
        blob.type == ShaderType::Pixel ? "ps" : "vs", kTranslatorVersion);
    return dir / name;
}

// Loads a cache entry; any mismatch (size, header, checksum, DXBC magic) means "not cached".
static bool LoadCached(const ShaderBlob& blob, const std::filesystem::path& path, std::vector<uint8_t>& out)
{
    std::ifstream f(path, std::ios::binary);
    CacheHeader h{};
    if (!f || !f.read((char*)&h, sizeof(h)))
        return false;
    if (h.magic != kCacheMagic || h.version != kTranslatorVersion || h.type != uint32_t(blob.type) || h.hash != blob.hash ||
        h.size < 4 || h.size > 16u << 20)
        return false;
    out.resize(h.size);
    if (!f.read((char*)out.data(), h.size) || Fnv1a(out.data(), out.size()) != h.checksum || memcmp(out.data(), "DXBC", 4) != 0)
    {
        out.clear();
        return false;
    }
    return true;
}

static void StoreCached(const ShaderBlob& blob, const std::filesystem::path& path, const std::vector<uint8_t>& code)
{
    std::error_code ec;
    std::filesystem::create_directories(path.parent_path(), ec);
    CacheHeader h{ kCacheMagic, kTranslatorVersion, uint32_t(blob.type), uint32_t(code.size()), blob.hash, Fnv1a(code.data(), code.size()) };
    // Write to a temp file and rename, so a crash mid-write never leaves a half entry under the real name.
    std::filesystem::path tmp = path;
    tmp += ".tmp";
    {
        std::ofstream f(tmp, std::ios::binary | std::ios::trunc);
        f.write((const char*)&h, sizeof(h));
        f.write((const char*)code.data(), code.size());
        if (!f)
            return;
    }
    std::filesystem::rename(tmp, path, ec);
}

static void Compile(const ShaderBlob& blob, CompiledShader& out)
{
    g_compileCount++;
    const char* stage = blob.type == ShaderType::Pixel ? "ps" : "vs";
    TranslateResult t = TranslateShader(blob);
    if (!t.ok)
    {
        g_failedCount++;
        printf("[shader] %016llx.%s: translate failed: %s\n", (unsigned long long)blob.hash, stage, t.error.c_str());
        return;
    }
    // SCDA_DUMP_HLSL=<dir>: <hash>.vs.hlsl / <hash>.ps.hlsl for every translated shader.
    if (const char* dir = getenv("SCDA_DUMP_HLSL"))
    {
        std::error_code ec;
        std::filesystem::create_directories(dir, ec);
        char name[64];
        snprintf(name, sizeof(name), "%016llx.%s.hlsl", (unsigned long long)blob.hash, stage);
        std::ofstream(std::filesystem::path(dir) / name, std::ios::binary).write(t.hlsl.data(), t.hlsl.size());
    }
    std::string err;
    out.bytecode = CompileHlsl(t.hlsl, blob.type == ShaderType::Pixel, &err);
    out.ok = !out.bytecode.empty();
    if (!out.ok)
    {
        g_failedCount++;
        printf("[shader] %016llx.%s: compile failed: %s\n", (unsigned long long)blob.hash, stage, err.c_str());
    }
}

const CompiledShader& GetCompiledShader(const ShaderBlob& blob, const std::filesystem::path& cacheDir)
{
    std::lock_guard lock(g_mutex);
    auto it = g_compiled.find(blob.hash);
    if (it != g_compiled.end())
        return *it->second;
    auto shader = std::make_unique<CompiledShader>();
    std::filesystem::path path = CachePath(blob, cacheDir);
    if (LoadCached(blob, path, shader->bytecode))
        shader->ok = true;
    else
    {
        Compile(blob, *shader);
        if (shader->ok)
            StoreCached(blob, path, shader->bytecode);
    }
    return *g_compiled.emplace(blob.hash, std::move(shader)).first->second;
}

std::filesystem::path DefaultShaderCacheDir()
{
    if (const char* base = getenv("LOCALAPPDATA"))
        return std::filesystem::path(base) / "SCDARecomp" / "shadercache";
    return "shadercache";
}

uint32_t ShaderCompilerCompileCount()
{
    std::lock_guard lock(g_mutex);
    return g_compileCount;
}

uint32_t ShaderCompilerFailedCount()
{
    std::lock_guard lock(g_mutex);
    return g_failedCount;
}

void ShaderCompilerResetMemory()
{
    std::lock_guard lock(g_mutex);
    g_compiled.clear();
}
