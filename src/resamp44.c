#include <string.h>
#include "resamp44.h"
#include "resamp44_taps.h"   /* sbc_rs_taps[RS_L][RS_T], RS_L=160 RS_M=147 RS_T=16 */

_Static_assert(RS_T == 16, "resamp44_t.hist is sized for 16 taps");

void resamp44_reset_st(resamp44_t *st) {
    memset(st, 0, sizeof(*st));
}

size_t resamp44_process_st(resamp44_t *st, const int16_t *in, size_t in_frames,
                           int16_t *out, size_t max_out_frames) {
    size_t oi = 0;
    for (size_t ii = 0; ii < in_frames; ii++) {
        for (int c = 0; c < 2; c++) {
            memmove(&st->hist[c][1], &st->hist[c][0], (RS_T - 1) * sizeof(int16_t));
            st->hist[c][0] = in[ii * 2 + c];
        }
        while (st->phase < RS_L && oi < max_out_frames) {
            const int16_t *h = sbc_rs_taps[st->phase];
            for (int c = 0; c < 2; c++) {
                int32_t acc = 0;
                for (int k = 0; k < RS_T; k++) {
                    acc += (int32_t)st->hist[c][k] * h[k];
                }
                acc >>= 15;
                if (acc > 32767) acc = 32767;
                if (acc < -32768) acc = -32768;
                out[oi * 2 + c] = (int16_t)acc;
            }
            oi++;
            st->phase += RS_M;
        }
        st->phase -= RS_L;
    }
    return oi;
}

/* Legacy singleton — the ELD 44.1-mode encode path. */
static resamp44_t s_eld;

void resamp44_reset(void) { resamp44_reset_st(&s_eld); }

size_t resamp44_process(const int16_t *in, size_t in_frames,
                        int16_t *out, size_t max_out_frames) {
    return resamp44_process_st(&s_eld, in, in_frames, out, max_out_frames);
}
