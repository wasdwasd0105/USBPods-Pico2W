/*
 * codec/codec.h — the uniform A2DP source codec interface.
 *
 * One codec = one file in this folder + one line in codec_registry.c.
 * The registry array is priority-ordered: selection walks it top-down and
 * takes the first codec whose remote endpoint accepts our configuration.
 * Removing a codec = delete its file and its registry line; nothing else
 * in the firmware changes.
 *
 * Split of responsibilities:
 *   codec file  — capability template, endpoint registration, remote-SEP
 *                 matching + SET_CONFIGURATION, configuration parsing +
 *                 encoder init, encoder release, PCM->payload fill, packet
 *                 send/packing.
 *   transport   — pump/timing, slot queue, RTP session, recovery, telemetry
 *                 (btstack_avdtp_source.c; shared state via a2dp_engine.h).
 */
#ifndef USBPODS_CODEC_H
#define USBPODS_CODEC_H

#include <stdbool.h>
#include <stdint.h>

/* stable numeric ids — the historic cur_codec values (webhid + logs) */
#define CODEC_ID_NONE    0
#define CODEC_ID_SBC     1
#define CODEC_ID_AAC     2
#define CODEC_ID_LDAC    3
#define CODEC_ID_AACELD  4
#define CODEC_ID_LHDC    5

struct a2dp_media_sending_context;   /* a2dp_engine.h */

typedef struct codec_ops {
    uint8_t     id;
    const char *name;

    /* -- negotiation ------------------------------------------------------
       available(): did the capability scan find a usable remote endpoint?
       configure(): pick that endpoint and send SET_CONFIGURATION.
                    Returns 0 on success (BTstack status otherwise);
                    the config-accepted event then reaches on_config(). */
    bool    (*available)(void);
    uint8_t (*configure)(void);

    /* -- lifecycle --------------------------------------------------------
       release(): free encoder memory at link teardown (idle RAM matters:
       a parked FDK/LDAC instance starves the relay decoder). */
    void    (*release)(void);

    /* -- streaming --------------------------------------------------------
       fill(): encode staged PCM slots into ctx->codec_storage; returns
               samples consumed (0 = nothing to do).
       send(): hand the staged payload to AVDTP and reset the staging. */
    int     (*fill)(struct a2dp_media_sending_context *ctx);
    void    (*send)(void);
} codec_ops_t;

/* priority-ordered, NULL-terminated — THE list (codec_registry.c) */
extern const codec_ops_t * const codec_registry[];

const codec_ops_t *codec_by_id(uint8_t id);        /* NULL if unknown */
bool codec_enabled(uint8_t id);   /* user toggle (settings codec_dis); SBC always on */
int  codec_aac_live_kbps(void);   /* AAC-LC on-air rate (VBR-safe, clamped) */

/* ---- per-codec exports -------------------------------------------------
   Config-accepted events are protocol demux, so the transport forwards the
   raw AVDTP event packet to the owning codec; vendor codecs also export a
   vendor/codec-id match for the NON_A2DP paths. */
extern const codec_ops_t codec_sbc_ops, codec_aac_ops, codec_ldac_ops, codec_aaceld_ops,
                         codec_lhdc_ops;

void codec_sbc_on_capability(const uint8_t *packet);      /* codec-specific parse only */
void codec_sbc_on_configuration(const uint8_t *packet);
void codec_aac_on_capability(const uint8_t *packet);
void codec_aac_on_configuration(const uint8_t *packet);
void codec_aac_on_media_open(void);   /* real MTU known — re-apply bitrate */
void codec_ldac_on_configuration(const uint8_t *packet);
void codec_aaceld_on_configuration(const uint8_t *packet);

bool codec_ldac_matches(uint32_t vendor_id, uint16_t codec_id);
int  codec_ldac_quality_kbps(void);   /* persisted quality: 330 / 660 / 990 */
int  codec_ldac_live_kbps(void);      /* rate the encoder is actually running at */
void codec_ldac_request_quality(uint8_t q);  /* 1/2/3 — retuned LIVE by the tick */
void codec_ldac_on_media_open(void);   /* real MTU known — re-derive packing */
void codec_ldac_tick(void);           /* pump tick: apply a requested quality to the
                                         running encoder (no replug needed) */
bool codec_aaceld_matches(uint32_t vendor_id, uint16_t codec_id);
void codec_ldac_note_capability(bool present);
void codec_aaceld_note_capability(bool present);

/* ---- LHDC V5 (Savitech vendor codec; Rust encoder in 3rd-party/lhdcv5) -- */
bool codec_lhdc_matches(uint32_t vendor_id, uint16_t codec_id);
void codec_lhdc_note_capability(bool present, const uint8_t *caps11, uint8_t remote_seid);
void codec_lhdc_on_configuration(const uint8_t *packet);
int  codec_lhdc_live_kbps(void);      /* encoder's current rate, 0 = not running */
int  codec_lhdc_setting_kbps(void);   /* persisted choice: 256 / 400 / 500 */
void codec_lhdc_request_rate(uint8_t r);  /* 1/2/3 = 256/400/500 — live via tick */
void codec_lhdc_tick(void);           /* pump tick: apply a requested bitrate */

#endif /* USBPODS_CODEC_H */
