// Test XMA decoder: every packet decodes to one 512-sample frame per channel whose samples all
// equal the packet's second byte (the first is part of the packet header in real streams).
#include <stdlib.h>
#include "xma_decoder.h"

struct ScdaXmaStream
{
    int channels;
};

ScdaXmaStream* scda_xma_open(int channels, int sample_rate)
{
    (void)sample_rate;
    ScdaXmaStream* s = malloc(sizeof(*s));
    s->channels = channels;
    return s;
}

void scda_xma_close(ScdaXmaStream* stream) { free(stream); }

void scda_xma_reset(ScdaXmaStream* stream) { (void)stream; }

int scda_xma_decode_packet(ScdaXmaStream* stream, const uint8_t* packet, int16_t* out, int max_samples)
{
    int n = max_samples < 512 ? max_samples : 512;
    for (int i = 0; i < n * stream->channels; i++)
        out[i] = packet[1];
    return n;
}
