// Offline check of the XMA decoder on packets dumped with SCDA_DUMP_XMA:
//   scda_xma_tool <file.xma> <channels> [out.wav] [rate]
//     packet decode: prints samples per channel decoded and packets that failed.
//   scda_xma_tool --frames <file.xma> <channels> <rate> [out.wav]
//     frame decode (xma_frames.h walker): frames, failures, first sample differing from the
//     packet decode.
//   scda_xma_tool --loop <file.xma> <channels> <rate> <start> <end> <subEnd> <subSkip> <variant>
//                 <wraps> [out.wav]
//     loop [start, end) bits wraps times, decoder state carried across the jump; variant = what
//     the frame at loop end plays: 0 nothing, 1 subframes 0..subEnd (the runtime's), 2 all,
//     10 + n: n subframes from loop end on (may run past that frame). Prints each seam's
//     step |x[j] - x[j-1]| against the median step. variant + 100: after each jump, first decode
//     the frame before loop start (output dropped) so the start frame overlaps with it.
// Writes 16-bit PCM WAV if asked.
#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <vector>
#include "xma_decoder.h"
#include "xma_frames.h"

namespace
{
std::vector<uint8_t> g_data;

void WriteWav(const char* path, const std::vector<int16_t>& pcm, int channels, int rate)
{
    FILE* f = fopen(path, "wb");
    if (!f)
        return;
    uint32_t bytes = uint32_t(pcm.size() * 2);
    struct
    {
        char riff[4]; uint32_t size; char wave[4], fmt[4]; uint32_t fmtSize;
        uint16_t tag, ch; uint32_t rate, bps; uint16_t align, bits; char dataTag[4]; uint32_t dataSize;
    } h{{'R','I','F','F'}, 36 + bytes, {'W','A','V','E'}, {'f','m','t',' '}, 16, 1, uint16_t(channels), uint32_t(rate),
        uint32_t(rate * 2 * channels), uint16_t(2 * channels), 16, {'d','a','t','a'}, bytes};
    fwrite(&h, 1, sizeof(h), f);
    fwrite(pcm.data(), 2, pcm.size(), f);
    fclose(f);
}

std::vector<int16_t> DecodePackets(int channels, int rate, int* failed)
{
    ScdaXmaStream* s = scda_xma_open(channels, rate);
    std::vector<int16_t> pcm;
    static int16_t out[64 * 512 * 2];
    *failed = 0;
    for (size_t p = 0; s && p < g_data.size() / 2048; p++)
    {
        int n = scda_xma_decode_packet(s, &g_data[p * 2048], out, 64 * 512);
        if (n < 0)
            (*failed)++;
        else
            pcm.insert(pcm.end(), out, out + n * channels);
    }
    scda_xma_close(s);
    return pcm;
}

const uint8_t* Packet(uint32_t n) { return n < g_data.size() / 2048 ? &g_data[n * 2048] : nullptr; }

// Frame-level decode from pos while frames last; loop = {start, end, subEnd, subSkip, variant,
// wraps} or null.
std::vector<int16_t> DecodeFrames(ScdaXmaStream* s, int channels, const uint32_t* loop, int* frames, int* failed,
    std::vector<size_t>* seams)
{
    std::vector<int16_t> pcm;
    static uint8_t bits[xma::kMaxFrameBits / 8 + 1];
    static int16_t out[512 * 2];
    uint32_t pos = loop ? xma::kHeaderBits : xma::FirstFrameBit(&g_data[0]);
    uint32_t wraps = loop ? loop[5] : 0, skip = 0, prime = ~0u;
    const bool primed = loop && loop[4] >= 100;
    const uint32_t variant = loop ? loop[4] % 100 : 0;
    if (primed)
        for (uint32_t p = xma::FirstFrameBit(&g_data[0]), next; xma::ReadFrame(Packet, p, bits, &next) >= 0; p = next)
            if (next == loop[0])
            {
                prime = p;
                break;
            }
    auto jump = [&] {
        pos = loop[0], skip = loop[3], wraps--;
        seams->push_back(pcm.size() / channels);
        uint32_t next;
        int n = prime != ~0u ? xma::ReadFrame(Packet, prime, bits, &next) : 0;
        if (n > 0)
            scda_xma_decode_frame(s, bits, n, out, 512);
    };
    int budget = -1;
    *frames = *failed = 0;
    for (;;)
    {
        bool atEnd = loop && wraps && pos == loop[1];
        if (atEnd && variant >= 10) // research: play (variant - 10) subframes from loop end on
            budget = int(variant - 10) * 128;
        if (atEnd && variant == 0)
        {
            jump();
            continue;
        }
        uint32_t next;
        int n = xma::ReadFrame(Packet, pos, bits, &next);
        if (n < 0)
            break;
        pos = next;
        if (!n)
            continue;
        (*frames)++;
        int got = scda_xma_decode_frame(s, bits, n, out, 512);
        if (got < 0)
        {
            (*failed)++;
            continue;
        }
        int from = std::min<int>(got, int(skip * 128)), to = got;
        skip = 0;
        if (atEnd && variant == 1)
            to = std::min<int>(got, int(loop[2] + 1) * 128);
        if (budget >= 0)
        {
            to = std::min(to, from + budget);
            budget -= to - from;
            atEnd = budget == 0;
            if (atEnd)
                budget = -1;
        }
        pcm.insert(pcm.end(), out + from * channels, out + to * channels);
        if (atEnd)
            jump();
    }
    return pcm;
}
} // namespace

int main(int argc, char** argv)
{
    bool frames = argc > 1 && !strcmp(argv[1], "--frames"), loop = argc > 1 && !strcmp(argv[1], "--loop");
    if (frames || loop)
        argv++, argc--;
    if (argc < 3)
        return 2;
    FILE* f = fopen(argv[1], "rb");
    if (!f)
        return 2;
    uint8_t buf[2048];
    while (fread(buf, 1, 2048, f) == 2048)
        g_data.insert(g_data.end(), buf, buf + 2048);
    fclose(f);
    if (g_data.empty())
        return 2;
    const int channels = atoi(argv[2]);
    if (!frames && !loop)
    {
        int rate = argc > 4 ? atoi(argv[4]) : 48000, failed;
        std::vector<int16_t> pcm = DecodePackets(channels, rate, &failed);
        printf("packets %zu failed %d samples %zu\n", g_data.size() / 2048, failed, pcm.size() / channels);
        if (argc > 3)
            WriteWav(argv[3], pcm, channels, rate);
        return 0;
    }
    if (argc < 4 || (loop && argc < 10))
        return 2;
    const int rate = atoi(argv[3]);
    uint32_t loopArgs[6] = {};
    if (loop)
        for (int i = 0; i < 6; i++)
            loopArgs[i] = uint32_t(strtoul(argv[4 + i], nullptr, 0));
    ScdaXmaStream* s = scda_xma_open(channels, rate);
    if (!s)
        return 3;
    int count, failed;
    std::vector<size_t> seams;
    std::vector<int16_t> pcm = DecodeFrames(s, channels, loop ? loopArgs : nullptr, &count, &failed, &seams);
    scda_xma_close(s);
    printf("frames %d failed %d samples %zu\n", count, failed, pcm.size() / channels);
    if (frames)
    {
        int packetFailed;
        std::vector<int16_t> ref = DecodePackets(channels, rate, &packetFailed);
        size_t i = 0;
        while (i < pcm.size() && i < ref.size() && pcm[i] == ref[i])
            i++;
        printf("packet decode samples %zu, identical first %zu\n", ref.size() / channels, i / channels);
    }
    else
    {
        std::vector<int> steps;
        for (size_t i = channels; i < pcm.size(); i += channels)
            steps.push_back(abs(pcm[i] - pcm[i - channels]));
        std::vector<int> sorted = steps;
        std::nth_element(sorted.begin(), sorted.begin() + sorted.size() / 2, sorted.end());
        int median = std::max(1, sorted[sorted.size() / 2]);
        printf("median step %d, loop length %zu samples\n", median, seams.size() > 1 ? seams[1] - seams[0] : 0);
        for (size_t j : seams)
            if (j > 0 && j < pcm.size() / channels)
            {
                // Against the steps around it too: noise-like sounds have big steps everywhere.
                std::vector<int> local;
                for (size_t i = j > 257 ? j - 257 : 0; i < std::min(steps.size(), j + 256); i++)
                    if (i != j - 1)
                        local.push_back(steps[i]);
                std::nth_element(local.begin(), local.begin() + local.size() / 2, local.end());
                printf("  seam at %zu: step %d (%.1fx) local %.1fx\n", j, steps[j - 1], double(steps[j - 1]) / median,
                    double(steps[j - 1]) / std::max(1, local[local.size() / 2]));
            }
    }
    if (argc > (loop ? 10 : 4))
        WriteWav(argv[loop ? 10 : 4], pcm, channels, rate);
    return 0;
}
