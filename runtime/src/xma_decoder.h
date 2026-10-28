// One XMA stream as an XMA hardware context sees it: 2048-byte packets in, continuously, never
// an end of stream; 16-bit PCM out, interleaved when stereo, every decoded frame (512 samples
// per channel) as it completes. xma_ffmpeg.c implements it with FFmpeg's XMA2 decoder;
// xma_null.c (no FFmpeg build) opens nothing, which leaves contexts undecoded.
#pragma once
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef struct ScdaXmaStream ScdaXmaStream;

// channels 1 or 2. Null when no decoder is built in or the setup is unsupported.
ScdaXmaStream* scda_xma_open(int channels, int sample_rate);
void scda_xma_close(ScdaXmaStream* stream);
// Forget stream position (new data, seek): the next packet starts a new stream.
void scda_xma_reset(ScdaXmaStream* stream);
// Decodes one packet into out (room for max_samples samples per channel). Returns samples per
// channel written, or < 0 on a bitstream error (the next packet resynchronises).
int scda_xma_decode_packet(ScdaXmaStream* stream, const uint8_t* packet, int16_t* out, int max_samples);
// Decodes one frame: nbits bits from bit 0 of bits (its 15-bit length field first), packet
// headers already removed. Decoder state carries over from the previous frame, as on the
// hardware. Returns samples per channel written (512), or < 0 on a bitstream error.
int scda_xma_decode_frame(ScdaXmaStream* stream, const uint8_t* bits, int nbits, int16_t* out, int max_samples);

#ifdef __cplusplus
}
#endif
