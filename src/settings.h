/* Persistent user settings — snapshot-journaled flash store.
 *
 * WHY: the legacy stores (MAC page at FLASH_SIZE-256, slot byte at
 * FLASH_SIZE-1) live INSIDE the BTstack TLV flash bank (last 2 sectors,
 * pico-sdk default) — every write erased one of BTstack's link-key sectors.
 * They also erased a 4 KB sector per single-field write, from BT callback
 * context. This store fixes all three problems.
 *
 * LAYOUT: two dedicated 4 KB sectors directly BELOW the BTstack bank:
 *   [ ... firmware ... | settings A | settings B | btstack TLV x2 ]
 * Each commit appends ONE 256-byte page to the active sector:
 *   u32 magic | u16 version | u16 payload_len | u32 seq | payload | pad | u32 crc
 * Load = scan all 32 pages of both sectors, highest CRC-valid seq wins — a
 * torn (power-fail) write simply loses CRC and the previous snapshot rules.
 * When a sector fills, the OTHER sector is erased and takes over (ping-pong):
 * one erase per 16 commits per sector ≈ millions of commits at 100k cycles.
 *
 * CONCURRENCY: mutate the RAM cache from ANY context (USB task, BTstack
 * handlers, main loop) and call settings_mark_dirty(). Flash is written ONLY
 * by settings_tick() on the main loop, debounced 200 ms so bursts (pairing
 * writes mac+name+slot) coalesce into a single page append. Call
 * settings_flush() before any reboot.
 *
 * VERSIONING: append new fields by consuming `reserved` bytes — the loader
 * zero-fills the struct then copies payload_len bytes, so old snapshots load
 * with new fields at their zero/default values. Bump SETTINGS_VERSION only
 * for incompatible layout changes.
 */
#pragma once

#include <stdint.h>
#include <stdbool.h>

#define SETTINGS_VERSION   2        /* v2: 512-byte records + saved-device list */
#define SETTINGS_NAME_LEN  32
#define SETTINGS_SAVED_N        8   /* known headsets, MRU order  */
/* Lite (open-source) caps the USABLE list at the two slot targets. The
   STORAGE layout stays identical, so full<->lite reflashes keep settings. */
#ifdef USBPODS_LITE
#define SETTINGS_SAVED_LIMIT    2
#else
#define SETTINGS_SAVED_LIMIT    SETTINGS_SAVED_N
#endif
#define SETTINGS_SAVED_NAME_LEN 24
#define SETTINGS_PHONES_N       4   /* known relay phones, MRU    */

/* usbset bits (1 = on). Erased-flash migration defaults to all-on (low 4). */
#define USBSET_BOOTCONN  0x01
#define USBSET_PAUSEDC   0x02
#define USBSET_UAC1      0x04
#define USBSET_RATE44    0x08   /* pipeline at 44.1 kHz (USB + BT codecs) */
#define USBSET_SWVOL     0x10   /* software USB volume: host vol scales USB
                                   PCM only; headset + relay get own bars */
#define USBSET_RELAY_SBC 0x20   /* phone relay codec: 1 = SBC-only sink SEP
                                   (no AAC advertised); 0 = AAC preferred.
                                   Zero on existing records -> default AAC. */
#define USBSET_UART      0x40   /* hardware UART console (debug-probe bridge):
                                   printf mirror + debug-key input on the UART
                                   pins. Default OFF (a floating RX pin can
                                   inject garbage keystrokes). */
#define USBSET_GAME      0x80   /* gaming mode: 90/60 ms jitter, ELD forced to
                                   192k / 4 slots. Default OFF. */

typedef struct __attribute__((packed)) {
    uint8_t cur_slot;                       /* 1 or 2 */
    uint8_t usbset;                         /* USBSET_* bits */
    struct __attribute__((packed)) {
        uint8_t mac[6];                     /* all-FF / all-00 = empty */
        char    name[SETTINGS_NAME_LEN];    /* NUL-terminated UTF-8 */
    } slot[2];
    uint8_t connmode;                       /* USB Connect Mode: 0/1 manual,
                                               2 audio follows headset,
                                               3 auto-dial on host playback */
    uint8_t usb_vol_p1;                     /* software USB volume, PERCENT+1
                                               (0 = unset -> 100%; zero-fill
                                               compat for old snapshots)     */
    uint8_t relay_vol_p1;                   /* phone/relay volume, percent+1  */
    struct __attribute__((packed)) {
        uint8_t mac[6];                     /* all-FF / all-00 = empty        */
        char    name[SETTINGS_SAVED_NAME_LEN];
    } saved[SETTINGS_SAVED_N];              /* known headsets, MRU-first — the
                                               ESP32 model: slots 1/2 are
                                               assignable favorites, pairing
                                               never silently evicts a slot  */
    struct __attribute__((packed)) {
        uint8_t mac[6];
        char    name[SETTINGS_SAVED_NAME_LEN];
    } phones[SETTINGS_PHONES_N];            /* relay sources (phones), MRU    */
    uint8_t eld_rate;                       /* AAC-ELD bitrate: 0/2 = 256 kbps
                                               (default; zero-fill compat),
                                               1 = 192, 3 = 320 */
    uint8_t board_type;                     /* universal-fw radio probe cache:
                                               0 unknown, 1 = Pico (2) W pins,
                                               2 = Waveshare RP2350B-Plus-W  */
    uint8_t game_level;                     /* gaming profile: 0/1 = Stable first
                                               (90 ms jitter, 6 slots), 2 = Latency
                                               first (60 ms jitter, 4 slots) */
    uint8_t bt_mac[6];                      /* local BT MAC, cached at HCI-up:
                                               USB strings enumerate BEFORE the
                                               radio is up, so the "USBPods XXXX"
                                               suffix reads last boot's cache
                                               (all-zero = not yet known) */
    uint8_t btn_mode;                       /* button action WHILE CONNECTED:
                                               0/1 = vol+ short, vol- long
                                               2 = short: reset stream
                                               3 = short: play/pause
                                               4 = none. Long-press pairing is
                                               ALWAYS disabled while connected. */
    uint8_t relay_aac_rate;                 /* phone-relay AAC cap: 0/2 = 128 kbps
                                               (default), 1 = 96 kbps. Applies at
                                               the next phone connection. */
    uint8_t unbind_armed;                   /* 1 = an unbind was armed; the next
                                               boot consumes it and, IF that boot
                                               was a real power-cycle (replug,
                                               not a soft reboot), confirms the
                                               unbind so the wipe is accepted. */
    uint8_t dbg_mode;                       /* 1 = debug mode: the console boots
                                               verbose (AVDTP capability scan +
                                               AACP protocol chatter). Per-area
                                               keys still override live. */
    uint8_t ldac_quality;                   /* LDAC EQMID choice: 0/2 = 660 kbps
                                               SQ (default; zero-fill compat),
                                               1 = 330 MQ, 3 = 990 HQ. Applies
                                               at the next LDAC connection. */
    uint8_t codec_dis;                      /* source-codec DISABLE bitmask by
                                               CODEC_ID bit (1<<id): 0x04 AAC,
                                               0x08 LDAC, 0x10 AAC-ELD, 0x20
                                               LHDC. 0 = all enabled (zero-fill
                                               compat). SBC is the mandatory
                                               baseline and cannot be disabled.
                                               Applies at the next connection. */
    uint8_t lhdc_rate;                      /* LHDC V5 bitrate: 0/2 = 400 kbps
                                               (default; zero-fill compat),
                                               1 = 256, 3 = 500. Retuned live
                                               on a running stream. */
    uint8_t hfp_en;                         /* 1 = run the HFP-AG battery SLC.
                                               0 = OFF, and off is the DEFAULT
                                               on purpose - the polarity is
                                               "enable", not "disable", so the
                                               zero-fill that every existing
                                               record already has (this byte
                                               was reserved through 1.0.1)
                                               lands on off without a
                                               migration.
                                               Why off: presenting an AG makes
                                               some headsets treat the dongle
                                               as a phone. One rang on the
                                               user's incoming call purely
                                               because our AG existed - we
                                               never send RING or callsetup,
                                               and neither claiming "no network
                                               service" nor asserting an
                                               explicit call-end stopped it
                                               (both tried on HW, both
                                               reverted). Another tore its
                                               whole profile stack down when
                                               our SLC came up. Battery
                                               percentage is not worth those.
                                               Reboot to apply. */
} user_settings_t;

void settings_init(void);            /* load / migrate; call EARLY in main() */
user_settings_t *settings(void);     /* the RAM cache — read fields directly */
void settings_mark_dirty(void);      /* after mutating the cache */
void settings_tick(void);            /* main-loop cadence: debounced commits */
bool settings_flush(void);           /* commit NOW (call before any reboot) */

/* Saved-device list: dedupes by MAC, moves to front (MRU), evicts the
   OLDEST when full — never a slot. Safe from any context (RAM cache). */
void settings_saved_add(const uint8_t mac[6], const char *name);
void settings_saved_remove(uint8_t idx);
void settings_saved_forget(const uint8_t mac[6]);  /* drop a saved entry by MAC */
void settings_phone_add(const uint8_t mac[6], const char *name);
void settings_phone_remove(uint8_t idx);
void settings_debug_dump(void);      /* seq/sector/page/rc — 'r' diagnostics */
