// Test XMA decoder: every frame decodes to 512 samples per channel, all equal to the frame's third
// byte; every packet to one such frame whose samples equal the packet's second byte.
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

// Frames: 512 samples per channel, all equal to the frame's third byte (after the length).
int scda_xma_decode_frame(ScdaXmaStream* stream, const uint8_t* bits, int nbits, int16_t* out, int max_samples)
{
    (void)nbits;
    int n = max_samples < 512 ? max_samples : 512;
    for (int i = 0; i < n * stream->channels; i++)
        out[i] = bits[2];
    return n;
}

int scda_xma_decode_packet(ScdaXmaStream* stream, const uint8_t* packet, int16_t* out, int max_samples)
{
    int n = max_samples < 512 ? max_samples : 512;
    for (int i = 0; i < n * stream->channels; i++)
        out[i] = packet[1];
    return n;
}
