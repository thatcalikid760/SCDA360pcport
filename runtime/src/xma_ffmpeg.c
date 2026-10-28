// XMA stream decoder on FFmpeg (LGPL-2.1+, external/ffmpeg). FFmpeg's XMA2 decoder wraps the
// WMA Pro frame decoder for multi-stream files: it holds the last 4096 samples back until end
// of stream (to align streams) and drops the first frame. A hardware context is one stream
// that never ends, so this file compiles wmaprodec.c itself and drives its single-stream
// decode_packet directly: every frame comes out as soon as it is decoded, the first included
// (the hardware writes 512 samples for every frame it decodes). FFmpeg's source is unchanged.
#include "libavcodec/wmaprodec.c"
#include "xma_decoder.h"

struct ScdaXmaStream
{
    AVCodecContext* avctx;
    AVPacket* packet;
    uint8_t data[2048 + AV_INPUT_BUFFER_PADDING_SIZE];
};

static WMAProDecodeCtx* Stream(ScdaXmaStream* x) { return &((XMADecodeCtx*)x->avctx->priv_data)->xma[0]; }

ScdaXmaStream* scda_xma_open(int channels, int sample_rate)
{
    if (channels < 1 || channels > 2)
        return NULL;
    ScdaXmaStream* x = av_mallocz(sizeof(*x));
    if (!x)
        return NULL;
    x->avctx = avcodec_alloc_context3(&ff_xma2_decoder.p);
    x->packet = av_packet_alloc();
    // XMA2WAVEFORMATEX (34 bytes): u16 stream count, u32 channel mask, rest unused here.
    uint8_t* extradata = av_mallocz(34 + AV_INPUT_BUFFER_PADDING_SIZE);
    if (!x->avctx || !x->packet || !extradata)
    {
        av_free(extradata);
        scda_xma_close(x);
        return NULL;
    }
    AV_WL16(extradata, 1);
    AV_WL32(extradata + 2, channels == 2 ? 3 : 4);
    x->avctx->extradata = extradata;
    x->avctx->extradata_size = 34;
    x->avctx->sample_rate = sample_rate;
    av_channel_layout_default(&x->avctx->ch_layout, channels);
    if (avcodec_open2(x->avctx, &ff_xma2_decoder.p, NULL) < 0)
    {
        scda_xma_close(x);
        return NULL;
    }
    Stream(x)->skip_frame = 0;
    return x;
}

void scda_xma_close(ScdaXmaStream* x)
{
    if (!x)
        return;
    avcodec_free_context(&x->avctx);
    av_packet_free(&x->packet);
    av_free(x);
}

void scda_xma_reset(ScdaXmaStream* x)
{
    if (!x)
        return;
    flush(Stream(x));
    Stream(x)->skip_frame = 0;
}

static int Output(ScdaXmaStream* x, int16_t* out, int max_samples)
{
    WMAProDecodeCtx* s = Stream(x);
    AVFrame* frame = ((XMADecodeCtx*)x->avctx->priv_data)->frames[0];
    int n = FFMIN(frame->nb_samples, max_samples);
    for (int c = 0; c < s->nb_channels; c++)
    {
        const float* in = (const float*)frame->extended_data[c];
        for (int k = 0; k < n; k++)
            out[k * s->nb_channels + c] = (int16_t)av_clip_int16(lrintf(in[k] * 32768.0f));
    }
    return n;
}

int scda_xma_decode_frame(ScdaXmaStream* x, const uint8_t* bits, int nbits, int16_t* out, int max_samples)
{
    if (!x)
        return -1;
    WMAProDecodeCtx* s = Stream(x);
    AVFrame* frame = ((XMADecodeCtx*)x->avctx->priv_data)->frames[0];
    if (nbits < 16 || (nbits + 7) / 8 > MAX_FRAMESIZE)
        return -1;
    if (!frame->data[0])
    {
        frame->nb_samples = 512;
        if (ff_get_buffer(x->avctx, frame, 0) < 0)
            return -1;
    }
    // What save_bits leaves for decode_frame: the frame alone in the reservoir, from bit 0.
    memcpy(s->frame_data, bits, (nbits + 7) / 8);
    memset(s->frame_data + (nbits + 7) / 8, 0, AV_INPUT_BUFFER_PADDING_SIZE);
    s->frame_offset = 0;
    s->num_saved_bits = nbits;
    init_get_bits(&s->gb, s->frame_data, nbits);
    s->packet_loss = 0;
    int got = 0;
    decode_frame(s, frame, &got);
    if (s->packet_loss)
    {
        s->packet_loss = 0;
        return -1;
    }
    return got ? Output(x, out, max_samples) : 0;
}

int scda_xma_decode_packet(ScdaXmaStream* x, const uint8_t* packet, int16_t* out, int max_samples)
{
    if (!x)
        return -1;
    WMAProDecodeCtx* s = Stream(x);
    AVFrame* frame = ((XMADecodeCtx*)x->avctx->priv_data)->frames[0];
    const int channels = s->nb_channels;
    memcpy(x->data, packet, 2048);
    memset(x->data + 2048, 0, AV_INPUT_BUFFER_PADDING_SIZE);
    x->packet->data = x->data;
    x->packet->size = 2048;
    int written = 0;
    // decode_packet takes one frame per call and returns the bytes it consumed; like FFmpeg's
    // decode loop (decode.c), the next call gets the rest, until the packet is used up. A packet
    // holds at most 64 frames (6-bit count): bound the loop against a corrupt stream.
    for (int i = 0; i < 128 && x->packet->size > 0; i++)
    {
        if (!frame->data[0])
        {
            frame->nb_samples = 512;
            if (ff_get_buffer(x->avctx, frame, 0) < 0)
                return -1;
        }
        int got = 0;
        int ret = decode_packet(x->avctx, s, frame, &got, x->packet);
        if (got)
        {
            int n = FFMIN(frame->nb_samples, max_samples - written);
            for (int c = 0; c < channels; c++)
            {
                const float* in = (const float*)frame->extended_data[c];
                for (int k = 0; k < n; k++)
                    out[(written + k) * channels + c] = (int16_t)av_clip_int16(lrintf(in[k] * 32768.0f));
            }
            written += n;
        }
        if (ret < 0)
            return written ? written : ret;
        if (ret >= x->packet->size)
            break;
        x->packet->data += ret;
        x->packet->size -= ret;
    }
    return written;
}
