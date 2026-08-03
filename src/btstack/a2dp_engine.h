/*
 * a2dp_engine.h — INTERNAL API between the A2DP source transport
 * (btstack_avdtp_source.c: pump, slot queue, RTP session, recovery,
 * telemetry) and the codec modules (codec/codec_*.c).
 *
 * Nothing outside src/btstack/ may include this. The public firmware API
 * stays in btstack_avdtp_source.h.
 */
#ifndef USBPODS_A2DP_ENGINE_H
#define USBPODS_A2DP_ENGINE_H

#include <stdbool.h>
#include <stdint.h>
#include "btstack.h"

#define AVDTP_MAX_SEP_NUM 10
#define AVDTP_MAX_MEDIA_CODEC_CONFIG_LEN 16
#define AVDTP_MAX_MEDIA_CODEC_CAPABILITES_EVENT_LEN 100

/* ---- media session context (one per headset link) ----------------------- */
typedef struct a2dp_media_sending_context {
    uint16_t avdtp_cid;
    uint8_t  local_seid;
    uint8_t  remote_seid;
    uint16_t avrcp_cid;

    uint32_t time_audio_data_sent; // ms
    uint32_t acc_num_missed_samples;
    _Atomic uint32_t samples_ready;   /* pacing credit: touched from pump +
                                         (historically) USB IRQ — atomic RMWs
                                         on the M33 close the resurrection race
                                         even if a new cross-context writer
                                         sneaks in later */
    btstack_timer_source_t audio_timer;
    uint8_t  streaming;
    int      max_media_payload_size;

    uint32_t rtp_timestamp;

    uint8_t  codec_storage[1500];  // 3 AAC-ELD frames: 3*~400 = 1200 + margin
    uint16_t codec_storage_count;
    uint8_t  codec_ready_to_send;
    uint16_t codec_num_frames;
    uint8_t volume;
}  a2dp_media_sending_context_t;

extern a2dp_media_sending_context_t media_tracker;

/* ---- remote endpoint table (filled by the capability scan) -------------- */
typedef struct {
    avdtp_sep_t  sep;
    bool have_media_codec_apabilities;
    uint8_t                  media_codec_event[AVDTP_MAX_MEDIA_CODEC_CAPABILITES_EVENT_LEN];
    uint32_t                 vendor_id;
    uint32_t                 codec_id;
} remote_sep_entry_t;

extern remote_sep_entry_t remote_seps[AVDTP_MAX_SEP_NUM];
extern uint16_t num_remote_seps;
extern uint8_t  selected_remote_sep_index;

/* stream context: the endpoint being configured + its sample rate */
typedef struct {
    avdtp_stream_endpoint_t * local_stream_endpoint;
    uint32_t sampling_frequency;
} a2dp_stream_context_t;
extern a2dp_stream_context_t sc;

/* scratch buffer codecs build their SET_CONFIGURATION bytes in */
extern uint16_t media_codec_config_len;
extern uint8_t  media_codec_config_data[AVDTP_MAX_MEDIA_CODEC_CONFIG_LEN];

/* ---- codec selection state --------------------------------------------- */
extern uint8_t cur_codec;              /* CODEC_ID_*; 0 = none negotiated */

/* ---- audio slot queue (PCM staging, multicore-safe) --------------------- */
bool     audio_slot_pop(uint8_t *slot_idx);
void     audio_slot_release(uint8_t slot_idx);
int16_t *audio_slot_data(uint8_t slot_idx);
void     audio_slot_queue_configure_with_count(uint16_t samples_per_slot,
                                               uint8_t slot_count);

/* ---- pump / timing knobs (set by codecs at configure time) -------------- */
extern uint8_t  audio_timer_interval;    /* pump tick, ms */
extern uint16_t acc_num_simples;         /* INPUT frames per encode slot */
extern uint16_t rtp_samples_per_frame;   /* RTP clock units per frame */
void configure_sample_rate(int sampling_frequency);
int  get_current_sample_rate(void);
void set_current_sample_rate(int hz);

/* 44.1 pipeline -> 48 k ELD encoder resample bridge */
extern bool    eld_resample;
extern int16_t eld_rs_buf[480 * 2];

/* ---- transport telemetry + recovery hooks (written by send paths) ------- */
extern uint32_t src_tx_pkts;             /* every media packet sent          */
extern uint32_t src_splice_last_ms;      /* damage signal timestamp          */
extern volatile bool src_auto_kick_pending; /* arms the recovery kick        */

/* protocol-log gate ('C'/debug mode): capability + config chatter */
extern bool src_proto_log;
#define PLOG(...) do { if (src_proto_log) printf(__VA_ARGS__); } while (0)

/* ---- FDK relay-ladder restore points + AAC setup gate ------------------- */
extern uint32_t src_full_bitrate;      /* set at each encoder init            */
extern uint8_t  src_vbr_mode;          /* BITRATEMODE applied at init (0=CBR) */
extern bool     src_relay_rate_low;
extern bool     finish_setup_aac;      /* AAC config gate (scan-order race)   */
extern uint8_t  aac_audio_timer_interval;

/* ELD engine core hosted in the transport (see codec_aaceld.c header) */
void a2dp_source_eld_send(void);

/* ---- shared helpers ----------------------------------------------------- */
uint32_t get_vendor_id(const uint8_t *codec_info);
uint16_t get_codec_id(const uint8_t *codec_info);

#endif /* USBPODS_A2DP_ENGINE_H */
