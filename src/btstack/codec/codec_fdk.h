/*
 * codec_fdk.h — the shared FDK-AAC encoder core used by BOTH AAC-LC and
 * AAC-ELD (one handle, one fill path; only one codec is configured at a
 * time). The state and fill currently live in the engine
 * (btstack_avdtp_source.c) because the ELD fill is fused with the slot
 * queue, resampler and recovery hooks; this header is the seam.
 */
#ifndef USBPODS_CODEC_FDK_H
#define USBPODS_CODEC_FDK_H

#include "aacenc_lib.h"
#include "../a2dp_engine.h"

extern HANDLE_AACENCODER handleAAC;
extern AACENC_InfoStruct aacinf;

int  codec_fdk_fill(a2dp_media_sending_context_t *ctx);  /* AAC-LC + ELD */
void codec_fdk_release(void);                            /* close handleAAC */

#endif
