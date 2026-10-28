// Built when FFmpeg is not (external/ffmpeg/build missing): no XMA decoding.
#include "xma_decoder.h"

ScdaXmaStream* scda_xma_open(int channels, int sample_rate)
{
    (void)channels;
    (void)sample_rate;
    return 0;
}

void scda_xma_close(ScdaXmaStream* stream) { (void)stream; }

void scda_xma_reset(ScdaXmaStream* stream) { (void)stream; }

int scda_xma_decode_packet(ScdaXmaStream* stream, const uint8_t* packet, int16_t* out, int max_samples)
{
    (void)stream;
    (void)packet;
    (void)out;
    (void)max_samples;
    return -1;
}

int scda_xma_decode_frame(ScdaXmaStream* stream, const uint8_t* bits, int nbits, int16_t* out, int max_samples)
{
    (void)stream;
    (void)bits;
    (void)nbits;
    (void)out;
    (void)max_samples;
    return -1;
}
