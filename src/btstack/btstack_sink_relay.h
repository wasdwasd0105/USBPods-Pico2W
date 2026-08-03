/* BT sink relay — phone -> Pico -> headset (port of the ESP32 bt_sink.c).
 *
 * The Pico registers an SBC SINK stream endpoint next to its A2DP SOURCE
 * endpoints. A phone pairs to it like a speaker; its SBC audio is decoded,
 * resampled 44.1->48 when the pipeline runs at 48 k, and MIXED into the
 * always-running source stream toward the headset (the pump encodes silence
 * when idle, so relay start/stop is purely a mix-gate — no stream plumbing).
 *
 * Event attribution (the ESP32 "handle-routed events" lesson): BTstack
 * broadcasts connection-level AVDTP signaling events to BOTH the source and
 * sink packet handlers. A peer is the HEADSET iff its address matches a
 * saved slot (headsets only ever come from our slots); everything else that
 * connects inbound is a phone. The source handler must consult
 * bt_sink_relay_is_phone_addr()/owns_cid() before acting on any event.
 */
#pragma once

#include <stdint.h>
#include <stdbool.h>

void bt_sink_relay_init(void);      /* register sink SEP + SDP; call from btstack_main */

/* Attribution helpers for the source-role handlers. */
bool bt_sink_relay_is_phone_addr(const uint8_t *addr);   /* not a saved headset slot */
bool bt_sink_relay_owns_cid(uint16_t avdtp_cid);         /* the phone's live avdtp_cid */

/* Mix gate: saturating-add phone PCM onto a popped slot buffer.
   Called from audio_slot_pop() for every slot (real data or silence). */
void bt_sink_relay_mix(int16_t *buf, uint16_t stereo_frames);

bool bt_sink_relay_connected(void);   /* phone AVDTP up */
bool bt_sink_relay_streaming(void);   /* phone stream STARTED */
bool bt_sink_relay_work(void);        /* decode ONE queued AAC packet — call
                                         from the MAIN LOOP only */
uint32_t bt_sink_relay_conn_age_ms(void); /* ms since phone connect, UINT32_MAX none */
uint16_t bt_sink_relay_con_handle(void);  /* phone ACL handle, 0 = none */
void bt_sink_relay_set_vol_pct(uint8_t pct);  /* phone bar: square-law, unity max */
bool bt_sink_relay_dial(const uint8_t *mac);  /* Pico -> phone outgoing connect */
void bt_sink_relay_disconnect(void);          /* drop the current phone link    */
void bt_sink_relay_dump(void);        /* diag counters ('R' debug key) */
void bt_sink_relay_toggle_aac(void);  /* 'N' key: hide/show AAC sink SEP (SBC test) */
void bt_sink_relay_sync_codec_pref(void);  /* apply USBSET_RELAY_SBC to the SEP set */
void bt_sink_relay_refresh_aac_caps(void); /* 96/128k cap; next phone connection */
