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

// A packet holding one 32-bit frame at its start whose third byte is value (xma_fake_decoder.c
// decodes it to 512 samples of that value), then padding.
static void PutFramePacket(uint32_t packet, uint8_t value)
{
    const uint8_t frame[] = {0, 0, 0, 0, 0x00, 0x40, value, 0}; // header, length 32 bits
    memcpy(g_base + packet, frame, sizeof(frame));
    memset(g_base + packet + sizeof(frame), 0xFF, 2048 - sizeof(frame));
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

// Decoder pass (tests link xma_fake_decoder.c), one frame per packet.
TEST(xma_decoder_fills_output_and_returns_input_buffers)
{
    TestGuestBase();
    const uint32_t out = 0x40700100, init = 0x40700110;
    const uint32_t in0 = 0xA3000000, in1 = 0xA3001000, pcm = 0xA3002000;
    PutFramePacket(in0, 0x11);
    PutFramePacket(in0 + 2048, 0x22);
    PutFramePacket(in1, 0x33);
    CHECK_EQ(XmaCall(__imp__XMACreateContext, out), 0u);
    uint32_t c = GuestLoad32(out);
    // Mono 48 kHz, 15 output blocks: 3 packets = 3 x 1024 bytes = 12 blocks fit.
    const uint32_t fields[] = {in0, 2, in1, 1, 32, pcm, 15, 0, 2, 0, 3, 0, 0, 0};
    for (int i = 0; i < 14; i++)
        GuestStore32(init + i * 4, fields[i]);
    XmaCall(__imp__XMAInitializeContext, c, init);
    XmaCall(__imp__XMASetInputBuffer0Valid, c);
    XmaCall(__imp__XMASetInputBuffer1Valid, c);
    XmaCall(__imp__XMASetOutputBufferValid, c);
    XmaCall(__imp__XMAEnableContext, c);
    for (int i = 0; i < 200 && XmaCall(__imp__XMAGetOutputBufferWriteOffset, c) != 12; i++)
        Sleep(2);
    XmaCall(__imp__XMADisableContext, c);
    CHECK_EQ(XmaCall(__imp__XMAGetOutputBufferWriteOffset, c), 12u);
    CHECK_EQ(XmaCall(__imp__XMAIsInputBuffer0Valid, c), 0u);
    CHECK_EQ(XmaCall(__imp__XMAIsInputBuffer1Valid, c), 0u);
    CHECK_EQ(XmaCall(__imp__XMAIsOutputBufferValid, c), 1u);
    CHECK_EQ(GuestLoad16(pcm), 0x11u);                // big-endian 16-bit samples
    CHECK_EQ(GuestLoad16(pcm + 4 * 256 + 2), 0x22u);  // second packet from block 4
    CHECK_EQ(GuestLoad16(pcm + 8 * 256 + 510), 0x33u); // in1 after in0

    // Refill in0 with 2 more packets: 8 blocks, but write == read would read as empty, so only
    // 2 of the 3 free blocks fill (write offset 14, one short of read offset 0) and output
    // buffer valid clears.
    XmaCall(__imp__XMASetInputBuffer0Valid, c);
    XmaCall(__imp__XMAEnableContext, c);
    for (int i = 0; i < 200 && XmaCall(__imp__XMAIsOutputBufferValid, c); i++)
        Sleep(2);
    XmaCall(__imp__XMADisableContext, c);
    CHECK_EQ(XmaCall(__imp__XMAIsOutputBufferValid, c), 0u);
    CHECK_EQ(XmaCall(__imp__XMAGetOutputBufferWriteOffset, c), 14u);
    XmaCall(__imp__XMAReleaseContext, c);
}

// Loop from frame 1 to frame 2 (one per packet) x 2, subframe end 1, skip 1: frame 0, 1, 2 (256
// samples), then twice 1 (from sample 128) and 2 (256, then 512 once the count is used up).
TEST(xma_decoder_loops_frames)
{
    TestGuestBase();
    const uint32_t out = 0x40700200, init = 0x40700210;
    const uint32_t in0 = 0xA3004000, pcm = 0xA3006000;
    PutFramePacket(in0, 0x11);
    PutFramePacket(in0 + 2048, 0x22);
    PutFramePacket(in0 + 4096, 0x33);
    CHECK_EQ(XmaCall(__imp__XMACreateContext, out), 0u);
    uint32_t c = GuestLoad32(out);
    const uint32_t fields[] = {in0, 3, 0, 0, 32, pcm, 30, 0, 2, 0, 3, 16384 + 32, 2 * 16384 + 32, 0x02010100};
    for (int i = 0; i < 14; i++)
        GuestStore32(init + i * 4, fields[i]);
    XmaCall(__imp__XMAInitializeContext, c, init);
    XmaCall(__imp__XMASetInputBuffer0Valid, c);
    XmaCall(__imp__XMASetOutputBufferValid, c);
    XmaCall(__imp__XMAEnableContext, c);
    for (int i = 0; i < 200 && XmaCall(__imp__XMAIsInputBuffer0Valid, c); i++)
        Sleep(2);
    XmaCall(__imp__XMADisableContext, c);
    CHECK_EQ(XmaCall(__imp__XMAIsInputBuffer0Valid, c), 0u);
    CHECK_EQ(XmaCall(__imp__XMAGetOutputBufferWriteOffset, c), 22u); // 128-sample blocks
    CHECK_EQ((GuestLoad32(c) >> 12) & 0xFF, 0u); // loop count used up
    const uint8_t expect[] = {0x11, 0x11, 0x11, 0x11, 0x22, 0x22, 0x22, 0x22, 0x33, 0x33, 0x22,
        0x22, 0x22, 0x33, 0x33, 0x22, 0x22, 0x22, 0x33, 0x33, 0x33, 0x33};
    for (int b = 0; b < 22; b++)
        CHECK_EQ(GuestLoad16(pcm + b * 256 + 254), expect[b]);
    XmaCall(__imp__XMAReleaseContext, c);
}

void XmaEnsureArray();
void XmaKickVoiceSet(uint32_t set);
void XmaLockVoiceSet(uint32_t set);

// XAudio's own contexts: register 0x7FEA1800 holds the array (little-endian), a kick of its voice
// set decodes without XMAInitializeContext / XMAEnableContext, a lock stops decoding.
TEST(xma_xaudio_voice_set_kick_and_lock)
{
    TestGuestBase();
    XmaEnsureArray();
    const uint32_t out = 0x40700200, set = 0x40700210, entries = 0x40700300;
    const uint32_t in0 = 0xA3010000, pcm = 0xA3012000;
    PutFramePacket(in0, 0x44);
    CHECK_EQ(XmaCall(__imp__XMACreateContext, out), 0u);
    const uint32_t c = GuestLoad32(out);
    uint32_t reg;
    memcpy(&reg, g_base + 0x7FEA1800, 4);
    CHECK_EQ(c & 0x3F, 0u);
    CHECK(c >= reg && (c - reg) % 64 == 0);
    // Context written directly: in0 1 packet valid, read offset 32, mono 48 kHz, out 8 blocks valid.
    GuestStore32(c + 0, 1 | (1u << 20) | (8u << 22));
    GuestStore32(c + 4, (3u << 27) | (1u << 31));
    GuestStore32(c + 8, 32);
    GuestStore32(c + 20, in0 & 0x1FFFFFFF);
    GuestStore32(c + 28, pcm & 0x1FFFFFFF);
    GuestStore32(set, 1);
    GuestStore32(set + 4, 0);
    GuestStore32(set + 8, entries);
    GuestStore32(entries + 64, c);
    XmaKickVoiceSet(set);
    for (int i = 0; i < 200 && XmaCall(__imp__XMAGetOutputBufferWriteOffset, c) != 4; i++)
        Sleep(2);
    XmaLockVoiceSet(set);
    CHECK_EQ(XmaCall(__imp__XMAGetOutputBufferWriteOffset, c), 4u); // 512 mono samples = 4 blocks
    CHECK_EQ(XmaCall(__imp__XMAIsInputBuffer0Valid, c), 0u);
    CHECK_EQ(GuestLoad16(pcm), 0x44u);
    // Locked: new input stays undecoded.
    GuestStore32(c + 0, GuestLoad32(c + 0) | (1u << 20));
    GuestStore32(c + 8, 32);
    Sleep(20);
    CHECK_EQ(XmaCall(__imp__XMAIsInputBuffer0Valid, c), 1u);
    XmaCall(__imp__XMAReleaseContext, c);
}
