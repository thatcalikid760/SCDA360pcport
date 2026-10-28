// Offline check of the XMA decoder on packets dumped with SCDA_DUMP_XMA:
//   scda_xma_tool <file.xma> <channels> [out.wav]
// Prints samples per channel decoded and packets that failed; writes 16-bit PCM WAV if asked.
#include <cstdio>
#include <cstdlib>
#include <vector>
#include "xma_decoder.h"

int main(int argc, char** argv)
{
    if (argc < 3)
        return 2;
    FILE* f = fopen(argv[1], "rb");
    if (!f)
        return 2;
    std::vector<uint8_t> data;
    uint8_t buf[2048];
    while (fread(buf, 1, 2048, f) == 2048)
        data.insert(data.end(), buf, buf + 2048);
    fclose(f);
    int channels = atoi(argv[2]);
    int rate = argc > 4 ? atoi(argv[4]) : 48000;
    ScdaXmaStream* s = scda_xma_open(channels, rate);
    if (!s)
        return 3;
    std::vector<int16_t> pcm;
    static int16_t out[64 * 512 * 2];
    int failed = 0;
    for (size_t p = 0; p < data.size() / 2048; p++)
    {
        int n = scda_xma_decode_packet(s, &data[p * 2048], out, 64 * 512);
        if (n < 0)
            failed++;
        else
            pcm.insert(pcm.end(), out, out + n * channels);
    }
    scda_xma_close(s);
    printf("packets %zu failed %d samples %zu\n", data.size() / 2048, failed, pcm.size() / channels);
    if (argc > 3 && (f = fopen(argv[3], "wb")))
    {
        uint32_t bytes = uint32_t(pcm.size() * 2);
        struct
        {
            char riff[4]; uint32_t size; char wave[4], fmt[4]; uint32_t fmtSize;
            uint16_t tag, ch; uint32_t rate, bps; uint16_t align, bits; char dataTag[4]; uint32_t dataSize;
        } h{{'R','I','F','F'}, 36 + bytes, {'W','A','V','E'}, {'f','m','t',' '}, 16, 1, uint16_t(channels), 48000,
            uint32_t(48000 * 2 * channels), uint16_t(2 * channels), 16, {'d','a','t','a'}, bytes};
        fwrite(&h, 1, sizeof(h), f);
        fwrite(pcm.data(), 2, pcm.size(), f);
        fclose(f);
    }
    return 0;
}
