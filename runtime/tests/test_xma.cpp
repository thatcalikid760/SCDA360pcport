#include "check.h"
#include "guest_memory.h"
#include <runtime.h>

static uint32_t XmaCall(void (*fn)(PPCContext&, uint8_t*), uint32_t a, uint32_t b = 0)
{
    PPCContext ctx{};
    ctx.r3.u64 = a;
    ctx.r4.u64 = b;
    fn(ctx, g_base);
    return ctx.r3.u32;
}

TEST(xma_context_create_init_and_buffer_state)
{
    TestGuestBase();
    const uint32_t out = 0x40700000, init = 0x40700010;
    CHECK_EQ(XmaCall(__imp__XMACreateContext, out), 0u);
    uint32_t c = GuestLoad32(out);
    CHECK(c >= 0xA0000000u);
    // in0 0xA1000000 x 4 packets, in1 none, read offset 32 bits, out 0xA2000000 x 8 blocks, stereo.
    const uint32_t fields[] = {0xA1000000, 4, 0, 0, 32, 0xA2000000, 8, 0, 3, 1, 3, 0, 0, 0};
    for (int i = 0; i < 14; i++)
        GuestStore32(init + i * 4, fields[i]);
    CHECK_EQ(XmaCall(__imp__XMAInitializeContext, c, init), 0u);
    CHECK_EQ(GuestLoad32(c + 5 * 4), 0x01000000u); // physical input pointer
    CHECK_EQ(GuestLoad32(c + 7 * 4), 0x02000000u);
    CHECK_EQ(GuestLoad32(c + 2 * 4) & 0x3FFFFFF, 32u);
    CHECK_EQ(XmaCall(__imp__XMAIsInputBuffer0Valid, c), 0u);
    XmaCall(__imp__XMASetInputBuffer0Valid, c);
    CHECK_EQ(XmaCall(__imp__XMAIsInputBuffer0Valid, c), 1u);
    CHECK_EQ(XmaCall(__imp__XMAIsInputBuffer1Valid, c), 0u);
    CHECK_EQ(GuestLoad32(c) & 0xFFF, 4u); // packet count kept beside the valid bit
    XmaCall(__imp__XMASetOutputBufferValid, c);
    CHECK_EQ(XmaCall(__imp__XMAIsOutputBufferValid, c), 1u);
    XmaCall(__imp__XMASetOutputBufferReadOffset, c, 5);
    CHECK_EQ(XmaCall(__imp__XMAGetOutputBufferReadOffset, c), 5u);
    CHECK_EQ(XmaCall(__imp__XMAGetOutputBufferWriteOffset, c), 0u);

    // A second context is a different slot; a released slot is handed out again.
    CHECK_EQ(XmaCall(__imp__XMACreateContext, out + 4), 0u);
    CHECK(GuestLoad32(out + 4) != c);
    XmaCall(__imp__XMAReleaseContext, c);
    CHECK_EQ(XmaCall(__imp__XMACreateContext, out + 8), 0u);
    CHECK_EQ(GuestLoad32(out + 8), c);
}
