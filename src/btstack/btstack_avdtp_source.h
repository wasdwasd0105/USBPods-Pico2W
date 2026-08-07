//
// Created by Sean on 8/7/23.
//

#ifndef PICOW_USB_BT_AUDIO_SSP_COUNTER_H
#define PICOW_USB_BT_AUDIO_SSP_COUNTER_H

#include <stdint.h>
#include <stdbool.h>

// Slot queue constants
#define AUDIO_SLOT_COUNT_SBC   16   // SBC: 128 samples/slot, 16*2.67ms = 43ms buffer
#define AUDIO_SLOT_COUNT_AAC   16   // AAC-LC: 1024 samples/slot, 16*21.3ms = 341ms buffer
#define AUDIO_SLOT_COUNT_ELD      6 // AAC-ELD: 480 samples/slot, 6*10ms = 60ms buffer
#define AUDIO_SLOT_COUNT_ELD_320  9 // 320 kbps: bigger AUs need TX-jitter headroom — 90ms
#define AUDIO_SLOT_COUNT_ELD_GAME 4 // gaming mode: latency first — 40ms
#define AUDIO_SLOT_COUNT_LDAC  16   // LDAC: 256 samples/slot, 16*5.3ms = 85ms buffer
#define AUDIO_SLOT_COUNT_MAX   16   // pool size = max of all above. 24 sized the
                                    // pool at 96 KB for a slot shape no codec
                                    // uses (AAC 16x1024 is the real max) —
                                    // trimmed 32 KB to fund the relay AAC
                                    // decoder's configure-time heap (its
                                    // config mallocs failing surfaces as
                                    // endless LATM "decode err 0x1001").
#define AUDIO_SLOT_MAX_SAMPLES 1024
#define AUDIO_SLOT_MAX_INT16   (AUDIO_SLOT_MAX_SAMPLES * 2)  // stereo

void set_usb_sw_gain_pct(uint8_t pct);   /* swvol: software gain on USB PCM */

// Slot queue API (multicore-safe)
void audio_slot_queue_init(void);
void audio_slot_request_flush(bool live);  /* EP-close flush (USB context)   */
void audio_slot_live_close_grace(void);    /* live close: defer close 80 ms  */
void audio_slot_grace_cancel(void);        /* EP reopened inside the grace   */
void a2dp_source_queue_jitter(uint8_t ms);   /* AACP jitter, pushed post-START */
void audio_slot_queue_configure(uint16_t samples_per_slot);
void audio_slot_push_samples(const int16_t *src, uint16_t stereo_pair_count);

/* HCI Flush Occurred events: incremented by the HCI event handler
   (btstack_hci.c), consumed by the source pump as the universal
   link-damage signal that arms the recovery kick. */
extern uint32_t usbpods_flush_occurred_count;
/* Enhanced Flush Complete (0x39) events — our manual jam-breakers; kept
   separate from flush_occurred so automatic-timer flushes are provable
   by event type alone. */
extern uint32_t usbpods_enhanced_flush_done_count;

bool check_is_streaming();

void set_bt_volume(int16_t);

uint8_t get_bt_volume();

bool get_bt_mute();

void set_usb_streaming(bool flag);

bool * get_is_bt_sink_volume_changed_ptr();

int btstack_main(int argc, const char * argv[]);

void avdtp_disconnect_and_scan(void);

void gap_start_scanning(void);

bool get_a2dp_connected_flag();

uint8_t get_cur_codec(void);     /* 0 none, 1 SBC, 2 AAC, 3 LDAC, 4 AAC-ELD */
int8_t  get_avrcp_battery(void); /* AVRCP enum 0-4, -1 = not reported */

void a2dp_source_reconnect();
void a2dp_source_main_work(void);  /* deferred FDK encoder rebuild — main loop only */
void a2dp_source_selfheal_init(void);  /* arm the suspended-stream watchdog (init) */
void a2dp_source_on_aap_wake(void);    /* AAP ear/wake: resume a suspended stream */
void a2dp_source_apply_eld_rate(void); /* live ELD bitrate change (web setting) */
uint16_t a2dp_source_con_handle(void); /* headset ACL handle, 0 = none */
void a2dp_source_note_send(uint8_t rc);      /* count/report a failed media send */
const uint8_t *a2dp_source_peer_addr(void);  /* who is ACTUALLY connected (not the
                                                dial target from the slot) */
bool radio_is_absent(void);   /* no CYW43 on this board — no Bluetooth */
uint32_t a2dp_source_live_kbps(void);  /* on-air rate of the current codec, 0 = unknown */
void bt_link_tx_power(uint16_t con_handle, int8_t dbm); /* CYW43 vendor cap */

void avdtp_source_establish_stream();

void set_next_capablity_and_start_stream();

void start_led_blink();

static int setup_aac_configuration();

static int setup_sbc_configuration();

static int set_ldac_configuration();

bool get_allow_switch_slot();
void a2dp_source_switch_slot(uint8_t want);  /* handover: drop link, dial slot */
void avdtp_disconnect_keep_pairing(void);    /* user disconnect (web/console 'd') */
bool a2dp_user_dc_hold(void);  /* user disconnected — auto-redial suppressed */
void a2dp_user_dc_clear(void);
void a2dp_source_remote_bye(uint16_t handle, uint8_t reason); /* headset powered off */

void core1_aaceld_encoder_loop(void);

void increase_vol_by_key();

void decrease_vol_by_key();

/* ---- console bridge (see console.c) -------------------------------------
   The UI lives in console.c; these are the only doors it needs into the
   A2DP source. a2dp_source_console_key() handles the advanced/debug tier
   (plus any modal it owns) and returns false for keys it does not own. */
bool a2dp_source_console_key(char cmd);
bool a2dp_source_is_playing(void);          /* stream up AND host feeding    */
void a2dp_source_set_debug_mode(bool on);   /* protocol + AACP logging       */
void a2dp_source_console_disconnect(void);  /* drop the headset link         */

#endif //PICOW_USB_BT_AUDIO_SSP_COUNTER_H
