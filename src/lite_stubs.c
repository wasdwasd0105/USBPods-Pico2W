/*
 * lite_stubs.c — the open-source (Lite) build's stand-ins for the private
 * modules: the WebHID/WebUSB config channel (webhid_cmd.c), the AirPods
 * AACP client (btstack_aap.c) and the license system (license.c).
 *
 * Compiled ONLY when USBPODS_LITE (CMake picks this file OR the real
 * modules — never both). Everything here is a no-op with the semantics
 * "feature absent", states the callers already handle: no AAP channel
 * (battery 0xFF = none, game mode off), no license (state 0 = none),
 * feature-report GETs stall so a probing host sees an empty channel.
 */
#include <stdint.h>
#include <stdbool.h>
#include <string.h>

#include "btstack/btstack_aap.h"
#include "btstack/btstack_sink_relay.h"

/* ---- WebHID/WebUSB config channel (HID feature-report callbacks, uac.c) - */

uint16_t webhid_get_report(uint8_t report_id, uint8_t *buf, uint16_t reqlen){
    (void)report_id; (void)buf; (void)reqlen;
    return 0;                              /* 0 = stall the GET */
}
void webhid_set_report(uint8_t report_id, const uint8_t *buf, uint16_t len){
    (void)report_id; (void)buf; (void)len;
}
void webhid_task(void){}

/* ---- A2DP phone sink relay (btstack_sink_relay.c) ------------------------
   No sink SEP/SDP is registered, so a phone can never connect: every query
   answers "no phone", the mixer adds nothing, and the pump keeps its full
   TX budget (the relay coex caps never engage). */

void bt_sink_relay_init(void){}
bool bt_sink_relay_is_phone_addr(const uint8_t *addr){ (void)addr; return false; }
bool bt_sink_relay_owns_cid(uint16_t avdtp_cid){ (void)avdtp_cid; return false; }
void bt_sink_relay_mix(int16_t *buf, uint16_t stereo_frames){ (void)buf; (void)stereo_frames; }
bool bt_sink_relay_connected(void){ return false; }
bool bt_sink_relay_streaming(void){ return false; }
bool bt_sink_relay_work(void){ return false; }          /* nothing queued, ever */
uint32_t bt_sink_relay_conn_age_ms(void){ return UINT32_MAX; }
uint16_t bt_sink_relay_con_handle(void){ return 0; }
void bt_sink_relay_set_vol_pct(uint8_t pct){ (void)pct; }
bool bt_sink_relay_dial(const uint8_t *mac){ (void)mac; return false; }
void bt_sink_relay_disconnect(void){}
void bt_sink_relay_dump(void){}
void bt_sink_relay_toggle_aac(void){}
void bt_sink_relay_sync_codec_pref(void){}
void bt_sink_relay_refresh_aac_caps(void){}

/* ---- license system ----------------------------------------------------- */

void     license_init(void){}
void     license_task(void){}
void     license_recheck(void){}
void     license_unbind_boot_check(bool was_power_cycle){ (void)was_power_cycle; }
uint8_t  license_state(void){ return 0; }          /* LICENSE_NONE */
uint8_t  license_key_id(void){ return 0; }
uint32_t license_features(void){ return 0; }
void     license_device_id(uint8_t out[22]){ memset(out, 0, 22); }

/* ---- AirPods AACP client ------------------------------------------------ */

void aap_connect(bd_addr_t addr){ (void)addr; }
void aap_disconnect(void){}
void aap_set_jitter_raw(uint8_t value){ (void)value; }
void aap_cycle_jitter(void){}
void aap_probe_4c(void){}
void aap_send_09_03(uint8_t v){ (void)v; }
void aap_get_battery(uint8_t out[3]){ memset(out, 0xFF, 3); }   /* 0xFF = none */
bool aap_channel_open(void){ return false; }
bool aap_protocol_seen(void){ return false; }
uint8_t aap_get_noise_mode(void){ return 0; }      /* unknown */
bool aap_get_ear_pause(void){ return false; }
bool aap_get_game(void){ return false; }
int  aap_get_pvol(void){ return -1; }              /* unsupported */
int  aap_get_ca(void){ return -1; }
int  aap_get_aa(void){ return -1; }
void aap_set_noise_mode(uint8_t mode){ (void)mode; }
void aap_set_ear_pause(bool on){ (void)on; }
void aap_set_game(bool on){ (void)on; }
void aap_set_pvol(bool on){ (void)on; }
void aap_set_ca(bool on){ (void)on; }
void aap_set_aa(uint8_t strength){ (void)strength; }
uint8_t aap_game_jitter(void){ return 90; }        /* full: settings game_level */
uint32_t aap_last_lock_ms(void){ return 0; }       /* never held the BT lock */
volatile bool aap_rx_log = false;                  /* 'A' key raw-frame logging */

