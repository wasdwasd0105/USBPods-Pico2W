/*
 * console.c — the USBPods serial console.
 *
 * This is the product UI for builds without the web app, so it is written for
 * a user, not a developer: a live status header, plain-language labels, and
 * one keystroke per action. Everything developer-facing lives behind 'H'.
 *
 * Key tiers
 *   punctuation , . / [ ]   media transport + volume
 *   lowercase               device + setup (the everyday keys)
 *   UPPERCASE               advanced / debug
 *
 * Layering: this file may use the public API of the audio, radio and settings
 * modules, but never their internals. Keys that need A2DP/AVDTP internals are
 * forwarded to a2dp_source_console_key(), which lives with that state and
 * returns false for keys it does not own.
 */

#include <stdbool.h>
#include <stdio.h>
#include <string.h>

#include "btstack.h"

#include "console.h"
#include "version.h"
#include "settings.h"
#include "pico_w_led.h"                 /* slot MAC/name + the settings byte  */
#include "btstack/btstack_avdtp_source.h"
#include "btstack/codec/codec.h"     /* codec_ldac_quality_kbps() */
#include "btstack/btstack_aap.h"        /* battery + noise mode for the header */
#include "btstack/btstack_hci.h"        /* get_link_keys()                     */
#include "tinyusb/uac.h"                /* usb_hid_consumer()                  */

/* HID consumer usages (Usage Page 0x0C) — the same codes the buds' own tap
   gestures send, so console transport keys behave identically to the earbuds. */
#define HID_CC_PLAY_PAUSE  0x00CD
#define HID_CC_NEXT        0x00B5
#define HID_CC_PREV        0x00B6

/* ------------------------------------------------------------------ helpers */

/* all-FF (erased flash) and all-00 both mean "no device here" */
static bool mac_empty(const uint8_t *m){
    uint8_t and_all = 0xFF, or_all = 0x00;
    for (int i = 0; i < MAC_LEN; i++){ and_all &= m[i]; or_all |= m[i]; }
    return (and_all == 0xFF) || (or_all == 0x00);
}

static const char *btn_mode_str(uint8_t m){
    switch (m){
        case 2:  return "restart the audio stream";
        case 3:  return "play / pause";
        case 4:  return "do nothing";
        default: return "volume up (short) / down (long)";
    }
}

/* name if the device ever reported one, else the address */
static const char *slot_label(uint8_t slot, char name_out[SLOT_NAME_LEN]){
    uint8_t mac[MAC_LEN];
    if (slot == 1) read_slot1_mac(mac); else read_slot2_mac(mac);
    read_slot_name(slot, name_out);
    if (mac_empty(mac))    return "(empty — press p to pair)";
    if (name_out[0] != 0)  return name_out;
    return bd_addr_to_str(mac);
}

/* ------------------------------------------------------------------- screens */

/* what the link is actually carrying right now (0 = nothing negotiated) */
static const char *codec_name(uint8_t c){
    switch (c){
        case 1:  return "SBC";
        case 2:  return "AAC";
        case 3:  return "LDAC";
        case 4:  return "AAC-ELD";
        case 5:  return "LHDC";
        default: return NULL;
    }
}

/* Codec enable toggle (keys 5-8): flips the settings codec_dis bit and
   persists. Applies at the NEXT connection — a live stream is never torn
   down under the user; SBC has no key because the A2DP baseline must stay. */
static void console_toggle_codec(uint8_t id, const char *name){
    settings()->codec_dis ^= (uint8_t)(1u << id);
    settings_mark_dirty();
    printf("%s: %s (applies at the next connection)\n",
           name, codec_enabled(id) ? "enabled" : "disabled");
}

/* Label for the device ACTUALLY connected, not for whatever slot happens to
   be current. The slot normally follows the connection (see the AVDTP
   established handler), but a headset dialled from the saved MRU list via the
   web occupies no slot at all — resolve it there too, and fall back to the
   address. */
static const char *connected_label(char out[SLOT_NAME_LEN]){
    const uint8_t *addr = a2dp_source_peer_addr();   /* NOT the dial target */
    uint8_t m[MAC_LEN];
    out[0] = 0;
    for (uint8_t s = 1; s <= 2; s++){
        if (s == 1) read_slot1_mac(m); else read_slot2_mac(m);
        if (memcmp(addr, m, MAC_LEN) == 0){
            read_slot_name(s, out);
            if (out[0]) return out;
        }
    }
    for (uint8_t i = 0; i < SETTINGS_SAVED_N; i++){
        if (memcmp(addr, settings()->saved[i].mac, MAC_LEN) != 0) continue;
        size_t n = sizeof(settings()->saved[i].name);
        if (n > SLOT_NAME_LEN - 1) n = SLOT_NAME_LEN - 1;
        memcpy(out, settings()->saved[i].name, n);
        out[n] = 0;
        if (out[0]) return out;
    }
    return bd_addr_to_str((uint8_t *) addr);
}

void console_show_menu(void){
    uint8_t cur = read_uint8_last_flash();
    char    nm[SLOT_NAME_LEN];

#ifdef USBPODS_LITE
    printf("\n=============  USBPods Lite  Ver. %s  =============\n", USBPODS_VERSION_NAME);
#else
    printf("\n===============  USBPods  Ver. %s  ===============\n", USBPODS_VERSION_NAME);
#endif
    /* live status — answers "is it working?" with no key pressed */
    if (get_a2dp_connected_flag()){
        printf(" Connected: %s%s\n",
               connected_label(nm),
               a2dp_source_is_playing() ? "   (playing)" : "");
        {   uint8_t b[3]; aap_get_battery(b);
            const char *codec = codec_name(get_cur_codec());
            bool any = (b[0] != 0xFF) || (b[1] != 0xFF) || (b[2] != 0xFF);
            if (codec || any){
                printf("           ");
                if (codec){
                    printf(" %s", codec);
                    uint32_t kb = a2dp_source_live_kbps();   /* on-air rate */
                    if (kb) printf("  %lu kbps", (unsigned long) kb);
                }
                if (b[0] != 0xFF) printf("   L %u%%%s",   b[0] & 0x7F, (b[0] & 0x80) ? "+" : "");
                if (b[1] != 0xFF) printf("   R %u%%%s",   b[1] & 0x7F, (b[1] & 0x80) ? "+" : "");
                if (b[2] != 0xFF) printf("   case %u%%%s", b[2] & 0x7F, (b[2] & 0x80) ? "+" : "");
                printf("\n");
            }
        }
    } else {
        printf(" Not connected — press c to connect, or p to pair new earbuds\n");
    }
    printf("-----------------------------------------------------\n");
    printf(" Music      ,  next        .  previous    /  play-pause\n");
    printf(" Volume     [  up          ]  down\n");
    printf("-----------------------------------------------------\n");
    printf(" Earbuds    c  connect     d  disconnect  p  pair NEW earbuds\n");
    for (uint8_t s = 1; s <= 2; s++){
        const char *label = slot_label(s, nm);
        printf("            %u  %s%s\n", s, label, (s == cur) ? "   <- current" : "");
    }
    printf("            press 1 or 2 to change slot\n");
    printf("            l  all saved devices          r  forget everything\n");
    printf("-----------------------------------------------------\n");
    {   uint8_t us = read_settings_byte();
        printf(" Settings   w  auto-connect on power ....... %s\n",
               (us & USBSET_BOOTCONN) ? "ON" : "off");
        printf("            u  USB audio mode .............. %s\n",
               (us & USBSET_UAC1) ? "UAC1 (legacy hosts)" : "UAC2");
        printf("            k  USB sample rate ............. %s\n",
               (us & USBSET_RATE44) ? "44.1 kHz" : "48 kHz");
        printf("            b  earbud button ............... %s\n",
               btn_mode_str(settings()->btn_mode));
        printf("            q  LDAC quality ................ %d kbps\n",
               codec_ldac_quality_kbps());
        printf("            e  LHDC bitrate ................ %d kbps\n",
               codec_lhdc_setting_kbps());
        printf(" Codecs     5  AAC-ELD (AirPods) ........... %s\n",
               codec_enabled(CODEC_ID_AACELD) ? "ON" : "off");
        printf("            6  LHDC V5 ..................... %s\n",
               codec_enabled(CODEC_ID_LHDC) ? "ON" : "off");
        printf("            7  LDAC ........................ %s\n",
               codec_enabled(CODEC_ID_LDAC) ? "ON" : "off");
        printf("            8  AAC ......................... %s\n",
               codec_enabled(CODEC_ID_AAC) ? "ON" : "off");
    }
    printf("-----------------------------------------------------\n");
    printf(" h  show this menu   H  advanced menu   D  debug mode\n");
    printf("=====================================================\n");
}

/* every device this dongle knows: the two dialable slots, the MRU headset
   history slots are assigned from, and the relay phones */
static void show_devices(void){
    char    nm[SLOT_NAME_LEN];
    uint8_t cur = read_uint8_last_flash();

    printf("\n--- paired devices ---\n");
    for (uint8_t s = 1; s <= 2; s++)
        printf("slot %u%s %s\n", s, (s == cur) ? " *" : "  ", slot_label(s, nm));

    /* The saved-headset history and the phone relay are Full-only features
       (no web UI to assign from; no relay in Lite) — in Lite the two slots
       ARE the device list. */
#ifndef USBPODS_LITE
    uint8_t n = 0;
    printf("saved earbuds (assign to a slot from the web UI):\n");
    for (uint8_t i = 0; i < SETTINGS_SAVED_LIMIT; i++){
        const uint8_t *m = settings()->saved[i].mac;
        if (mac_empty(m)) continue;
        printf("  %u  %s  %.*s\n", i, bd_addr_to_str((uint8_t *)m),
               SETTINGS_SAVED_NAME_LEN, settings()->saved[i].name);
        n++;
    }
    if (n == 0) printf("  (none)\n");

    n = 0;
    printf("relay phones:\n");
    for (uint8_t i = 0; i < SETTINGS_PHONES_N; i++){
        const uint8_t *m = settings()->phones[i].mac;
        if (mac_empty(m)) continue;
        printf("  %u  %s  %.*s\n", i, bd_addr_to_str((uint8_t *)m),
               SETTINGS_SAVED_NAME_LEN, settings()->phones[i].name);
        n++;
    }
    if (n == 0) printf("  (none)\n");
#endif
    printf("---\n");
}

static void show_advanced_menu(void){
    printf("\n--- USBPods advanced (debug mode %s) ---\n",
           settings()->dbg_mode ? "ON" : "off");
    printf("mode    D debug mode on/off, saved to flash (boots verbose)\n");
    printf("report  R status report   V 1 Hz [diag] telemetry   C AVDTP capability scan\n");
    printf("taps    E AAC-ELD frame tap   U USB PCM tap   B raw USB PCM hex dump\n");
    printf("        A raw AACP frame log\n");
    printf("heal    K kick (suspend->rebuild->restart)   J jitter cycle 150/60/30\n");
    printf("        X auto-recovery on/off   F fake one EP splice\n");
    printf("aacp    Q 0x4c metrics probe   G 0x09/03=0   I 0x09/03=1\n");
    printf("radio   T tx power (headset)   W tx power (phone relay)\n");
    printf("        N hide AAC sink SEP (force SBC relay)   M send one media packet\n");
    printf("avdtp   i select endpoint   a all caps   f get cfg   s set cfg   n reconfigure\n");
    printf("        o open   m start   t stop   P suspend   x abort   g next codec + start\n");
    printf("---\n");
}

/* -------------------------------------------------------------- the keys */

/* 'r' is destructive, so it takes a confirmation keystroke */
static bool confirm_wipe;

static void forget_everything(void){
    uint8_t ff[MAC_LEN];
    printf("y\nDeleting all link keys and slot records\n");
    gap_delete_all_link_keys();
    memset(ff, 0xFF, sizeof(ff));
    write_slot1_mac(ff);   write_slot2_mac(ff);
    write_slot_name(1, ""); write_slot_name(2, "");
    get_link_keys();
    printf("all pairings deleted — 'p' to pair again\n");
}

static void select_slot(uint8_t want){
    char nm[SLOT_NAME_LEN];
    if (get_a2dp_connected_flag()){
        uint8_t m[MAC_LEN];
        if (want == 1) read_slot1_mac(m); else read_slot2_mac(m);
        if (memcmp(m, a2dp_source_peer_addr(), MAC_LEN) == 0){
            printf("slot %u is already connected\n", want);
            return;
        }
        /* live handover: drop the current link, then dial the picked slot
           as soon as the release completes (btstack_avdtp_source.c) */
        printf("slot %u  %s — dropping the current link, then dialing\n",
               want, slot_label(want, nm));
        a2dp_source_switch_slot(want);
        return;
    }
    write_uint8_last_flash(want);
    get_link_keys();
    printf("dialing slot %u\n", want);
    a2dp_source_reconnect();
}

/* the USBSET bits are a single persisted byte shared with the web UI */
static void toggle_usbset(uint8_t bit, const char *label,
                          const char *on, const char *off, const char *note){
    uint8_t v = (uint8_t)(read_settings_byte() ^ bit);
    write_settings_byte(v);
    printf("%s: %s%s\n", label, (v & bit) ? on : off, note ? note : "");
}

void console_key(char cmd){
    /* pending confirmation owns the next keystroke */
    if (confirm_wipe){
        confirm_wipe = false;
        if (cmd != 'y' && cmd != 'Y'){ printf("cancelled\n"); return; }
        forget_everything();
        return;
    }

    /* advanced/debug keys (and any modal they own) belong to the module that
       holds the state; it returns false for keys it does not handle */
    if (a2dp_source_console_key(cmd)) return;

    switch (cmd){
        /* ---- music + volume ------------------------------------------- */
        case ',': printf(" - next track\n");     usb_hid_consumer(HID_CC_NEXT);       break;
        case '.': printf(" - previous track\n"); usb_hid_consumer(HID_CC_PREV);       break;
        case '/': printf(" - play/pause\n");     usb_hid_consumer(HID_CC_PLAY_PAUSE); break;
        case '[': increase_vol_by_key(); break;
        case ']': decrease_vol_by_key(); break;

        /* ---- earbuds --------------------------------------------------- */
        case 'c':
            printf("connecting to %s\n", get_device_addr_string());
            a2dp_source_reconnect();
            break;
        case 'd':
            printf("disconnecting\n");
            a2dp_source_console_disconnect();
            if (settings()->connmode == 3)
                printf("(auto-dial holds off until playback stops and restarts)\n");
            break;
        case 'p':   /* pair into the CURRENT slot: drop the link, then scan —
                       the same path as the web UI's "pair new" and the
                       button long-press */
            printf("pairing into slot %u — scanning\n", read_uint8_last_flash());
            avdtp_disconnect_and_scan();
            break;
        case '1': select_slot(1); break;
        case '2': select_slot(2); break;
        case '5': console_toggle_codec(CODEC_ID_AACELD, "AAC-ELD"); break;
        case '6': console_toggle_codec(CODEC_ID_LHDC,   "LHDC V5"); break;
        case '7': console_toggle_codec(CODEC_ID_LDAC,   "LDAC");    break;
        case '8': console_toggle_codec(CODEC_ID_AAC,    "AAC");     break;
        case 'l': show_devices();  break;
        case 'r':   /* FORGET EVERYTHING — keys AND slot records. Keys alone
                       leave a slot dialing a device whose key is gone, which
                       is the status-102 trap. */
            printf("Delete ALL pairings (link keys + slots 1 and 2)? [y/N] ");
            confirm_wipe = true;
            break;

        /* ---- settings (same fields the web UI writes) ------------------- */
        case 'w':
            toggle_usbset(USBSET_BOOTCONN, "auto-connect on power", "ON", "off", NULL);
            break;
        case 'u':   /* descriptor order, read once at enumeration */
            toggle_usbset(USBSET_UAC1, "USB audio mode", "UAC1", "UAC2",
                          " (replug USB to apply)");
            break;
        case 'k':   /* descriptor rate, baked in at enumeration */
            toggle_usbset(USBSET_RATE44, "USB sample rate", "44.1 kHz", "48 kHz",
                          " (replug USB to apply)");
            break;
        case 'q': { /* LDAC quality — cycles 660 -> 990 -> 330, persisted */
            uint8_t cur = settings()->ldac_quality;
            settings()->ldac_quality = (cur == 3) ? 1 : (cur == 1) ? 2 : 3;
            settings_mark_dirty();
            codec_ldac_request_quality(settings()->ldac_quality);   /* live, no replug */
            printf("LDAC quality: %d kbps\n", codec_ldac_quality_kbps());
            break;
        }
        case 'e': { /* LHDC bitrate — cycles 400 -> 500 -> 256, persisted */
            uint8_t cur = settings()->lhdc_rate;
            /* 400 -> 500 -> 900 -> 256 -> 400 */
            settings()->lhdc_rate = (cur == 3) ? 4 : (cur == 4) ? 1 : (cur == 1) ? 2 : 3;
            settings_mark_dirty();
            codec_lhdc_request_rate(settings()->lhdc_rate);   /* live, no replug */
            printf("LHDC bitrate: %d kbps\n", codec_lhdc_setting_kbps());
            break;
        }
        case 'b': { /* button action WHILE CONNECTED — cycles 1..4 */
            uint8_t m = settings()->btn_mode ? settings()->btn_mode : 1;
            m = (uint8_t)((m >= 4) ? 1 : m + 1);
            settings()->btn_mode = m;
            settings_mark_dirty();
            printf("earbud button: %s\n", btn_mode_str(m));
            break;
        }

        /* ---- menus ----------------------------------------------------- */
        case 'D': {  /* debug MODE — persisted, survives reboot */
            bool on = !settings()->dbg_mode;
            settings()->dbg_mode = on ? 1 : 0;
            settings_mark_dirty();
            a2dp_source_set_debug_mode(on);
            printf("[dbg] debug mode %s (saved) — AVDTP capability scan + AACP logs %s\n",
                   on ? "ON" : "off", on ? "on" : "off");
            break;
        }
        case 'H': show_advanced_menu(); break;
        case 'h':
        case '?': console_show_menu(); break;

        case '\n':
        case '\r': {  /* Enter shows the menu; a \r\n pair counts once */
            static uint32_t last_ms;
            uint32_t now = btstack_run_loop_get_time_ms();
            if (now - last_ms > 200) console_show_menu();
            last_ms = now;
            break;
        }
        default:   console_show_menu(); break;   /* space or any unbound key */
    }
}

void console_init(void){
#ifdef HAVE_BTSTACK_STDIN
    btstack_stdin_setup(console_key);
#endif
    /* persisted debug mode: boot verbose or boot quiet */
    if (settings()->dbg_mode){
        a2dp_source_set_debug_mode(true);
        printf("[dbg] debug mode ON (saved) — protocol logs enabled\n");
    }
}
