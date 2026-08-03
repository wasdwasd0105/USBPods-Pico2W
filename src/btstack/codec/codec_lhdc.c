/*
 * codec_lhdc.c — Savitech LHDC V5 (vendor 0x053A, codec 0x4C35) A2DP source.
 *
 * Encoder: the Rust lhdcv5 crate (3rd-party/lhdcv5, no_std port, staticlib)
 * behind its C wrapper lhdcv5BT_enc.c. One encode call consumes exactly one
 * 5 ms frame (240 samples @48/44.1 k); the wrapper's internal circular
 * buffer flushes a complete multi-frame payload every frame_per_packet
 * calls. We prepend the 2-byte LHDC media header [frames<<2|latency][seq]
 * and ship it as one RTP payload — the framing the Savitech/AOSP decoders
 * strip on the other end.
 *
 * Capability layout (11-byte media_codec_information, after NON_A2DP):
 *   [0..3] vendor 3A 05 00 00   [4..5] codec 35 4C
 *   [6] rates: 0x20=44.1k 0x10=48k 0x04=96k 0x01=192k
 *   [7] bit fmt 0x04=16/0x02=24/0x01=32 | max-BR bits(0x30) | min-BR (0xC0)
 *   [8] version 0x01 = V1 | 0x10 = 5 ms frame
 *   [9] features (JAS/AR/META/LL/LLESS*) — none supported here
 *   [10] LLESS raw flag
 */

#include <stdio.h>
#include <string.h>
#include "pico.h"            /* panic() — routed into usbpods_panic */
#include "lhdcv5BT.h"

#include "codec.h"
#include "../a2dp_engine.h"
#include "../btstack_avdtp_source.h"
#include "../../settings.h"

#define LHDC_VENDOR_ID  0x0000053A
#define LHDC_CODEC_ID   0x4C35

#define LHDC_FRAME_SAMPLES 240        /* 5 ms at 48/44.1 k — fixed in V5 */

/* payload prefix: [hdr][seq] — hdr = frame_count << 2 | latency(2 bits) */
#define LHDC_HDR_BYTES     2
#define LHDC_LATENCY_BITS  0x00       /* low-latency hint; buds only display it */

/* Bitrate: index into g_bitrate_table_48k
 * [64,160,192,256,320,400,500,900,...] — 3=256, 5=400, 6=500 kbps. Savitech's
 * names mislead ("LOW" is 400, "HIGH" jumps to 900), so index by value.
 * 400 kbps is the default: two 5 ms frames + headers pack to ~508 B inside
 * our 656 B payload budget with real air headroom, and it was the first
 * rate stable on HW. 500 packs ~630 B — razor thin, and a frame-size wobble
 * there drops the bundle to 1 frame/packet mid-song ("clear but buggy").
 * 256 is the robust floor for a weak link. */
static uint8_t lhdc_bitrate_index(void){
    switch (settings()->lhdc_rate){
        case 1:  return 3;    /* 256 kbps */
        case 3:  return 6;    /* 500 kbps */
        default: return 5;    /* 400 kbps (0/2 = zero-fill default) */
    }
}
int codec_lhdc_setting_kbps(void){
    switch (settings()->lhdc_rate){
        case 1:  return 256;
        case 3:  return 500;
        default: return 400;
    }
}

#define LHDC_MTU_FOR_PACKING 600      /* wrapper packs frames up to this;
                                         keep below max_media_payload_size
                                         minus our 2-byte prefix */

static HANDLE_LHDC_BT handleLHDC;
static avdtp_stream_endpoint_t *stream_endpoint_lhdc;
static bool    have_lhdc;
static uint8_t remote_caps[11];       /* sink's capability bytes (see layout) */
static uint8_t lhdc_seq;              /* packet sequence for the media header */

/* Live retune. lhdcv5BT_set_bitrate defers the switch until the encoder's
   input ring drains (update_frame_info -> "predict: empty buffer"), so it is
   safe on a running stream — but it must run in the PUMP context that owns
   the encoder, never from USB/console context. Web and console only REQUEST;
   codec_lhdc_tick() applies. */
static volatile uint8_t lhdc_pending_rate;   /* 0 = nothing requested */

void codec_lhdc_request_rate(uint8_t r){
    if (r >= 1 && r <= 3) lhdc_pending_rate = r;
}

void codec_lhdc_tick(void){
    uint8_t r = lhdc_pending_rate;
    if (r == 0) return;
    lhdc_pending_rate = 0;
    if (handleLHDC == NULL) return;      /* not streaming: init reads the setting */
    if (lhdcv5BT_set_bitrate(handleLHDC, lhdc_bitrate_index()) == LHDC_FRET_SUCCESS)
        printf("LHDC bitrate: %d kbps — applied live\n", codec_lhdc_setting_kbps());
    else
        printf("LHDC bitrate: live retune refused — applies at the next connection\n");
}

/* Rust panic lands here (rt.rs) — fold into the usbpods panic path so the
 * watchdog forensics capture it instead of a silent lockup. */
void lhdcv5_rust_panic(void);
void lhdcv5_rust_panic(void) {
    panic("lhdcv5 rust panic");
    while (1) { }                     /* panic() is noreturn; belt+braces */
}

/* ---- capabilities (what WE offer as a source) --------------------------- */

static uint8_t media_lhdc_codec_capabilities[] = {
    0x3A, 0x05, 0x00, 0x00,           /* vendor LE */
    0x35, 0x4C,                       /* codec  LE */
    0x30,                             /* rates: 48 k + 44.1 k (USB pipeline) */
    0x04,                             /* 16-bit in | max-BR 1000 K | min-BR 64 K */
    0x11,                             /* version 1 | 5 ms frames */
    0x00,                             /* no JAS/AR/META/LL features */
    0x00,                             /* no lossless-raw */
};

static uint8_t local_stream_endpoint_lhdc_media_codec_configuration[11];
static uint8_t media_codec_config_data_lhdc[11];

bool codec_lhdc_matches(uint32_t vendor_id, uint16_t codec_id){
    return vendor_id == LHDC_VENDOR_ID && codec_id == LHDC_CODEC_ID;
}

/* The remote seid is captured HERE, at capability-scan time, because it
   cannot be re-derived later: remote_seps[].capabilities.media_codec.
   media_codec_information points into a SHARED reassembly buffer, and a
   headset with TWO vendor SEPs (HW: ROSE BudsFeel — LHDC V5 on seid 3,
   LHDC V3 on seid 4) leaves BOTH entries reading as the last-scanned
   vendor codec. configure()'s old re-scan then matched nothing and LHDC
   silently lost to SBC. */
static uint8_t lhdc_remote_seid;

void codec_lhdc_note_capability(bool present, const uint8_t *caps11, uint8_t remote_seid){
    have_lhdc = present;
    lhdc_remote_seid = present ? remote_seid : 0;
    if (present && caps11 != NULL) {
        memcpy(remote_caps, caps11, sizeof(remote_caps));
        printf("LHDC V5 sink caps: rates 0x%02x fmt/br 0x%02x ver 0x%02x feat 0x%02x\n",
               remote_caps[6], remote_caps[7], remote_caps[8], remote_caps[9]);
    }
}

static bool lhdc_available(void){ return have_lhdc; }

/* ---- negotiation -------------------------------------------------------- */

static uint8_t lhdc_configure(void){
    if (stream_endpoint_lhdc == NULL) {
        stream_endpoint_lhdc = a2dp_source_create_stream_endpoint(
            AVDTP_AUDIO, AVDTP_CODEC_NON_A2DP,
            (uint8_t *) media_lhdc_codec_capabilities, sizeof(media_lhdc_codec_capabilities),
            (uint8_t *) local_stream_endpoint_lhdc_media_codec_configuration,
            sizeof(local_stream_endpoint_lhdc_media_codec_configuration));
        btstack_assert(stream_endpoint_lhdc != NULL);
        stream_endpoint_lhdc->media_codec_configuration_info = local_stream_endpoint_lhdc_media_codec_configuration;
        stream_endpoint_lhdc->media_codec_configuration_len  = sizeof(local_stream_endpoint_lhdc_media_codec_configuration);
        avdtp_source_register_delay_reporting_category(avdtp_local_seid(stream_endpoint_lhdc));
    }

    /* Use the seid captured at scan time — see codec_lhdc_note_capability
       for why the stored capability pointers cannot be trusted here. */
    if (!have_lhdc || lhdc_remote_seid == 0) {
        printf("LHDC: no scanned V5 endpoint (seid %u) — skipping\n", lhdc_remote_seid);
        return ERROR_CODE_UNSUPPORTED_FEATURE_OR_PARAMETER_VALUE;
    }
    int lhdc_sep = -1;
    for (int i = 0; i < num_remote_seps; i++){
        if (remote_seps[i].sep.seid == lhdc_remote_seid) { lhdc_sep = i; break; }
    }
    if (lhdc_sep < 0) {
        printf("LHDC: scanned seid %u not in remote_seps — skipping\n", lhdc_remote_seid);
        return ERROR_CODE_UNSUPPORTED_FEATURE_OR_PARAMETER_VALUE;
    }
    printf("found LHDC V5!!! Remote Stream Endpoints ID is %d (seid %u)\n",
           lhdc_sep, lhdc_remote_seid);
    selected_remote_sep_index = lhdc_sep;

    sc.local_stream_endpoint  = stream_endpoint_lhdc;
    media_tracker.local_seid  = avdtp_local_seid(stream_endpoint_lhdc);
    media_tracker.remote_seid = lhdc_remote_seid;

    /* pipeline rate ∩ sink rates — refuse rather than misconfigure */
    bool want44 = (settings()->usbset & USBSET_RATE44) != 0;
    uint8_t rate_bit = want44 ? 0x20 : 0x10;
    if (!(remote_caps[6] & rate_bit)) {
        /* sink lacks our pipeline rate: try the other one and let the
           resampler story stay out of it (LHDC path has none) */
        uint8_t alt = want44 ? 0x10 : 0x20;
        if (!(remote_caps[6] & alt)) {
            printf("LHDC: sink offers rates 0x%02x, none usable — skipping\n", remote_caps[6]);
            return ERROR_CODE_UNSUPPORTED_FEATURE_OR_PARAMETER_VALUE;
        }
        printf("LHDC: sink lacks our pipeline rate — skipping (rates 0x%02x)\n", remote_caps[6]);
        return ERROR_CODE_UNSUPPORTED_FEATURE_OR_PARAMETER_VALUE;
    }
    if (!(remote_caps[7] & 0x04)) {
        printf("LHDC: sink refuses 16-bit input (fmt 0x%02x) — skipping\n", remote_caps[7]);
        return ERROR_CODE_UNSUPPORTED_FEATURE_OR_PARAMETER_VALUE;
    }

    memcpy(media_codec_config_data_lhdc, media_lhdc_codec_capabilities, 6);
    media_codec_config_data_lhdc[6]  = rate_bit;
    media_codec_config_data_lhdc[7]  = 0x04 |                       /* 16-bit */
        (remote_caps[7] & 0x30) |                                   /* echo max-BR */
        (remote_caps[7] & 0xC0);                                    /* echo min-BR */
    media_codec_config_data_lhdc[8]  = 0x11;                        /* V1 | 5 ms */
    media_codec_config_data_lhdc[9]  = 0x00;                        /* no features */
    media_codec_config_data_lhdc[10] = 0x00;

    avdtp_capabilities_t new_configuration;
    new_configuration.media_codec.media_type = AVDTP_AUDIO;
    new_configuration.media_codec.media_codec_type = AVDTP_CODEC_NON_A2DP;
    new_configuration.media_codec.media_codec_information_len = sizeof(media_codec_config_data_lhdc);
    new_configuration.media_codec.media_codec_information = media_codec_config_data_lhdc;
    uint8_t status = avdtp_source_set_configuration(media_tracker.avdtp_cid,
        media_tracker.local_seid, media_tracker.remote_seid,
        1 << AVDTP_MEDIA_CODEC, new_configuration);
    printf("Set LHDC Connection Result is %d\n", status);
    return status;
}

/* ---- configuration accepted --------------------------------------------- */

void codec_lhdc_on_configuration(const uint8_t *packet){
    const uint8_t *codec_info =
        a2dp_subevent_signaling_media_codec_other_configuration_get_media_codec_information(packet);

    uint8_t rate_bit = codec_info[6];
    int rate = (rate_bit & 0x20) ? 44100 : 48000;

    printf("A2DP Source: Received LHDC V5 configuration! rate 0x%02x fmt 0x%02x ver 0x%02x\n",
           codec_info[6], codec_info[7], codec_info[8]);

    if (handleLHDC != NULL) {          /* reconnect: no stale instance */
        lhdcv5BT_free_handle(handleLHDC);
        handleLHDC = NULL;
    }
    if (lhdcv5BT_get_handle(LHDC_VERSION_1, &handleLHDC) != LHDC_FRET_SUCCESS || handleLHDC == NULL) {
        printf("LHDC: failed to get encoder handle\n");
        return;
    }
    if (lhdcv5BT_init_encoder(handleLHDC, (uint32_t) rate, LHDCBT_SMPL_FMT_S16,
                              lhdc_bitrate_index(), LHDC_MTU_FOR_PACKING,
                              LHDC_ENC_INTERVAL_10MS, 0) != LHDC_FRET_SUCCESS) {
        printf("LHDC: encoder init failed\n");
        lhdcv5BT_free_handle(handleLHDC);
        handleLHDC = NULL;
        return;
    }
    uint32_t kbps = 0;
    lhdcv5BT_get_bitrate(handleLHDC, &kbps);
    printf("LHDC V5 setup complete! rate=%d, bitrate index %d (%lu kbps), 5 ms frames\n",
           rate, lhdc_bitrate_index(), (unsigned long) kbps);

    lhdc_seq = 0;
    set_current_sample_rate(rate);
    audio_timer_interval = 2;          /* 5 ms frames; keep the pump snappy */
    audio_slot_queue_configure_with_count(LHDC_FRAME_SAMPLES, AUDIO_SLOT_COUNT_LDAC);

    cur_codec = CODEC_ID_LHDC;

    avdtp_source_open_stream(media_tracker.avdtp_cid, media_tracker.local_seid, media_tracker.remote_seid);
}

/* ---- lifecycle ---------------------------------------------------------- */

static void lhdc_release(void){
    if (handleLHDC != NULL) {
        lhdcv5BT_free_handle(handleLHDC);
        handleLHDC = NULL;
    }
}

int codec_lhdc_live_kbps(void){
    if (handleLHDC == NULL) return 0;
    uint32_t kbps = 0;
    if (lhdcv5BT_get_bitrate(handleLHDC, &kbps) != LHDC_FRET_SUCCESS) return 0;
    return (int) kbps;
}

/* ---- streaming ---------------------------------------------------------- */

static int lhdc_fill(a2dp_media_sending_context_t *context) {
    int total_samples_read = 0;
    if (handleLHDC == NULL) return 0;

    /* one staged packet at a time: once the wrapper flushes a payload into
       codec_storage (after the 2-byte header slot), stop encoding until it
       has been sent — LDAC-style single-packet staging */
    while (context->samples_ready >= LHDC_FRAME_SAMPLES &&
           context->codec_storage_count == 0) {

        uint8_t slot_idx;
        if (!audio_slot_pop(&slot_idx)) break;

        uint32_t out_bytes = 0, out_frames = 0;
        int32_t rc = lhdcv5BT_encode(handleLHDC,
                audio_slot_data(slot_idx),
                LHDC_FRAME_SAMPLES * 2 /*ch*/ * 2 /*bytes*/,
                &context->codec_storage[LHDC_HDR_BYTES],
                sizeof(context->codec_storage) - LHDC_HDR_BYTES,
                &out_bytes, &out_frames);

        audio_slot_release(slot_idx);

        if (rc != LHDC_FRET_SUCCESS) {
            printf("LHDC encode error %ld\n", (long) rc);
            break;
        }
        total_samples_read      += LHDC_FRAME_SAMPLES;
        context->samples_ready  -= LHDC_FRAME_SAMPLES;

        if (out_bytes > 0) {           /* wrapper flushed a full packet */
            context->codec_storage[0] = (uint8_t)((out_frames << 2) | LHDC_LATENCY_BITS);
            context->codec_storage[1] = lhdc_seq++;
            context->codec_storage_count = (uint16_t)(out_bytes + LHDC_HDR_BYTES);
            context->codec_num_frames    = (uint8_t) out_frames;
        }
    }
    return total_samples_read;
}

static void lhdc_send(void) {
    uint8_t num_frames = media_tracker.codec_num_frames;

    src_tx_pkts++;
    a2dp_source_stream_send_media_payload_rtp(media_tracker.avdtp_cid,
        media_tracker.local_seid, 0, media_tracker.rtp_timestamp,
        &media_tracker.codec_storage[0], media_tracker.codec_storage_count);
    media_tracker.rtp_timestamp += (uint32_t) num_frames * LHDC_FRAME_SAMPLES;

    media_tracker.codec_storage_count = 0;
    media_tracker.codec_ready_to_send = 0;
    media_tracker.codec_num_frames = 0;
}

/* ---- ops ---------------------------------------------------------------- */

const codec_ops_t codec_lhdc_ops = {
    .id        = CODEC_ID_LHDC,
    .name      = "LHDC",
    .available = lhdc_available,
    .configure = lhdc_configure,
    .release   = lhdc_release,
    .fill      = lhdc_fill,
    .send      = lhdc_send,
};
