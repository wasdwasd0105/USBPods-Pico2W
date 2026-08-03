/* 44.1 kHz -> 48 kHz polyphase resampler (stereo interleaved S16).
 *
 * Table: auto-generated windowed-sinc polyphase (L=160, M=147, 16 taps/phase,
 * Kaiser beta=8.0, q15) — the identical filter proven on the ESP32 (bt_sink
 * relay decode + AAC-ELD pump paths). Output-driven phase walk: each output
 * advances the input position by M/L; 441 input frames (3 x 147 full filter
 * periods) produce EXACTLY 480 output frames, so a 10 ms AAC-ELD granule
 * converts with no fractional carry between frames.
 *
 * Cost: 16 taps x 2 ch x 48000 out/s ~= 1.5M MAC/s — noise at 250 MHz.
 *
 * Instance API: state lives in a caller-owned resamp44_t so independent
 * streams can resample concurrently (ELD encode feed + BT sink relay).
 * Each instance is single-context; the legacy singleton wrappers keep the
 * original ELD call sites unchanged.
 */
#pragma once
#include <stdint.h>
#include <stddef.h>

typedef struct {
    int16_t hist[2][16];   /* per-channel input history, [0] = newest */
    int     phase;         /* output phase within the input interval  */
} resamp44_t;

void   resamp44_reset_st(resamp44_t *st);
/* Returns output FRAMES written (<= max_out_frames). in/out interleaved L/R. */
size_t resamp44_process_st(resamp44_t *st, const int16_t *in, size_t in_frames,
                           int16_t *out, size_t max_out_frames);

/* Legacy singleton (the ELD 44.1-mode encode path). */
void   resamp44_reset(void);
size_t resamp44_process(const int16_t *in, size_t in_frames,
                        int16_t *out, size_t max_out_frames);
