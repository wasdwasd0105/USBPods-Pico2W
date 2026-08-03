/*
 * codec_registry.c — THE codec list. Priority order: selection walks this
 * top-down and takes the first available codec the remote accepts.
 * Add a codec: one file + one line here. Remove: delete both.
 */
#include <stddef.h>
#include "codec.h"
#include "../../settings.h"

const codec_ops_t * const codec_registry[] = {
    &codec_aaceld_ops,   /* AirPods first: the whole point of this dongle */
    &codec_lhdc_ops,     /* LHDC V5 above LDAC while it proves itself — buds
                            that offer both currently pick LHDC for testing */
    &codec_ldac_ops,
    &codec_aac_ops,
    &codec_sbc_ops,      /* mandatory baseline — must stay last */
    NULL,
};

const codec_ops_t *codec_by_id(uint8_t id){
    for (int i = 0; codec_registry[i]; i++)
        if (codec_registry[i]->id == id) return codec_registry[i];
    return NULL;
}

/* User toggle (settings()->codec_dis, bit 1<<id = disabled): selection skips
   disabled codecs at the NEXT connection. SBC is the mandatory A2DP baseline
   and always reports enabled, so a headset can never be left codec-less. */
bool codec_enabled(uint8_t id){
    if (id == CODEC_ID_SBC) return true;
    return (settings()->codec_dis & (uint8_t)(1u << id)) == 0;
}
