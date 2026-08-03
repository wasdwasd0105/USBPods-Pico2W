/*
 * codec_aac.c — MPEG AAC-LC (LATM/MCP1) A2DP source codec.
 *
 * Shares the FDK encoder handle with AAC-ELD (codec_fdk.h): only one of the
 * two is ever configured at a time, and each open closes a previous instance
 * first (a leaked ~100 KB FDK instance kills the heap — error 33).
 */

#include <stdio.h>
#include <string.h>
#include "aacenc_lib.h"

#include "codec.h"
#include "codec_fdk.h"
#include "../a2dp_engine.h"
#include "../btstack_avdtp_source.h"
#include "../../settings.h"

static avdtp_stream_endpoint_t *stream_endpoint_aac;

/* ---- caps template + type ---- */
static uint8_t media_aac_codec_capabilities[] = {
        0xF0,
        0xFF,
        0xFC,
        0x80,
        0,
        0
};

typedef struct {
    int reconfigure;
    int sampling_frequency_bitmap;
    int object_type_bitmap;
    int bit_rate;
    int channels_bitmap;
    int vbr;
} avdtp_media_codec_capabilities_aac_t;

/* ---- local config buffer ---- */
static uint8_t local_stream_endpoint_aac_media_codec_configuration[6];

/* ---- bitrate state ----
   There is no tuning constant here any more: the ceiling is PACKET GEOMETRY.
   One AAC-LC access unit is 1024 samples and we ship exactly one per RTP
   payload, so the most a link can carry is
       payload_bytes * 8 * sample_rate / 1024.
   For the classic 656 B payload (the 672 B L2CAP MTU nearly every sink
   negotiates) that is 234 kbps at 48 k and 215 kbps at 44.1 k — which is why
   220 kbps tested as "the most stable rate for most headsets": frames above
   it simply did not fit in a packet. Now we follow whatever the headset
   advertises, bounded only by what its ACTUAL negotiated MTU can carry, so a
   sink with a bigger MTU gets the higher rate it asked for. */
#define AAC_DEFAULT_BIT_RATE 192000   /* used when the sink states no limit */
#define AAC_LATM_HDR_RESERVE 16       /* StreamMuxConfig rides inside every frame
                                         (HEADER_PERIOD 1) on top of the coded
                                         bits; ~8-12 B for LC stereo, 16 keeps a
                                         little slack. The RTP header is already
                                         excluded by avdtp_max_media_payload_size. */
static int aac_bit_rate = AAC_DEFAULT_BIT_RATE;

/* negotiated media payload, or the conservative classic value when the media
   channel is not open yet (encoder init runs before OPEN) */
static int aac_payload_budget(void){
    int m = avdtp_max_media_payload_size(media_tracker.avdtp_cid, media_tracker.local_seid);
    if (m < 160 || m > 1024) m = 656;
    return m;
}
static int aac_bitrate_ceiling(uint32_t sample_rate){
    int payload = aac_payload_budget() - AAC_LATM_HDR_RESERVE;
    if (payload < 128) payload = 128;
    if (sample_rate == 0) sample_rate = 48000;
    return (int)(((uint64_t) payload * 8u * sample_rate) / 1024u);
}
/* Object types the SINK advertised (A2DP bitmap: 0x80 MPEG-2 AAC LC,
   0x40 MPEG-4 AAC LC, 0x20 LTP, 0x10 scalable). Kept from the capability
   scan because SET_CONFIGURATION must name one the sink actually supports —
   a hardcoded 0x80 is rejected by sinks that only offer MPEG-4 LC (HW: a
   headset advertising object type 64 fell back to SBC). */
static uint8_t aac_object_type_bitmap;
static uint16_t aac_sink_freq_bitmap;
static uint8_t  aac_sink_ch_bitmap;
static uint8_t  aac_sink_vbr;   /* sink's VBR capability (we send CBR anyway) */

/* ---- send ---- */
static void a2dp_demo_send_media_packet_aac(void) {
    uint8_t num_frames = 1;

    src_tx_pkts++; a2dp_source_stream_send_media_payload_rtp(media_tracker.avdtp_cid, media_tracker.local_seid, 0, media_tracker.rtp_timestamp, &media_tracker.codec_storage[0], media_tracker.codec_storage_count);

    //media_tracker.rtp_timestamp += aacinf.frameLength;
    media_tracker.rtp_timestamp += rtp_samples_per_frame;

    media_tracker.codec_storage_count = 0;
    media_tracker.codec_ready_to_send = 0;
    media_tracker.codec_num_frames = 0;

}

/* ---- object-type helper ---- */
static int convert_aac_object_type(int bitmap) {
    switch (bitmap) {
        case AVDTP_AAC_MPEG4_SCALABLE:
    return AOT_AAC_SCAL;
        case AVDTP_AAC_MPEG4_LTP:
    return AOT_AAC_LTP;
        case AVDTP_AAC_MPEG4_LC:
    return AOT_AAC_LC;
        case AVDTP_AAC_MPEG2_LC:
    // https://lists.freedesktop.org/archives/gstreamer-commits/2016-September/096332.html
    return AOT_AAC_LC;
        default:
    printf("invalid aac aot config %d\n", bitmap);
    return 0;
    }
}

/* ---- vbr helper ---- */
static int convert_aac_vbr(int vbr) {
    if (vbr)
        return 4;
    else
        return 0;
}

/* capability event parse (caching stays in the transport) */
void codec_aac_on_capability(const uint8_t *packet){
    uint8_t remote_seid = a2dp_subevent_signaling_media_codec_mpeg_aac_capability_get_remote_seid(packet);
    (void)remote_seid;
    PLOG("CAPABILITY - MEDIA_CODEC: MPEG AAC, remote seid %u: \n", remote_seid);

    avdtp_media_codec_capabilities_aac_t aac_capabilities;
    aac_capabilities.sampling_frequency_bitmap = a2dp_subevent_signaling_media_codec_mpeg_aac_capability_get_sampling_frequency_bitmap(packet);
    aac_capabilities.object_type_bitmap = a2dp_subevent_signaling_media_codec_mpeg_aac_capability_get_object_type_bitmap(packet);
    aac_capabilities.channels_bitmap = a2dp_subevent_signaling_media_codec_mpeg_aac_capability_get_channels_bitmap(packet);
    aac_capabilities.bit_rate = a2dp_subevent_signaling_media_codec_mpeg_aac_capability_get_bit_rate(packet);
    aac_capabilities.vbr = a2dp_subevent_signaling_media_codec_mpeg_aac_capability_get_vbr(packet);
    PLOG("A2DP Source: Received AAC capabilities! Sampling frequency bitmap: 0x%04x, object type %u, channel mode %u, bitrate %u, vbr: %u\n",
           aac_capabilities.sampling_frequency_bitmap, aac_capabilities.object_type_bitmap, aac_capabilities.channels_bitmap,
           aac_capabilities.bit_rate, aac_capabilities.vbr);
    /* The sink's bit_rate field is its MAXIMUM, and 0 means "unspecified" —
       NOT "zero bitrate". Echoing that 0 back in SET_CONFIGURATION gets the
       whole thing rejected (HW: a ROSE OpenFeel advertises bitrate 0 and
       answered "Rejected AVDTP_SI_SET_CONFIGURATION"). Keep our own default
       when the sink states no limit, and never ask for more than it allows. */
    aac_bit_rate = (aac_capabilities.bit_rate > 0) ? (int) aac_capabilities.bit_rate
                                                   : AAC_DEFAULT_BIT_RATE;
    aac_object_type_bitmap = (uint8_t) aac_capabilities.object_type_bitmap;
    aac_sink_freq_bitmap   = (uint16_t) aac_capabilities.sampling_frequency_bitmap;
    aac_sink_ch_bitmap     = (uint8_t) aac_capabilities.channels_bitmap;
    aac_sink_vbr           = (uint8_t) aac_capabilities.vbr;
}

/* configuration accepted -> encoder init */
void codec_aac_on_configuration(const uint8_t *packet){
    printf("Set configuration and init encoder\n");

    if(!finish_setup_aac){
        return;
    }
    avdtp_configuration_mpeg_aac_t aac_configuration;
    // avdtp_media_codec_configuration_aac_t aac_configuration;
    aac_configuration.sampling_frequency = a2dp_subevent_signaling_media_codec_mpeg_aac_configuration_get_sampling_frequency(
            packet);
    aac_configuration.object_type = a2dp_subevent_signaling_media_codec_mpeg_aac_configuration_get_object_type(
            packet);
    aac_configuration.channels = a2dp_subevent_signaling_media_codec_mpeg_aac_configuration_get_num_channels(
            packet);
    aac_configuration.bit_rate = a2dp_subevent_signaling_media_codec_mpeg_aac_configuration_get_bit_rate(
            packet);
    aac_configuration.vbr = a2dp_subevent_signaling_media_codec_mpeg_aac_configuration_get_vbr(packet);

    PLOG("A2DP Source: Received AAC configuration! Sampling frequency: %u, object type %u, channel mode %u, bitrate %u, vbr: %u\n",
           aac_configuration.sampling_frequency, aac_configuration.object_type, aac_configuration.channels,
           aac_configuration.bit_rate, aac_configuration.vbr);

           int aot = convert_aac_object_type(aac_configuration.object_type);

           int vbr = convert_aac_vbr(aac_configuration.vbr);

           //aac_configuration.bit_rate = aac_bit_rate;

    aac_configuration.bit_rate = aac_bit_rate;
    {   int ceiling = aac_bitrate_ceiling(aac_configuration.sampling_frequency);
        if (aac_configuration.bit_rate > ceiling) {
            printf("AAC: sink asked %d bps, but one AU must fit %d B of payload -> %d bps\n",
                   aac_configuration.bit_rate, aac_payload_budget(), ceiling);
            aac_configuration.bit_rate = ceiling;
        }
    }


    //init encoder
    AACENC_ERROR err;
    // Close any previous encoder (ELD from an AirPods session, or a
    // reconnect) — aacEncOpen on a live handle allocates a SECOND
    // instance and leaks the first (~100 KB) until the heap dies
    // with AACENC_MEMORY_ERROR (33). The ELD path already did this.
    if (handleAAC != NULL) {
        aacEncClose(&handleAAC);
        handleAAC = NULL;
    }
    if ((err = aacEncOpen(&handleAAC, 0x01, aac_configuration.channels)) != AACENC_OK) {
        printf("Couldn't open AAC encoder: %d\n", err);
        return;
    }
    if ((err = aacEncoder_SetParam(handleAAC, AACENC_AOT, 2)) != AACENC_OK) {
        printf("Couldn't set audio object type: %d\n", err);
        return;
    }
    src_full_bitrate = aac_configuration.bit_rate;   /* ladder restore point */
    src_relay_rate_low = false;
    if ((err = aacEncoder_SetParam(handleAAC, AACENC_BITRATE, aac_configuration.bit_rate)) != AACENC_OK) {
        printf("Couldn't set bitrate: %d\n", err);
        return;
    }
    if ((err = aacEncoder_SetParam(handleAAC, AACENC_SAMPLERATE, aac_configuration.sampling_frequency)) !=
        AACENC_OK) {
        printf("Couldn't set sampling rate: %d\n", err);
        return;
    }
    if ((err = aacEncoder_SetParam(handleAAC, AACENC_CHANNELMODE, aac_configuration.channels)) != AACENC_OK) {
        printf("Couldn't set channel mode: %d\n", err);
        return;
    }
    if (aac_configuration.vbr) {
        if ((err = aacEncoder_SetParam(handleAAC, AACENC_BITRATEMODE, vbr)) != AACENC_OK) {
            printf("Couldn't set VBR bitrate mode %u: %d\n", aac_configuration.vbr, err);
            return;
        }
        src_vbr_mode = (uint8_t)vbr;    /* ladder restore point */
    } else {
        if ((err = aacEncoder_SetParam(handleAAC, AACENC_BITRATEMODE, 0)) != AACENC_OK) {
            printf("Couldn't set CBR bitrate mode: %d\n", err);
            return;
        }
        src_vbr_mode = 0;
    }
    /* FRAME-SIZE CEILING. AACENC_PEAK_BITRATE is what gives FDK a
       maxBitsPerFrame (aacenc_lib.cpp: the bit-reservoir switch uses
       userPeakBitrate for CBR *and* VBR, and in VBR it also steps the mode
       down when the peak is lower than the mode would need). Without it a
       peak AU is unbounded and can exceed max_media_payload_size (656 B),
       producing an oversized RTP payload that some sinks handle badly —
       the AAC-ELD path has always set this; AAC-LC never did. */
    if ((err = aacEncoder_SetParam(handleAAC, AACENC_PEAK_BITRATE,
                                   aac_configuration.bit_rate)) != AACENC_OK) {
        printf("Couldn't set AAC peak bitrate: %d\n", err);
        return;
    }
    if ((err = aacEncoder_SetParam(handleAAC, AACENC_AFTERBURNER, false)) != AACENC_OK) {
        printf("Couldn't enable afterburner: %d\n", err);
        return;
    }
    if ((err = aacEncoder_SetParam(handleAAC, AACENC_TRANSMUX, TT_MP4_LATM_MCP1)) != AACENC_OK) {
        printf("Couldn't enable LATM transport type: %d\n", err);
        return;
    }
    if ((err = aacEncoder_SetParam(handleAAC, AACENC_HEADER_PERIOD, 1)) != AACENC_OK) {
        printf("Couldn't set LATM header period: %d\n", err);
        return;
    }
    if ((err = aacEncoder_SetParam(handleAAC, AACENC_AUDIOMUXVER, 1)) != AACENC_OK) {
        printf("Couldn't set LATM version: %d\n", err);
        return;
    }
    if ((err = aacEncEncode(handleAAC, NULL, NULL, NULL, NULL)) != AACENC_OK) {
        printf("Couldn't initialize AAC encoder: %d\n", err);
        return;
    }
    if ((err = aacEncInfo(handleAAC, &aacinf)) != AACENC_OK) {
        printf("Couldn't get encoder info: %d\n", err);
        return;
    }
    set_current_sample_rate(aac_configuration.sampling_frequency);
    rtp_samples_per_frame = acc_num_simples;
    eld_resample = false;
    printf("AAC setup complete: mode %s, %lu bps, peak cap %d bps, max AU %u B\n",
           src_vbr_mode ? "VBR" : "CBR",
           (unsigned long) aacEncoder_GetParam(handleAAC, AACENC_BITRATE),
           aac_configuration.bit_rate, (unsigned) aacinf.maxOutBufBytes);

    audio_timer_interval = aac_audio_timer_interval;

    audio_slot_queue_configure_with_count(acc_num_simples, AUDIO_SLOT_COUNT_AAC);

    cur_codec = CODEC_ID_AAC;

    avdtp_source_open_stream(media_tracker.avdtp_cid, media_tracker.local_seid, media_tracker.remote_seid);

}

/* ---- configure ---- */
static uint8_t aac_configure(void){

    #ifdef HAVE_AAC_FDK
    // - AAC
    if (stream_endpoint_aac == NULL) {
        stream_endpoint_aac = a2dp_source_create_stream_endpoint(AVDTP_AUDIO, AVDTP_CODEC_MPEG_2_4_AAC, (uint8_t *) media_aac_codec_capabilities, sizeof(media_aac_codec_capabilities), (uint8_t*) local_stream_endpoint_aac_media_codec_configuration, sizeof(local_stream_endpoint_aac_media_codec_configuration));
        btstack_assert(stream_endpoint_aac != NULL);
        stream_endpoint_aac->media_codec_configuration_info = local_stream_endpoint_aac_media_codec_configuration;
        stream_endpoint_aac->media_codec_configuration_len  = sizeof(local_stream_endpoint_aac_media_codec_configuration);
        avdtp_source_register_delay_reporting_category(avdtp_local_seid(stream_endpoint_aac));
    }
    #endif

    uint8_t  aac_num = 0;
    for (int i = 0; i < num_remote_seps; i++){
        if ( remote_seps[i].sep.capabilities.media_codec.media_codec_type == AVDTP_CODEC_MPEG_2_4_AAC){
    PLOG("found aac!!! Remote Stream Endpoints ID is %d\n", i);
    selected_remote_sep_index = i;
    aac_num = i;
    break;
        }
    }

    // set up local stream_endpoint;
    sc.local_stream_endpoint = stream_endpoint_aac;

    // store local seid
    media_tracker.local_seid  = avdtp_local_seid(sc.local_stream_endpoint);
    media_tracker.remote_seid = remote_seps[aac_num].sep.seid;

    /* OBJECT TYPE — mind BTstack's shift. The capability event reports
       media_codec_information[0] >> 1, so the bitmap we hold is ALREADY
       shifted: bit 0x40 there means MPEG-2 AAC LC and 0x20 means MPEG-4 AAC
       LC (avdtp_util.c maps exactly those). Reading it as raw A2DP bits made
       us answer a sink advertising 0x40 ("MPEG-2 LC") with MPEG-4 LC, and a
       strict one refused with A2DP error 0xD7 NOT_SUPPORTED_OBJECT_TYPE
       (HW: ROSE OpenFeel; a lenient headset had accepted it and hidden the
       mistake). Pick the enum and let avdtp_config_mpeg_aac_store encode the
       byte — it already does 1 << (7 - (object_type - AVDTP_AAC_MPEG2_LC)).
       Only the LC flavours are usable: the encoder is plain AAC-LC. */
    avdtp_aac_object_type_t obj;
    if      (aac_object_type_bitmap & 0x40) obj = AVDTP_AAC_MPEG2_LC;
    else if (aac_object_type_bitmap & 0x20) obj = AVDTP_AAC_MPEG4_LC;
    else {
        printf("AAC: sink offers no LC object type (bitmap 0x%02x) — skipping AAC\n",
               aac_object_type_bitmap);
        return ERROR_CODE_UNSUPPORTED_FEATURE_OR_PARAMETER_VALUE;
    }

    // setup MPEG AAC configuration
    avdtp_configuration_mpeg_aac_t configuration;
    memset(&configuration, 0, sizeof(configuration));   /* drc etc: the struct
                                                           was uninitialised
                                                           stack, and drc rides
                                                           in config[0] bit 0 */
    configuration.object_type = obj;
    configuration.sampling_frequency = (settings()->usbset & USBSET_RATE44) ? 44100 : 48000;
    configuration.channels = 2;

    /* belt and braces: aac_bit_rate is already sanitised at capability time,
       but SET_CONFIGURATION must never carry 0 (sinks reject it outright) */
    configuration.bit_rate = (aac_bit_rate > 0) ? aac_bit_rate : AAC_DEFAULT_BIT_RATE;
    {   int ceiling = aac_bitrate_ceiling(configuration.sampling_frequency);
        if (configuration.bit_rate > ceiling) configuration.bit_rate = ceiling;
    }


    /* CBR, like Android's A2DP AAC encoder (VBR is opt-in there and gated on
       the sink advertising it). Two reasons this matters here:
       - FDK DISCARDS AACENC_BITRATE in VBR mode and substitutes the VBR
         table value, so the bitrate we so carefully negotiate had no effect
         and we could exceed the maximum the sink declared;
       - VBR frame sizes are unbounded without a peak cap, and a peak AU can
         overflow the 656 B payload budget -> oversized RTP packet.
       Declaring vbr = 0 keeps the SET_CONFIGURATION honest about what we
       actually send. (The sink's own VBR capability is kept in
       aac_sink_vbr if VBR is ever offered as a user option.) */
    configuration.vbr = 0;

    avdtp_config_mpeg_aac_store(media_codec_config_data, &configuration);
    media_codec_config_len = 6;

    avdtp_capabilities_t new_configuration;
    new_configuration.media_codec.media_type = AVDTP_AUDIO;
    new_configuration.media_codec.media_codec_type =  2,    // Media Codec Type: A2DP_MEDIA_CT_AAC
    new_configuration.media_codec.media_codec_information_len = media_codec_config_len;
    new_configuration.media_codec.media_codec_information = media_codec_config_data;
    printf("AAC config bytes:");
    for (int ci = 0; ci < media_codec_config_len; ci++) printf(" %02x", media_codec_config_data[ci]);
    printf("  (sink caps: obj 0x%02x, freq 0x%04x, ch 0x%02x, br %d)\n",
           aac_object_type_bitmap, aac_sink_freq_bitmap, aac_sink_ch_bitmap, aac_bit_rate);
    int status = avdtp_source_set_configuration(media_tracker.avdtp_cid, media_tracker.local_seid, media_tracker.remote_seid, 1 << AVDTP_MEDIA_CODEC, new_configuration);
    printf("Set AAC Connection Result is %d\n", status);
    finish_setup_aac = true;

    return status;
}

/* On-air rate for the status line. Under VBR, FDK's GetParam(AACENC_BITRATE)
   can return garbage (HW: 35127 kbps on the ROSE at VBR/165k) — trust it only
   when plausible, else report the negotiated target from the sink caps. */
int codec_aac_live_kbps(void){
    if (handleAAC == NULL) return 0;
    uint32_t kb = (uint32_t)aacEncoder_GetParam(handleAAC, AACENC_BITRATE) / 1000u;
    if (kb >= 32 && kb <= 1000) return (int)kb;
    return aac_bit_rate / 1000;
}

/* MTU IS ONLY KNOWN AFTER THE MEDIA CHANNEL OPENS.
   The encoder is configured from the SET_CONFIGURATION-accepted event, which
   fires BEFORE the media L2CAP channel exists — so avdtp_max_media_payload_size
   has nothing to report and the ceiling falls back to the conservative 656 B
   (HW: a link was clamped to 215 kbps even though the sink asked for 233 k).
   Once the media channel is up the real MTU is available, so re-apply the
   rate. Nothing has been encoded yet at this point, so FDK re-initialising on
   the next aacEncEncode is free. */
void codec_aac_on_media_open(void){
    if (cur_codec != CODEC_ID_AAC || handleAAC == NULL) return;
    int ceiling = aac_bitrate_ceiling(get_current_sample_rate());
    int want    = (aac_bit_rate > 0) ? aac_bit_rate : AAC_DEFAULT_BIT_RATE;
    if (want > ceiling) want = ceiling;
    uint32_t now = aacEncoder_GetParam(handleAAC, AACENC_BITRATE);
    printf("AAC: media open — negotiated payload %d B, ceiling %d bps, sink wants %d bps\n",
           aac_payload_budget(), ceiling, aac_bit_rate);
    if ((uint32_t) want == now) return;
    if (aacEncoder_SetParam(handleAAC, AACENC_BITRATE, want) == AACENC_OK &&
        aacEncoder_SetParam(handleAAC, AACENC_PEAK_BITRATE, want) == AACENC_OK) {
        src_full_bitrate = want;      /* relay ladder restore point */
        printf("AAC: media MTU known (%d B payload) -> %d bps\n",
               aac_payload_budget(), want);
    }
}

/* ---- ops ---------------------------------------------------------------- */

static bool aac_available(void){
    for (int i = 0; i < num_remote_seps; i++)
        if (remote_seps[i].sep.capabilities.media_codec.media_codec_type == AVDTP_CODEC_MPEG_2_4_AAC)
            return true;
    return false;
}

const codec_ops_t codec_aac_ops = {
    .id        = CODEC_ID_AAC,
    .name      = "AAC",
    .available = aac_available,
    .configure = aac_configure,
    .release   = codec_fdk_release,      /* shared FDK handle */
    .fill      = codec_fdk_fill,         /* shared AAC/ELD fill path */
    .send      = a2dp_demo_send_media_packet_aac,
};
