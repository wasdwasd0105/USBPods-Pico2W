/*
 * codec_ldac.c — Sony LDAC (vendor 0x012D, codec 0xAA) A2DP source codec.
 *
 * Encoder: libldacBT (3rd-party/ldacBT), SQ quality, 256-sample LSU.
 * Packet: [num_frames] byte + concatenated LDAC frames; RTP clock advances
 * by frames * LDACBT_ENC_LSU (sample count).
 */

#include <stdio.h>
#include <ldacBT.h>

#include "codec.h"
#include "../a2dp_engine.h"
#include "../btstack_avdtp_source.h"   /* AUDIO_SLOT_COUNT_* */
#include "../../settings.h"

#define A2DP_CODEC_VENDOR_ID_SONY 0x12d
#define A2DP_SONY_CODEC_LDAC      0xaa

#define LDAC_TX_OVERHEAD      18   /* LDACBT_TX_HEADER_SIZE (libldac internal) */
#define LDAC_PAYLOAD_HDR       1   /* our frame-count byte, prepended in send  */
#define LDAC_FALLBACK_PAYLOAD 656  /* used until the media MTU is known        */

/* libldac mtu that makes it pack at most (payload - our header) bytes */
static int ldac_mtu_for_payload(int payload){
    return payload - LDAC_PAYLOAD_HDR + LDAC_TX_OVERHEAD;
}

static HANDLE_LDAC_BT handleLDAC;
static avdtp_stream_endpoint_t *stream_endpoint_ldac;
static bool have_ldac;

static uint8_t media_ldac_codec_capabilities[] = {
        0x2D, 0x1, 0x0, 0x0,
        0xAA, 0,
        0x30,  // 0x20 (44100) | 0x10 (48000)
        0x01,
        0x1
};
static uint8_t local_stream_endpoint_ldac_media_codec_configuration[9];

typedef struct {
    int reconfigure;
    int channel_mode;
    int num_channels;
    int num_samples;
    int sampling_frequency;
} avdtp_media_codec_configuration_ldac_t;
static avdtp_media_codec_configuration_ldac_t ldac_configuration;

/* ---- capability scan ---------------------------------------------------- */

void codec_ldac_note_capability(bool present){ have_ldac = present; }
static bool ldac_available(void){ return have_ldac; }

bool codec_ldac_matches(uint32_t vendor_id, uint16_t codec_id){
    return vendor_id == A2DP_CODEC_VENDOR_ID_SONY && codec_id == A2DP_SONY_CODEC_LDAC;
}

/* ---- quality (persisted: settings()->ldac_quality) ----------------------
   LDAC EQMIDs: HQ = 990 kbps, SQ = 660 (default), MQ = 330. Applied at
   encoder init, i.e. the next LDAC connection/stream setup. */

static int ldac_eqmid(void){
    switch (settings()->ldac_quality){
        case 1:  return LDACBT_EQMID_MQ;   /* 330 kbps */
        case 3:  return LDACBT_EQMID_HQ;   /* 990 kbps */
        default: return LDACBT_EQMID_SQ;   /* 660 kbps (0/2 = zero-fill default) */
    }
}

int codec_ldac_quality_kbps(void){
    switch (settings()->ldac_quality){
        case 1:  return 330;
        case 3:  return 990;
        default: return 660;
    }
}

/* EQMID -> kbps. The two middle entries are the unofficial rungs Sony's ABR
   governor used; the encoder can still report them, so keep the mapping
   complete even though only HQ/SQ/MQ are selectable here. */
static const int ldac_eqmid_kbps[] = { 990, 660, 330, 492, 396 };  /* by EQMID */

int codec_ldac_live_kbps(void){
    if (handleLDAC == NULL) return 0;
    int e = ldacBT_get_eqmid(handleLDAC);
    if (e < 0 || e >= (int)(sizeof(ldac_eqmid_kbps)/sizeof(ldac_eqmid_kbps[0])))
        return 0;
    return ldac_eqmid_kbps[e];
}

/* ---- live quality change (no replug) ------------------------------------
   The EQMID rides in every LDAC frame header, so ldacBT_set_eqmid() retunes
   a RUNNING stream and the sink just follows — the same mechanism Sony's own
   governor uses. The web/console only REQUEST a value; it is applied from
   the pump tick, i.e. the very context that owns the encoder (the fill/
   encode call sites are serialized with it), so no lock is needed and there
   is never a set in flight against an encode. Requests while nothing is
   streaming are dropped — encoder init reads the setting anyway. */
static volatile uint8_t ldac_pending_quality;   /* 0 = nothing requested */

void codec_ldac_request_quality(uint8_t q){
    if (q >= 1 && q <= 3) ldac_pending_quality = q;
}

void codec_ldac_tick(void){
    uint8_t q = ldac_pending_quality;
    if (q == 0) return;
    ldac_pending_quality = 0;
    if (handleLDAC == NULL) return;      /* not streaming: init will apply it */
    if (ldacBT_set_eqmid(handleLDAC, ldac_eqmid()) == 0)
        printf("LDAC quality: %d kbps — applied live\n", codec_ldac_quality_kbps());
    else
        printf("LDAC quality: live retune refused (%d) — applies at the next connection\n",
               ldacBT_get_error_code(handleLDAC));
}

/* ---- helpers ------------------------------------------------------------ */

static int convert_ldac_sampling_frequency(uint8_t frequency_bitmap) {
    switch (frequency_bitmap) {
        case 0x20: return 44100;
        case 0x10: return 48000;
        case 0x08: return 88200;
        case 0x04: return 96000;
        default:
            printf("invalid ldac sampling frequency %d\n", frequency_bitmap);
            return 0;
    }
}

/* ---- negotiation -------------------------------------------------------- */

static uint8_t ldac_configure(void){
    int ladc_num = -1;
    if (num_remote_seps == 0){
        printf("Remote Stream Endpoints not discovered yet, please discover stream endpoints first\n");
        return 1;
    }
    for (int i = 0; i < num_remote_seps; i++){
        if (codec_ldac_matches(remote_seps[i].vendor_id, remote_seps[i].codec_id)){
            PLOG("found LDAC!!! Remote Stream Endpoints ID is %d\n", i);
            selected_remote_sep_index = i;
            ladc_num = i;
            break;
        }
    }
    if (ladc_num < 0){
        printf("not found LDAC!!!\n");
        return 1;
    }

    avdtp_media_codec_type_t codec_type = remote_seps[ladc_num].sep.capabilities.media_codec.media_codec_type;
    if (codec_type != AVDTP_CODEC_NON_A2DP) {
        printf("LDAC codec unmatch!!!\n");
        return 1;
    }

    // - LDAC endpoint (create ONCE — recreating leaked an endpoint-pool slot
    //   per connect until the btstack_assert fired; the other codecs guard)
    if (stream_endpoint_ldac == NULL) {
        stream_endpoint_ldac = a2dp_source_create_stream_endpoint(AVDTP_AUDIO, AVDTP_CODEC_NON_A2DP, (uint8_t *) media_ldac_codec_capabilities, sizeof(media_ldac_codec_capabilities), (uint8_t*) local_stream_endpoint_ldac_media_codec_configuration, sizeof(local_stream_endpoint_ldac_media_codec_configuration));
        btstack_assert(stream_endpoint_ldac != NULL);
        stream_endpoint_ldac->media_codec_configuration_info = local_stream_endpoint_ldac_media_codec_configuration;
        stream_endpoint_ldac->media_codec_configuration_len  = sizeof(local_stream_endpoint_ldac_media_codec_configuration);
        avdtp_source_register_delay_reporting_category(avdtp_local_seid(stream_endpoint_ldac));
    }

    sc.local_stream_endpoint = stream_endpoint_ldac;
    media_tracker.local_seid  = avdtp_local_seid(sc.local_stream_endpoint);
    media_tracker.remote_seid = remote_seps[ladc_num].sep.seid;

    sc.local_stream_endpoint->remote_configuration_bitmap = store_bit16(sc.local_stream_endpoint->remote_configuration_bitmap, AVDTP_MEDIA_CODEC, 1);
    sc.local_stream_endpoint->remote_configuration.media_codec.media_type = AVDTP_AUDIO;
    sc.local_stream_endpoint->remote_configuration.media_codec.media_codec_type = codec_type;

    media_codec_config_data[0] = 0x2D;
    media_codec_config_data[1] = 0x1;
    media_codec_config_data[2] = 0x0;
    media_codec_config_data[3] = 0x0;  // A2DP_LDAC_VENDOR_ID 0x0000012D
    media_codec_config_data[4] = 0xAA;
    media_codec_config_data[5] = 0x0;  // A2DP_LDAC_CODEC_ID 0x00AA
    media_codec_config_data[6] = (settings()->usbset & USBSET_RATE44)
        ? 0x20   // A2DP_LDAC_SAMPLING_FREQ_44100
        : 0x10;  // A2DP_LDAC_SAMPLING_FREQ_48000
    media_codec_config_data[7] = 0x01; // A2DP_LDAC_CHANNEL_MODE_STEREO
    media_codec_config_len = 8;

    avdtp_capabilities_t new_configuration;
    new_configuration.media_codec.media_type = AVDTP_AUDIO;
    new_configuration.media_codec.media_codec_type = remote_seps[selected_remote_sep_index].sep.capabilities.media_codec.media_codec_type;
    new_configuration.media_codec.media_codec_information_len = media_codec_config_len;
    new_configuration.media_codec.media_codec_information = media_codec_config_data;
    int status = avdtp_source_set_configuration(media_tracker.avdtp_cid, media_tracker.local_seid, media_tracker.remote_seid, 1 << AVDTP_MEDIA_CODEC, new_configuration);

    printf("Set LADC Connection Result is %d\n", status);
    return (uint8_t)status;
}

/* ---- configuration accepted -> encoder init ----------------------------- */

void codec_ldac_on_configuration(const uint8_t *packet){
    const uint8_t *codec_info =
        a2dp_subevent_signaling_media_codec_other_configuration_get_media_codec_information(packet);

    ldac_configuration.reconfigure = a2dp_subevent_signaling_media_codec_other_configuration_get_reconfigure(packet);
    ldac_configuration.sampling_frequency = convert_ldac_sampling_frequency(codec_info[6]);
    ldac_configuration.channel_mode = codec_info[7];
    ldac_configuration.num_channels = 2;

    printf("A2DP Source: Received LDAC configuration! Sampling frequency: %d, channel mode: %d channels: %d\n",
            ldac_configuration.sampling_frequency, ldac_configuration.channel_mode, ldac_configuration.num_channels);

    if (handleLDAC != NULL) {          /* reconnect: don't leak the old instance */
        ldacBT_free_handle(handleLDAC);
        handleLDAC = NULL;
    }
    handleLDAC = ldacBT_get_handle();
    if (handleLDAC == NULL) {
        printf("Failed to get LDAC handle\n");
        return;
    }

    // init ldac encoder
    /* MTU handed to libldac. It packs whole LDAC frames up to
         tx_size = min(LDACBT_MTU_REQUIRED - LDACBT_TX_HEADER_SIZE, mtu - 18)
       = 661 by default, so our 1-byte frame-count header makes a 662-byte
       payload and a 674-byte L2CAP packet — 2 bytes OVER the default 672-byte
       MTU, at every LDAC rate. BTstack then refuses the send with
       MEMORY_CAPACITY_EXCEEDED and the packet is silently dropped. The media
       channel is not open yet here, so start from the conservative budget and
       re-derive it in codec_ldac_on_media_open(). */
    int mtu = ldac_mtu_for_payload(LDAC_FALLBACK_PAYLOAD);
    if (ldacBT_init_handle_encode(handleLDAC, mtu, ldac_eqmid(), ldac_configuration.channel_mode,
                LDACBT_SMPL_FMT_S16, ldac_configuration.sampling_frequency) == -1) {
        printf("Couldn't initialize LDAC encoder: %d\n", ldacBT_get_error_code(handleLDAC));
        return;
    }
    // HQ -> audio_timer_interval = 1
    // SQ -> audio_timer_interval <= 5
    // MQ -> audio_timer_interval <= 10
    audio_timer_interval = 1;
    set_current_sample_rate(ldac_configuration.sampling_frequency);
    printf("current LDAC sampling rate is %d \n", get_current_sample_rate());

    audio_slot_queue_configure_with_count(LDACBT_ENC_LSU, AUDIO_SLOT_COUNT_LDAC);

    cur_codec = CODEC_ID_LDAC;

    avdtp_source_open_stream(media_tracker.avdtp_cid, media_tracker.local_seid, media_tracker.remote_seid);
}

/* ---- lifecycle ---------------------------------------------------------- */

static void ldac_release(void){
    if (handleLDAC != NULL) {
        ldacBT_free_handle(handleLDAC);
        handleLDAC = NULL;
    }
}

/* ---- streaming ---------------------------------------------------------- */

static int ldac_fill(a2dp_media_sending_context_t *context) {
    int          total_samples_read                = 0;
    unsigned int num_audio_samples_per_ldac_buffer = LDACBT_ENC_LSU;
    int          consumed;
    int          encoded = 0;
    int          frames;

    // reserve first byte for number of frames
    if (context->codec_storage_count == 0)
        context->codec_storage_count = 1;

    while (context->samples_ready >= num_audio_samples_per_ldac_buffer && encoded == 0) {

        uint8_t slot_idx;
        if (!audio_slot_pop(&slot_idx)) break;

        if (ldacBT_encode(handleLDAC, audio_slot_data(slot_idx), &consumed, &context->codec_storage[context->codec_storage_count], &encoded, &frames) != 0) {
            printf("LDAC encoding error: %d\n", ldacBT_get_error_code(handleLDAC));
        }
        consumed = consumed / (2 * ldac_configuration.num_channels);
        total_samples_read += consumed;
        context->codec_storage_count += encoded;
        context->codec_num_frames += frames;
        context->samples_ready -= consumed;

        audio_slot_release(slot_idx);
    }

    return total_samples_read;
}

static void ldac_send(void) {
    uint8_t num_frames = media_tracker.codec_num_frames;
    media_tracker.codec_storage[0] = num_frames; // frames in first byte

    src_tx_pkts++;
    a2dp_source_note_send(a2dp_source_stream_send_media_payload_rtp(
        media_tracker.avdtp_cid, media_tracker.local_seid, 0, media_tracker.rtp_timestamp,
        &media_tracker.codec_storage[0], media_tracker.codec_storage_count));
    media_tracker.rtp_timestamp += num_frames * LDACBT_ENC_LSU;

    media_tracker.codec_storage_count = 0;
    media_tracker.codec_ready_to_send = 0;
    media_tracker.codec_num_frames = 0;
}

/* The media MTU only exists once the media channel is open, and the encoder
   is configured from the config-accepted event before that. Re-derive the
   packing budget here; nothing has been encoded yet, so a re-init is free. */
void codec_ldac_on_media_open(void){
    if (cur_codec != CODEC_ID_LDAC || handleLDAC == NULL) return;
    int payload = avdtp_max_media_payload_size(media_tracker.avdtp_cid, media_tracker.local_seid);
    if (payload < 160 || payload > 1024) payload = LDAC_FALLBACK_PAYLOAD;
    if (payload == LDAC_FALLBACK_PAYLOAD) return;          /* already set up for this */
    if (ldacBT_init_handle_encode(handleLDAC, ldac_mtu_for_payload(payload), ldac_eqmid(),
                                  ldac_configuration.channel_mode, LDACBT_SMPL_FMT_S16,
                                  ldac_configuration.sampling_frequency) == -1) {
        printf("LDAC: re-init for %d B payload failed (%d) — keeping the safe packing\n",
               payload, ldacBT_get_error_code(handleLDAC));
        return;
    }
    printf("LDAC: media MTU known — packing for %d B payload\n", payload);
}

/* ---- ops ---------------------------------------------------------------- */

const codec_ops_t codec_ldac_ops = {
    .id        = CODEC_ID_LDAC,
    .name      = "LDAC",
    .available = ldac_available,
    .configure = ldac_configure,
    .release   = ldac_release,
    .fill      = ldac_fill,
    .send      = ldac_send,
};
