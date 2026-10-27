#include "check.h"
#include <filesystem>
#include <fstream>
#include <gpu/shader_compiler.h>

// Smallest valid pixel shader: ExecEnd at CF address 1 with one ALU (r1 = r0 + r0); `salt`
// lands in an unused src register byte so each test gets a distinct hash.
static ShaderBlob TinyShader(ShaderType type, uint32_t salt)
{
    ShaderBlob b;
    b.type = type;
    b.ucode = { 1u | (1u << 12), 2u << 12, 0, 1u | (0xFu << 16), 0, salt & 0xFF };
    // Same seeding as ShaderCacheInsert: type is part of the hash.
    uint64_t h = 1469598103934665603ull ^ uint64_t(type);
    for (uint32_t d : b.ucode)
        for (int i = 3; i >= 0; i--)
            h = (h ^ ((d >> (i * 8)) & 0xFF)) * 1099511628211ull;
    b.hash = h;
    return b;
}

static std::filesystem::path TempCache(const char* name)
{
    auto dir = std::filesystem::temp_directory_path() / "scda_shader_tests" / name;
    std::filesystem::remove_all(dir);
    return dir;
}

TEST(compile_once_then_from_memory)
{
    auto dir = TempCache("memory");
    ShaderBlob b = TinyShader(ShaderType::Pixel, 0x11);
    uint32_t before = ShaderCompilerCompileCount();
    const CompiledShader& a = GetCompiledShader(b, dir);
    const CompiledShader& c = GetCompiledShader(b, dir);
    CHECK(a.ok);
    CHECK(!a.bytecode.empty());
    CHECK(&a == &c);
    CHECK_EQ(ShaderCompilerCompileCount(), before + 1);
}

TEST(disk_cache_reused_across_instances)
{
    auto dir = TempCache("disk");
    ShaderBlob b = TinyShader(ShaderType::Pixel, 0x22);
    std::vector<uint8_t> first = GetCompiledShader(b, dir).bytecode;
    ShaderCompilerResetMemory(); // simulate a new process
    uint32_t before = ShaderCompilerCompileCount();
    const CompiledShader& again = GetCompiledShader(b, dir);
    CHECK(again.ok);
    CHECK(again.bytecode == first);
    CHECK_EQ(ShaderCompilerCompileCount(), before); // loaded, not recompiled
}

// Review Focus: a truncated/corrupt cache file is recompiled, not trusted.
TEST(corrupt_cache_file_recompiled)
{
    auto dir = TempCache("corrupt");
    ShaderBlob b = TinyShader(ShaderType::Pixel, 0x33);
    std::vector<uint8_t> good = GetCompiledShader(b, dir).bytecode;
    ShaderCompilerResetMemory();
    for (auto& e : std::filesystem::directory_iterator(dir))
    {
        std::vector<uint8_t> bytes(std::filesystem::file_size(e.path()));
        std::ifstream(e.path(), std::ios::binary).read((char*)bytes.data(), bytes.size());
        bytes.resize(bytes.size() / 2);
        bytes.back() ^= 0xFF;
        std::ofstream(e.path(), std::ios::binary | std::ios::trunc).write((const char*)bytes.data(), bytes.size());
    }
    uint32_t before = ShaderCompilerCompileCount();
    const CompiledShader& again = GetCompiledShader(b, dir);
    CHECK(again.ok);
    CHECK(again.bytecode == good);
    CHECK_EQ(ShaderCompilerCompileCount(), before + 1);
}

// Review Focus: identical microcode used as VS and PS gives two distinct compiled shaders.
TEST(vs_and_ps_same_ucode_distinct)
{
    auto dir = TempCache("vsps");
    ShaderBlob vs = TinyShader(ShaderType::Vertex, 0x44), ps = TinyShader(ShaderType::Pixel, 0x44);
    CHECK(vs.hash != ps.hash);
    const CompiledShader& a = GetCompiledShader(vs, dir);
    const CompiledShader& c = GetCompiledShader(ps, dir);
    CHECK(a.ok);
    CHECK(c.ok);
    CHECK(&a != &c);
    CHECK(a.bytecode != c.bytecode);
}

// A shader that fails translation is cached as failed (no retry every draw).
TEST(failed_shader_cached_as_failed)
{
    auto dir = TempCache("failed");
    ShaderBlob b;
    b.type = ShaderType::Pixel;
    b.hash = 0x5CDA0000F00Dull;
    b.ucode = { 0, 0 };
    uint32_t before = ShaderCompilerCompileCount();
    const CompiledShader& a = GetCompiledShader(b, dir);
    const CompiledShader& c = GetCompiledShader(b, dir);
    CHECK(!a.ok);
    CHECK(&a == &c);
    CHECK_EQ(ShaderCompilerCompileCount(), before + 1);
}
