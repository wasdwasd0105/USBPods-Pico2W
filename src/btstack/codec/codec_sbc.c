/*
 * codec_sbc.c — SBC, the mandatory A2DP baseline codec.
 *
 * Encoder: BTstack's built-in SBC encoder. Packet: SBC payload header byte
 * (frame count) + frames; RTP clock advances by frames * samples-per-frame.
 */

#include <stdio.h>
#include <string.h>

#include "codec.h"
#include "../a2dp_engine.h"
#include "../btstack_avdtp_source.h"   /* AUDIO_SLOT_COUNT_* */
#include "../../settings.h"

static avdtp_stream_endpoint_t *stream_endpoint_sbc;

/* ---- capability bitmap type ---- */
typedef struct {
    // bitmaps
    uint8_t sampling_frequency_bitmap;
    uint8_t channel_mode_bitmap;
    uint8_t block_length_bitmap;
    uint8_t subbands_bitmap;
    uint8_t allocation_method_bitmap;
    uint8_t min_bitpool_value;
    uint8_t max_bitpool_value;
} media_codec_information_sbc_t;

/* ---- configuration type ---- */
typedef struct {
    int reconfigure;
    int num_channels;
    int sampling_frequency;
    int block_length;
    int subbands;
    int min_bitpool_value;
    int max_bitpool_value;

    int channel_mode;
    int allocation_method;
} avdtp_media_codec_configuration_sbc_t;

/* ---- encoder state ---- */
static btstack_sbc_encoder_state_t sbc_encoder_state;

/* ---- capability template ---- */
static const uint8_t media_sbc_codec_capabilities[] = {
    0xFF,//(AVDTP_SBC_44100 << 4) | AVDTP_SBC_STEREO,
    0xFF,//(AVDTP_SBC_BLOCK_LENGTH_16 << 4) | (AVDTP_SBC_SUBBANDS_8 << 2) | AVDTP_SBC_ALLOCATION_METHOD_LOUDNESS,
    2, 53
};

/* ---- local config buffer ---- */
static uint8_t local_stream_endpoint_sbc_media_codec_configuration[4];

#define SBC_PAYLOAD_HDR 1   /* A2DP SBC media payload header: frame count */

/* ---- send ---- */
static void a2dp_demo_send_media_packet_sbc(void){
    int num_bytes_in_frame = btstack_sbc_encoder_sbc_buffer_length();
    int bytes_in_storage = media_tracker.codec_storage_count;
    uint8_t num_sbc_frames = bytes_in_storage / num_bytes_in_frame;
    // Prepend SBC Header
    media_tracker.codec_storage[0] = num_sbc_frames;  // (fragmentation << 7) | (starting_packet << 6) | (last_packet << 5) | num_frames;
    src_tx_pkts++; a2dp_source_stream_send_media_payload_rtp(media_tracker.avdtp_cid, media_tracker.local_seid, 0,
                               media_tracker.rtp_timestamp,
                               media_tracker.codec_storage, bytes_in_storage + 1);



    // update rtp_timestamp
    unsigned int num_audio_samples_per_sbc_buffer = btstack_sbc_encoder_num_audio_frames();

    media_tracker.rtp_timestamp += num_sbc_frames * num_audio_samples_per_sbc_buffer;

    media_tracker.codec_storage_count = 0;
    media_tracker.codec_ready_to_send = 0;
}

/* ---- fill ---- */
static int fill_sbc_audio_buffer(a2dp_media_sending_context_t * context){
    int total_num_bytes_read = 0;
    unsigned int num_audio_samples_per_sbc_buffer = btstack_sbc_encoder_num_audio_frames();

    /* RESERVE THE HEADER BYTE. Frames are staged at codec_storage[1 + count]
       and the send ships count + 1 bytes (the A2DP SBC payload header carries
       the frame count), but this bound only checked the frames — so whenever
       the frame size divides max_media_payload_size exactly the packet came
       out ONE byte over the L2CAP MTU and the send failed. 82 B frames hit it
       on 656: stereo / 8 subbands / 16 blocks / bitpool 35, an entirely
       ordinary negotiation. */
    while (context->samples_ready >= num_audio_samples_per_sbc_buffer &&
           (context->max_media_payload_size - SBC_PAYLOAD_HDR - context->codec_storage_count)
               >= btstack_sbc_encoder_sbc_buffer_length()){

        uint8_t slot_idx;
        if (!audio_slot_pop(&slot_idx)) break;

        btstack_sbc_encoder_process_data(audio_slot_data(slot_idx));

        uint16_t sbc_frame_size = btstack_sbc_encoder_sbc_buffer_length();
        uint8_t * sbc_frame = btstack_sbc_encoder_sbc_buffer();

        total_num_bytes_read += num_audio_samples_per_sbc_buffer;

        memcpy(&context->codec_storage[1 + context->codec_storage_count], sbc_frame, sbc_frame_size);
        context->codec_storage_count += sbc_frame_size;
        context->samples_ready -= num_audio_samples_per_sbc_buffer;

        audio_slot_release(slot_idx);
    }

    return total_num_bytes_read;
}

/* ---- capability dump ---- */
static void dump_sbc_capability(media_codec_information_sbc_t media_codec_sbc){
    PLOG("    - sampling_frequency: 0x%02x\n", media_codec_sbc.sampling_frequency_bitmap);
    PLOG("    - channel_mode: 0x%02x\n", media_codec_sbc.channel_mode_bitmap);
    PLOG("    - block_length: 0x%02x\n", media_codec_sbc.block_length_bitmap);
    PLOG("    - subbands: 0x%02x\n", media_codec_sbc.subbands_bitmap);
    PLOG("    - allocation_method: 0x%02x\n", media_codec_sbc.allocation_method_bitmap);
    PLOG("    - bitpool_value [%d, %d] \n", media_codec_sbc.min_bitpool_value, media_codec_sbc.max_bitpool_value);
}

/* ---- configuration dump ---- */
static void dump_sbc_configuration(avdtp_media_codec_configuration_sbc_t configuration){
    PLOG("Received media codec configuration:\n");
    PLOG("    - num_channels: %d\n", configuration.num_channels);
    PLOG("    - sampling_frequency: %d\n", configuration.sampling_frequency);
    PLOG("    - channel_mode: %d\n", configuration.channel_mode);
    PLOG("    - block_length: %d\n", configuration.block_length);
    PLOG("    - subbands: %d\n", configuration.subbands);
    PLOG("    - allocation_method: %d\n", configuration.allocation_method);
    PLOG("    - bitpool_value [%d, %d] \n", configuration.min_bitpool_value, configuration.max_bitpool_value);
}

/* capability event (AVDTP_SUBEVENT_SIGNALING_MEDIA_CODEC_SBC_CAPABILITY) */
void codec_sbc_on_capability(const uint8_t *packet){
    PLOG("CAPABILITY - MEDIA_CODEC: SBC, remote seid %u: \n",
         avdtp_subevent_signaling_media_codec_sbc_capability_get_remote_seid(packet));

    media_codec_information_sbc_t   sbc_capability;
    sbc_capability.sampling_frequency_bitmap = avdtp_subevent_signaling_media_codec_sbc_capability_get_sampling_frequency_bitmap(packet);
    sbc_capability.channel_mode_bitmap = avdtp_subevent_signaling_media_codec_sbc_capability_get_channel_mode_bitmap(packet);
    sbc_capability.block_length_bitmap = avdtp_subevent_signaling_media_codec_sbc_capability_get_block_length_bitmap(packet);
    sbc_capability.subbands_bitmap = avdtp_subevent_signaling_media_codec_sbc_capability_get_subbands_bitmap(packet);
    sbc_capability.allocation_method_bitmap = avdtp_subevent_signaling_media_codec_sbc_capability_get_allocation_method_bitmap(packet);
    sbc_capability.min_bitpool_value = avdtp_subevent_signaling_media_codec_sbc_capability_get_min_bitpool_value(packet);
    sbc_capability.max_bitpool_value = avdtp_subevent_signaling_media_codec_sbc_capability_get_max_bitpool_value(packet);
    dump_sbc_capability(sbc_capability);

}

/* configuration accepted (AVDTP_SUBEVENT_SIGNALING_MEDIA_CODEC_SBC_CONFIGURATION) */
void codec_sbc_on_configuration(const uint8_t *packet){
    printf("Set configuration and init encoder\n");
    avdtp_media_codec_configuration_sbc_t sbc_configuration;
    sbc_configuration.reconfigure = avdtp_subevent_signaling_media_codec_sbc_configuration_get_reconfigure(packet);
    sbc_configuration.num_channels = avdtp_subevent_signaling_media_codec_sbc_configuration_get_num_channels(packet);
    sbc_configuration.sampling_frequency = avdtp_subevent_signaling_media_codec_sbc_configuration_get_sampling_frequency(packet);
    sbc_configuration.block_length = avdtp_subevent_signaling_media_codec_sbc_configuration_get_block_length(packet);
    sbc_configuration.subbands = avdtp_subevent_signaling_media_codec_sbc_configuration_get_subbands(packet);
    sbc_configuration.min_bitpool_value = avdtp_subevent_signaling_media_codec_sbc_configuration_get_min_bitpool_value(packet);
    sbc_configuration.max_bitpool_value = avdtp_subevent_signaling_media_codec_sbc_configuration_get_max_bitpool_value(packet);
    
    avdtp_channel_mode_t channel_mode = (avdtp_channel_mode_t) avdtp_subevent_signaling_media_codec_sbc_configuration_get_channel_mode(packet);
    avdtp_sbc_allocation_method_t allocation_method = (avdtp_sbc_allocation_method_t) avdtp_subevent_signaling_media_codec_sbc_configuration_get_allocation_method(packet);
    
    // Map Bluetooth spec definition to SBC Encoder expected input
    sbc_configuration.allocation_method = (btstack_sbc_allocation_method_t)(((uint8_t) allocation_method) - 1);
    switch (channel_mode){
        case AVDTP_CHANNEL_MODE_JOINT_STEREO:
            sbc_configuration.channel_mode = SBC_CHANNEL_MODE_JOINT_STEREO;
            sbc_configuration.num_channels = 2;
            break;
        case AVDTP_CHANNEL_MODE_STEREO:
            sbc_configuration.channel_mode = SBC_CHANNEL_MODE_STEREO;
            sbc_configuration.num_channels = 2;
            break;
        case AVDTP_CHANNEL_MODE_DUAL_CHANNEL:
            sbc_configuration.channel_mode = SBC_CHANNEL_MODE_DUAL_CHANNEL;
            sbc_configuration.num_channels = 2;
            break;
        case AVDTP_CHANNEL_MODE_MONO:
            sbc_configuration.channel_mode = SBC_CHANNEL_MODE_MONO;
            sbc_configuration.num_channels = 1;
            break;
        default:
            btstack_assert(false);
            break;
    }
    dump_sbc_configuration(sbc_configuration);

    configure_sample_rate(sc.sampling_frequency);
    btstack_sbc_encoder_init(&sbc_encoder_state, SBC_MODE_STANDARD, 
        sbc_configuration.block_length, sbc_configuration.subbands, 
        sbc_configuration.allocation_method, sbc_configuration.sampling_frequency, 
        sbc_configuration.max_bitpool_value,
        sbc_configuration.channel_mode);

    audio_timer_interval = 10;

    audio_slot_queue_configure_with_count(btstack_sbc_encoder_num_audio_frames(), AUDIO_SLOT_COUNT_SBC);

    cur_codec = CODEC_ID_SBC;

    avdtp_source_open_stream(media_tracker.avdtp_cid, media_tracker.local_seid, media_tracker.remote_seid);
}

/* ---- configure ---- */
static uint8_t sbc_configure(void){

    uint8_t  sbc_num = 0;
    for (int i = 0; i < num_remote_seps; i++){
        if ( remote_seps[i].sep.capabilities.media_codec.media_codec_type == AVDTP_CODEC_SBC){
    PLOG("found sbc!!! Remote Stream Endpoints ID is %d\n", i);
    selected_remote_sep_index = i;
    sbc_num = i;
    break;
        }
    }

    // // - SBC (create only once)
    if (stream_endpoint_sbc == NULL) {
        stream_endpoint_sbc = a2dp_source_create_stream_endpoint(AVDTP_AUDIO, AVDTP_CODEC_SBC, (uint8_t *) media_sbc_codec_capabilities, sizeof(media_sbc_codec_capabilities), (uint8_t*) local_stream_endpoint_sbc_media_codec_configuration, sizeof(local_stream_endpoint_sbc_media_codec_configuration));
        btstack_assert(stream_endpoint_sbc != NULL);
        stream_endpoint_sbc->media_codec_configuration_info = local_stream_endpoint_sbc_media_codec_configuration;
        stream_endpoint_sbc->media_codec_configuration_len  = sizeof(local_stream_endpoint_sbc_media_codec_configuration);
    }
    avdtp_source_register_delay_reporting_category(avdtp_local_seid(stream_endpoint_sbc));
    avdtp_set_preferred_sampling_frequency(stream_endpoint_sbc,
        (settings()->usbset & USBSET_RATE44) ? 44100 : 48000);
    /* JOINT stereo, not plain stereo: at the same bitpool it codes the
       correlated part of the image once (M/S per subband) instead of twice,
       so it is strictly better on normal music — and it is what Android,
       macOS and iOS all pick. Plain STEREO also made the 82-byte frame size
       reachable, which is exactly the size that overflowed the payload. */
    avdtp_set_preferred_channel_mode(stream_endpoint_sbc, AVDTP_SBC_JOINT_STEREO);

    // set up local stream_endpoint; need change
    sc.local_stream_endpoint = stream_endpoint_sbc;

    // store local seid
    media_tracker.local_seid  = avdtp_local_seid(sc.local_stream_endpoint);
    media_tracker.remote_seid = remote_seps[sbc_num].sep.seid;


    // choose SBC config params
    const uint8_t * packet = remote_seps[sbc_num].media_codec_event;

    // choose SBC config params
    avdtp_configuration_sbc_t configuration;
    configuration.sampling_frequency = avdtp_choose_sbc_sampling_frequency(sc.local_stream_endpoint, avdtp_subevent_signaling_media_codec_sbc_capability_get_sampling_frequency_bitmap(packet));
    configuration.channel_mode       = avdtp_choose_sbc_channel_mode(sc.local_stream_endpoint, avdtp_subevent_signaling_media_codec_sbc_capability_get_channel_mode_bitmap(packet));
    configuration.block_length       = avdtp_choose_sbc_block_length(sc.local_stream_endpoint, avdtp_subevent_signaling_media_codec_sbc_capability_get_block_length_bitmap(packet));
    configuration.subbands           = avdtp_choose_sbc_subbands(sc.local_stream_endpoint, avdtp_subevent_signaling_media_codec_sbc_capability_get_subbands_bitmap(packet));
    configuration.allocation_method  = avdtp_choose_sbc_allocation_method(sc.local_stream_endpoint, avdtp_subevent_signaling_media_codec_sbc_capability_get_allocation_method_bitmap(packet));
    configuration.max_bitpool_value  = avdtp_choose_sbc_max_bitpool_value(sc.local_stream_endpoint, avdtp_subevent_signaling_media_codec_sbc_capability_get_max_bitpool_value(packet));
    configuration.min_bitpool_value  = avdtp_choose_sbc_min_bitpool_value(sc.local_stream_endpoint, avdtp_subevent_signaling_media_codec_sbc_capability_get_min_bitpool_value(packet));

    // setup SBC configuration
    avdtp_config_sbc_store(media_codec_config_data, &configuration);
    media_codec_config_len = 4;

    avdtp_capabilities_t new_configuration;
    new_configuration.media_codec.media_type = AVDTP_AUDIO;
    new_configuration.media_codec.media_codec_type = remote_seps[selected_remote_sep_index].sep.capabilities.media_codec.media_codec_type ;
    new_configuration.media_codec.media_codec_information_len = media_codec_config_len;
    new_configuration.media_codec.media_codec_information = media_codec_config_data;
    int status = avdtp_source_set_configuration(media_tracker.avdtp_cid, media_tracker.local_seid, media_tracker.remote_seid, 1 << AVDTP_MEDIA_CODEC, new_configuration);
    return status;
}

/* ---- ops ---------------------------------------------------------------- */

static bool sbc_available(void){
    /* SBC is the mandatory baseline: usable iff the scan found an SBC SEP */
    for (int i = 0; i < num_remote_seps; i++)
        if (remote_seps[i].sep.capabilities.media_codec.media_codec_type == AVDTP_CODEC_SBC)
            return true;
    return false;
}

static void sbc_release(void){
    /* BTstack's SBC encoder is static state — nothing to free */
}

const codec_ops_t codec_sbc_ops = {
    .id        = CODEC_ID_SBC,
    .name      = "SBC",
    .available = sbc_available,
    .configure = sbc_configure,
    .release   = sbc_release,
    .fill      = fill_sbc_audio_buffer,
    .send      = a2dp_demo_send_media_packet_sbc,
};
