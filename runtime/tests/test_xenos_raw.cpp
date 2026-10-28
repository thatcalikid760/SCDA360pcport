#include "check.h"
#include <filesystem>
#include <fstream>
#include <gpu/xenos/translate.h>

static ShaderBlob LoadBlob(const std::filesystem::path& p)
{
    std::vector<uint8_t> bytes(std::filesystem::file_size(p));
    std::ifstream(p, std::ios::binary).read((char*)bytes.data(), bytes.size());
    ShaderBlob b;
    b.type = p.stem().extension() == ".ps" ? ShaderType::Pixel : ShaderType::Vertex;
    b.hash = 0;
    for (size_t i = 0; i + 4 <= bytes.size(); i += 4)
        b.ucode.push_back((bytes[i] << 24) | (bytes[i + 1] << 16) | (bytes[i + 2] << 8) | bytes[i + 3]);
    return b;
}

// Every microcode blob the menu uploads (SCDA_DUMP_SHADERS output) translates and compiles.
TEST(raw_translates_and_compiles_menu_shaders)
{
    std::filesystem::path dir = SCDA_ROOT "/_run/shaders";
    if (!std::filesystem::exists(dir))
    {
        printf("  (skipped: no dumped shaders)\n");
        return;
    }
    // 01_Intro blobs (_run/shaders_intro) too, when dumped.
    std::vector<std::filesystem::directory_entry> files(std::filesystem::directory_iterator(dir), {});
    if (std::filesystem::path intro = SCDA_ROOT "/_run/shaders_intro"; std::filesystem::exists(intro))
        files.insert(files.end(), std::filesystem::directory_iterator(intro), std::filesystem::directory_iterator());
    int n = 0;
    for (auto& e : files)
    {
        if (e.path().extension() != ".bin")
            continue;
        ShaderBlob b = LoadBlob(e.path());
        TranslateResult t = TranslateShader(b);
        if (!t.ok)
            printf("  %s: translate: %s\n", e.path().filename().string().c_str(), t.error.c_str());
        CHECK(t.ok);
        std::string err;
        auto code = CompileHlsl(t.hlsl, b.type == ShaderType::Pixel, &err);
        if (code.empty())
            printf("  %s: compile: %s\n----\n%s\n----\n", e.path().filename().string().c_str(), err.c_str(), t.hlsl.c_str());
        CHECK(!code.empty());
        n++;
    }
    CHECK(n > 0);
}

// Review Focus: microcode too short for its control flow fails cleanly.
TEST(raw_rejects_truncated_microcode)
{
    ShaderBlob b;
    b.type = ShaderType::Pixel;
    b.hash = 0;
    b.ucode = { 0x00000000, 0x00000000 };
    TranslateResult t = TranslateShader(b);
    CHECK(!t.ok);
    CHECK(!t.error.empty());
}

// Exec at CF address `addr` running one ALU instruction with the given vector opcode (r1.xyzw).
static ShaderBlob AluShader(uint32_t addr, uint32_t vectorOpcode)
{
    ShaderBlob b;
    b.type = ShaderType::Pixel;
    b.hash = 0;
    // CF 0: ExecEnd address=addr count=1 sequence=0 (ALU); CF 1: Nop.
    b.ucode = { addr | (1u << 12), 2u << 12, 0,
                // ALU: vectorDest=1, vectorWriteMask=0xF; src swizzles identity; src regs 0; opcode.
                1u | (0xFu << 16), 0, vectorOpcode << 24 };
    return b;
}

// Review Focus: an unused ALU opcode emits a safe default, and the shader still compiles.
TEST(raw_unknown_opcode_still_compiles)
{
    TranslateResult t = TranslateShader(AluShader(1, 30));
    CHECK(t.ok);
    std::string err;
    auto code = CompileHlsl(t.hlsl, true, &err);
    if (code.empty())
        printf("  compile: %s\n", err.c_str());
    CHECK(!code.empty());
}

// A bool-conditional exec clause (01_Intro particle VS: cond_exec_pred_clean on b0) runs only when
// the bool matches its condition; translated unconditionally, both b0 paths clobbered each other.
TEST(raw_cond_exec_tests_bool_constant)
{
    ShaderBlob b = AluShader(1, 0);
    // CF 0: CondExecPredCleanEnd (14), bool 5, condition 1.
    b.ucode[1] = (5u << 2) | (1u << 10) | (14u << 12);
    TranslateResult t = TranslateShader(b);
    CHECK(t.ok);
    CHECK(t.hlsl.find("if (b5 != 0)") != std::string::npos);
    b.ucode[1] &= ~(1u << 10); // condition 0
    t = TranslateShader(b);
    CHECK(t.hlsl.find("if (b5 == 0)") != std::string::npos);
    std::string err;
    CHECK(!CompileHlsl(t.hlsl, true, &err).empty());
}

// Review Focus: a control-flow address past the end of the microcode fails cleanly.
TEST(raw_rejects_out_of_range_exec_address)
{
    TranslateResult t = TranslateShader(AluShader(4095, 0));
    CHECK(!t.ok);
    CHECK(!t.error.empty());
}

// The D3D built-in pixel shader (container at 0x82001D9C, dumped as 2008966a44d70a8a.ps.bin)
// also translates through the raw path.
TEST(raw_builtin_matches_container_semantics)
{
    std::filesystem::path p = SCDA_ROOT "/_run/shaders/2008966a44d70a8a.ps.bin";
    if (!std::filesystem::exists(p))
    {
        printf("  (skipped: no dumped built-in shader)\n");
        return;
    }
    TranslateResult t = TranslateShader(LoadBlob(p));
    CHECK(t.ok);
    std::string err;
    CHECK(!CompileHlsl(t.hlsl, true, &err).empty());
}
