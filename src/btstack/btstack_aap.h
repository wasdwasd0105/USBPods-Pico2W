//
// Apple Accessory Protocol (AAP / AACP) over L2CAP — minimal client.
//
// Opens the AirPods AAP control channel (PSM 0x1001) from the Pico and sets the
// AirPods "dynamic latency" jitter buffer via AACP control command 0x0B.
//

#ifndef PICOW_USB_BT_AUDIO_BTSTACK_AAP_H
#define PICOW_USB_BT_AUDIO_BTSTACK_AAP_H

#include <stdint.h>
#include "btstack.h"

#ifdef __cplusplus
extern "C" {
#endif

// Open the AAP L2CAP channel (PSM 0x1001) to `addr` and run the handshake.
// No-op if a channel is already open or opening.
void aap_connect(bd_addr_t addr);

// Close the AAP channel and reset state.
void aap_disconnect(void);

// Send AACP control command 0x0B (Set Jitter Buffer / dynamic latency) with a raw
// value byte. Requires the channel to be open.
void aap_set_jitter_raw(uint8_t value);

// Cycle the jitter target (default -> 60 -> 30), sending 0x0B on each call.
void aap_cycle_jitter(void);
void aap_probe_4c(void);   /* 'Q' key: replay macOS 0x4c status poll */
void aap_send_09_03(uint8_t v); /* 'G'/'H': experimental 0x09/0x03 decoder-reset probe */

#ifdef __cplusplus
}
#endif

#endif // PICOW_USB_BT_AUDIO_BTSTACK_AAP_H
void aap_get_battery(uint8_t out[3]);   /* L,R,case: level|0x80(charging), 0xFF none */
bool    aap_channel_open(void);
bool    aap_protocol_seen(void);    /* a valid inbound AACP frame arrived (real AirPods) */
uint8_t aap_get_noise_mode(void);   /* 0 unknown, 1 Off 2 ANC 3 Transp 4 Adaptive */
bool    aap_get_ear_pause(void);
bool    aap_get_game(void);
int     aap_get_pvol(void);         /* -1 unsupported, 0/1 */
int     aap_get_ca(void);
int     aap_get_aa(void);           /* -1 unsupported, 0..100 */
void    aap_set_noise_mode(uint8_t mode);
void    aap_set_ear_pause(bool on);
void    aap_set_game(bool on);
void    aap_set_pvol(bool on);
void    aap_set_ca(bool on);
void    aap_set_aa(uint8_t strength);
