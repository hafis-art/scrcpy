#include "audio_player.h"

#include "util/log.h"
#include "SDL3/SDL_hints.h"

/** Downcast frame_sink to sc_audio_player */
#define DOWNCAST(SINK) container_of(SINK, struct sc_audio_player, frame_sink)

#define SC_SDL_SAMPLE_FMT SDL_AUDIO_F32LE

static void SDLCALL
sc_audio_player_stream_callback(void *userdata, SDL_AudioStream *stream,
                                int additional_amount, int total_amount) {
    (void) total_amount;

    struct sc_audio_player *ap = userdata;

    if (additional_amount > 0) {
        size_t len = additional_amount;

        assert(len <= ap->aout_buffer_size);
        if (len > ap->aout_buffer_size) {
            // Just in case for release builds
            LOGE("Unexpected SDL audio behavior: too much data requested");
            len = ap->aout_buffer_size;
        }

        assert(len % ap->audioreg.sample_size == 0);
        uint32_t out_samples = len / ap->audioreg.sample_size;

        sc_audio_regulator_pull(&ap->audioreg, ap->aout_buffer, out_samples);
        bool ok = SDL_PutAudioStreamData(stream, ap->aout_buffer, len);
        SDL_stack_free(data);
        if (!ok) {
            LOGW("Audio stream error: %s", SDL_GetError());
        }
    }
}

static bool
sc_audio_player_frame_sink_push(struct sc_frame_sink *sink,
                                const AVFrame *frame) {
    struct sc_audio_player *ap = DOWNCAST(sink);

    return sc_audio_regulator_push(&ap->audioreg, frame);
}

static bool
sc_audio_player_frame_sink_open(struct sc_frame_sink *sink,
                                const AVCodecContext *ctx) {
    struct sc_audio_player *ap = DOWNCAST(sink);

#ifdef SCRCPY_LAVU_HAS_CHLAYOUT
    assert(ctx->ch_layout.nb_channels > 0 && ctx->ch_layout.nb_channels < 256);
    uint8_t nb_channels = ctx->ch_layout.nb_channels;
#else
    int tmp = av_get_channel_layout_nb_channels(ctx->channel_layout);
    assert(tmp > 0 && tmp < 256);
    uint8_t nb_channels = tmp;
#endif

    assert(ctx->sample_rate > 0);
    assert(!av_sample_fmt_is_planar(SC_AV_SAMPLE_FMT));
    int out_bytes_per_sample = av_get_bytes_per_sample(SC_AV_SAMPLE_FMT);
    assert(out_bytes_per_sample > 0);

    uint32_t target_buffering_samples =
        ap->target_buffering_delay * ctx->sample_rate / SC_TICK_FREQ;

    size_t sample_size = nb_channels * out_bytes_per_sample;
    bool ok = sc_audio_regulator_init(&ap->audioreg, sample_size, ctx,
                                      target_buffering_samples);
    if (!ok) {
        return false;
    }

    uint64_t aout_samples = ap->output_buffer_duration * ctx->sample_rate
                                                       / SC_TICK_FREQ;
    assert(aout_samples <= 0xFFFF);

    char str[5 + 1]; // max 65535
    int r = snprintf(str, sizeof(str), "%" PRIu16, (uint16_t) aout_samples);
    assert(r >= 0 && (size_t) r < sizeof(str));
    (void) r;
    if (!SDL_SetHint(SDL_HINT_AUDIO_DEVICE_SAMPLE_FRAMES, str)) {
        LOGE("Could not set audio output buffer");
        sc_audio_regulator_destroy(&ap->audioreg);
        return false;
    }

    ap->aout_buffer_size = aout_samples * sample_size;
    ap->aout_buffer = malloc(ap->aout_buffer_size);
    if (!ap->aout_buffer) {
        sc_audio_regulator_destroy(&ap->audioreg);
        return false;
    }

    SDL_AudioSpec spec = {
        .freq = ctx->sample_rate,
        .format = SC_SDL_SAMPLE_FMT,
        .channels = nb_channels,
    };

    ap->stream = SDL_OpenAudioDeviceStream(SDL_AUDIO_DEVICE_DEFAULT_PLAYBACK,
                                           &spec,
                                           sc_audio_player_stream_callback, ap);
    if (!ap->stream) {
        LOGE("Could not open audio device: %s", SDL_GetError());
        free(ap->aout_buffer);
        sc_audio_regulator_destroy(&ap->audioreg);
        return false;
    }

    ap->device = SDL_GetAudioStreamDevice(ap->stream);
    assert(ap->device);

    // The thread calling open() is the thread calling push(), which fills the
    // audio buffer consumed by the SDL audio thread.
    ok = sc_thread_set_priority(SC_THREAD_PRIORITY_TIME_CRITICAL);
    if (!ok) {
        ok = sc_thread_set_priority(SC_THREAD_PRIORITY_HIGH);
        (void) ok; // We don't care if it worked, at least we tried
    }

    SDL_ResumeAudioDevice(ap->device);

    return true;
}

static void
sc_audio_player_frame_sink_close(struct sc_frame_sink *sink) {
    struct sc_audio_player *ap = DOWNCAST(sink);

    assert(ap->stream);
    assert(ap->device);
    SDL_PauseAudioDevice(ap->device);

    // ap->device is owned by ap->stream
    SDL_DestroyAudioStream(ap->stream);

    sc_audio_regulator_destroy(&ap->audioreg);

    free(ap->aout_buffer);
}

void
sc_audio_player_init(struct sc_audio_player *ap, sc_tick target_buffering,
                     sc_tick output_buffer_duration) {
    ap->target_buffering_delay = target_buffering;
    ap->output_buffer_duration = output_buffer_duration;

    static const struct sc_frame_sink_ops ops = {
        .open = sc_audio_player_frame_sink_open,
        .close = sc_audio_player_frame_sink_close,
        .push = sc_audio_player_frame_sink_push,
    };

    ap->frame_sink.ops = &ops;
}
