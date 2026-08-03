/*
 * codec_aaceld.c — Apple AAC-ELD (vendor 0x004C, codec 0x8001), the AirPods
 * lossless-latency codec and this firmware's flagship path.
 *
 * This file owns the codec IDENTITY: capability template, endpoint, remote
 * matching and SET_CONFIGURATION. The ELD engine core (FDK encoder init with
 * VBR/SBR policy, the shared fill, the send path with its RTP ms-clock, hole
 * accounting, txchk validator and recovery arming) is deliberately hosted in
 * the transport engine (btstack_avdtp_source.c): it is fused with the slot
 * queue, resampler, relay ladder and self-healing machinery that three days
 * of hardware campaigns tuned. The seams are codec_fdk.h + a2dp_engine.h;
 * the ops table below is the single point of dispatch either way.
 */

#include <stdio.h>
#include <string.h>

#include "codec.h"
#include "codec_fdk.h"
#include "../a2dp_engine.h"
#include "../btstack_avdtp_source.h"
#include "../../settings.h"

#define A2DP_CODEC_VENDOR_ID_APPLE 0x004C
#define A2DP_APPLE_CODEC_AAC_ELD   0x8001

static avdtp_stream_endpoint_t *stream_endpoint_aaceld;
static bool have_aaceld;

void codec_aaceld_note_capability(bool present){ have_aaceld = present; }
static bool aaceld_available(void){ return have_aaceld; }

bool codec_aaceld_matches(uint32_t vendor_id, uint16_t codec_id){
    return vendor_id == A2DP_CODEC_VENDOR_ID_APPLE && codec_id == A2DP_APPLE_CODEC_AAC_ELD;
}

/* ---- caps template ---- */
// Apple AAC-ELD vendor codec capabilities (vendor_id=0x004C, codec_id=0x8001)
// Format: vendor_id(4 LE) + codec_id(2 LE) + obj_type(2) + freq+ch(2) + rsvd(1) + vbr+bitrate(3)
static uint8_t media_aaceld_codec_capabilities[] = {
        0x4C, 0x00, 0x00, 0x00,   // vendor_id = 0x004C (Apple)
        0x01, 0x80,                // codec_id  = 0x8001
        0x00, 0x80,                // object_type = 0x0080 (AAC-ELD)
        0x00, 0x8C,                // freq bitmap 0x008 (48kHz) + channels 0xC (mono+stereo)
        0x00,                      // reserved
        0x83, 0xE8, 0x00           // VBR=1, bitrate=256000
};

/* ---- local config buffer ---- */
static uint8_t local_stream_endpoint_aaceld_media_codec_configuration[14];



/* ---- configure ---- */
static uint8_t aaceld_configure(void){
    int aaceld_num = -1;
    if (num_remote_seps == 0){
        printf("Remote Stream Endpoints not discovered yet\n");
        return -1;
    }
    for (int i = 0; i < num_remote_seps; i++){
        if (remote_seps[i].vendor_id == A2DP_CODEC_VENDOR_ID_APPLE && remote_seps[i].codec_id == A2DP_APPLE_CODEC_AAC_ELD){
            PLOG("found Apple AAC-ELD!!! Remote Stream Endpoints ID is %d\n", i);
            selected_remote_sep_index = i;
            aaceld_num = i;
            break;
        }
    }

    if (aaceld_num < 0){
        printf("not found Apple AAC-ELD!!!\n");
        return -1;
    }

    avdtp_media_codec_type_t codec_type = remote_seps[aaceld_num].sep.capabilities.media_codec.media_codec_type;
    if (codec_type != AVDTP_CODEC_NON_A2DP) {
        printf("AAC-ELD codec type mismatch!!!\n");
        return -1;
    }

    // Create AAC-ELD stream endpoint (only once)
    if (stream_endpoint_aaceld == NULL) {
        stream_endpoint_aaceld = a2dp_source_create_stream_endpoint(
            AVDTP_AUDIO, AVDTP_CODEC_NON_A2DP,
            (uint8_t *) media_aaceld_codec_capabilities, sizeof(media_aaceld_codec_capabilities),
            (uint8_t*) local_stream_endpoint_aaceld_media_codec_configuration,
            sizeof(local_stream_endpoint_aaceld_media_codec_configuration));
        btstack_assert(stream_endpoint_aaceld != NULL);
        stream_endpoint_aaceld->media_codec_configuration_info = local_stream_endpoint_aaceld_media_codec_configuration;
        stream_endpoint_aaceld->media_codec_configuration_len  = sizeof(local_stream_endpoint_aaceld_media_codec_configuration);
        avdtp_source_register_delay_reporting_category(avdtp_local_seid(stream_endpoint_aaceld));
    }

    sc.local_stream_endpoint = stream_endpoint_aaceld;

    media_tracker.local_seid  = avdtp_local_seid(sc.local_stream_endpoint);
    media_tracker.remote_seid = remote_seps[aaceld_num].sep.seid;

    sc.local_stream_endpoint->remote_configuration_bitmap = store_bit16(sc.local_stream_endpoint->remote_configuration_bitmap, AVDTP_MEDIA_CODEC, 1);
    sc.local_stream_endpoint->remote_configuration.media_codec.media_type = AVDTP_AUDIO;
    sc.local_stream_endpoint->remote_configuration.media_codec.media_codec_type = codec_type;

    // Build config: vendor_id(4 LE) + codec_id(2 LE) + params(8)
    media_codec_config_data[0]  = 0x4C;  // vendor_id LE
    media_codec_config_data[1]  = 0x00;
    media_codec_config_data[2]  = 0x00;
    media_codec_config_data[3]  = 0x00;
    media_codec_config_data[4]  = 0x01;  // codec_id LE
    media_codec_config_data[5]  = 0x80;
    media_codec_config_data[6]  = 0x00;  // object_type: AAC-ELD
    media_codec_config_data[7]  = 0x80;
    media_codec_config_data[8]  = 0x00;  // sampling_freq 48kHz (0x008) + channels stereo (0x4)
    media_codec_config_data[9]  = 0x84;
    media_codec_config_data[10] = 0x00;  // reserved
    media_codec_config_data[11] = 0x83;  // VBR=1, bitrate=256000
    media_codec_config_data[12] = 0xE8;
    media_codec_config_data[13] = 0x00;
    media_codec_config_len = 14;

    avdtp_capabilities_t new_configuration;
    new_configuration.media_codec.media_type = AVDTP_AUDIO;
    new_configuration.media_codec.media_codec_type = codec_type;
    new_configuration.media_codec.media_codec_information_len = media_codec_config_len;
    new_configuration.media_codec.media_codec_information = media_codec_config_data;
    int status = avdtp_source_set_configuration(media_tracker.avdtp_cid, media_tracker.local_seid, media_tracker.remote_seid, 1 << AVDTP_MEDIA_CODEC, new_configuration);

    if (status) printf("Set Apple AAC-ELD Connection FAILED: %d\n", status);
    else        PLOG("Set Apple AAC-ELD Connection Result is 0\n");
    return status;
}

/* ---- ops ---------------------------------------------------------------- */

const codec_ops_t codec_aaceld_ops = {
    .id        = CODEC_ID_AACELD,
    .name      = "AAC-ELD",
    .available = aaceld_available,
    .configure = aaceld_configure,
    .release   = codec_fdk_release,       /* shared FDK handle */
    .fill      = codec_fdk_fill,          /* engine-hosted shared fill */
    .send      = a2dp_source_eld_send,    /* engine-hosted (transport-fused) */
};
