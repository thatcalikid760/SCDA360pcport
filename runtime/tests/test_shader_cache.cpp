#include "check.h"
#include "guest_memory.h"
#include <command_processor.h>
#include <gpu/shader_cache.h>
#include <gpu_state.h>
#include <runtime.h>

static uint32_t BE(uint32_t v) { return _byteswap_ulong(v); }

TEST(shader_insert_dedupes_by_content)
{
    size_t before = ShaderCacheSize();
    uint32_t a[3] = { BE(1), BE(2), BE(3) }, b[3] = { BE(1), BE(2), BE(4) };
    const ShaderBlob* x = ShaderCacheInsert(ShaderType::Pixel, a, 3);
    const ShaderBlob* y = ShaderCacheInsert(ShaderType::Pixel, a, 3);
    const ShaderBlob* z = ShaderCacheInsert(ShaderType::Pixel, b, 3);
    CHECK(x == y);
    CHECK(x != z);
    CHECK_EQ(ShaderCacheSize(), before + 2);
    CHECK_EQ(x->ucode[2], 3u);
}

static const uint32_t kRing = 0xA0070000;
static uint32_t g_sw;
static void SEmit(uint32_t v) { GuestStore32(kRing + g_sw++ * 4, v); }

static void ShaderRing()
{
    TestGuestBase();
    CommandProcessorReset();
    Gpu().ringSize = 0x10000;
    Gpu().ringBase = kRing;
    Gpu().rptrWriteback = 0;
    g_sw = 0;
}

TEST(im_load_immediate_sets_current_pixel_shader)
{
    ShaderRing();
    SEmit(0xC0052B00); SEmit(1); SEmit(4); SEmit(0x11); SEmit(0x22); SEmit(0x33); SEmit(0x44); // type 1, size 4
    GuestStore32(kGpuRegBase + kRegCpRbWptr * 4, g_sw);
    CpStats st{};
    CommandProcessorTick(&st);
    const ShaderBlob* ps = CurrentShader(ShaderType::Pixel);
    CHECK(ps != nullptr);
    CHECK_EQ(ps->ucode.size(), (size_t)4);
    CHECK_EQ(ps->ucode[3], 0x44u);
}

TEST(im_load_from_memory_sets_current_vertex_shader)
{
    ShaderRing();
    for (uint32_t i = 0; i < 8; i++)
        GuestStore32(0xA0080000 + i * 4, 0x100 + i);
    SEmit(0xC0012700); SEmit(0x00080000 | 0); SEmit(8); // IM_LOAD vertex, 8 dwords
    GuestStore32(kGpuRegBase + kRegCpRbWptr * 4, g_sw);
    CpStats st{};
    CommandProcessorTick(&st);
    const ShaderBlob* vs = CurrentShader(ShaderType::Vertex);
    CHECK(vs != nullptr);
    CHECK_EQ(vs->ucode.size(), (size_t)8);
    CHECK_EQ(vs->ucode[7], 0x107u);
}

// Review Focus: size 0 is ignored.
TEST(im_load_size_zero_ignored)
{
    CHECK(ShaderCacheInsert(ShaderType::Vertex, nullptr, 0) == nullptr);
}
