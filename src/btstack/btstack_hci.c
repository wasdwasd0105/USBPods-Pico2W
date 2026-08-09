//
// Created by Sean on 8/10/23.
//

#include <stdint.h>
#include "hardware/watchdog.h"   /* scratch[7] hang forensics */
#include <stdio.h>
#include <string.h>
#include <inttypes.h>

#include "btstack_hci.h"
#include "btstack_avdtp_source.h"
#include "../pico_w_led.h"
#include "../settings.h"


#define A2DP_SOURCE_DEMO_INQUIRY_DURATION_1280MS 12

static btstack_packet_callback_registration_t hci_event_callback_registration;

static char device_addr_string[] = "00:00:00:00:00:00";
static bd_addr_t device_addr;
static bd_addr_t device_addr_list[2];

static bool scan_active = true;
static bool s_pairing = false;    /* inquiry running for a NEW pairing (web/button) */
static char s_bt_name[20] = "USBPods";   /* + " XXXX" MAC suffix at HCI-up */

bool bt_pairing_active(void) { return s_pairing; }

/* ---- Web scan-list mode: collect audio devices instead of auto-pairing ----
   (The BOOT long-press keeps the headless auto-pair path: s_list_mode off.) */
#define SCAN_MAX 4
typedef struct { bd_addr_t addr; int8_t rssi; char name[24]; } scan_hit_t;
static scan_hit_t s_found[SCAN_MAX];
static uint8_t    s_found_n;
static bool       s_list_mode;
static uint8_t    s_scan_rounds;

void bt_scan_start_list(void){
    s_list_mode = true;
    s_found_n = 0;
    s_scan_rounds = 0;
    gap_start_scanning();
}

uint8_t bt_scan_count(void){ return s_found_n; }

bool bt_scan_get(uint8_t i, uint8_t mac[6], int8_t *rssi, char name[24]){
    if (i >= s_found_n) return false;
    memcpy(mac, s_found[i].addr, 6);
    *rssi = s_found[i].rssi;
    memcpy(name, s_found[i].name, 24);
    return true;
}

/* Pair to a scanned device: write it into the CURRENT slot and dial it.
   Mirrors the auto-pair path byte for byte. */
/* Slot has a saved device? (all-FF = erased, all-00 = never written) */
static bool slot_mac_valid(uint8_t slot){
    uint8_t mac[6];
    if (slot == 2) read_slot2_mac(mac); else read_slot1_mac(mac);
    bool any = false, zero = true;
    for (int k = 0; k < 6; k++) { any |= mac[k] != 0xFF; zero &= mac[k] == 0; }
    return any && !zero;
}

/* Pairing a new headset INTO a slot.
   Lite has exactly two slots and no web UI to re-assign remembered headsets,
   so a slot owns its device outright: replacing it forgets the old one for
   good — link key and saved record. Full keeps the history, because there the
   saved list is a feature (assign any remembered headset to a slot). */
#ifdef USBPODS_LITE
static void slot_replace_purge(uint8_t slot, const uint8_t *new_mac){
    uint8_t old_mac[6], other[6];
    if (!slot_mac_valid(slot)) return;                 /* slot was empty */
    if (slot == 2) read_slot2_mac(old_mac); else read_slot1_mac(old_mac);
    if (memcmp(old_mac, new_mac, 6) == 0) return;      /* same device again */
    if (slot == 2) read_slot1_mac(other); else read_slot2_mac(other);
    if (memcmp(old_mac, other, 6) != 0) {              /* not held by the other slot */
        gap_drop_link_key_for_bd_addr(old_mac);
        printf("slot %u: forgetting %s (replaced)\n", slot, bd_addr_to_str(old_mac));
    }
    settings_saved_forget(old_mac);
}
#else
#define slot_replace_purge(slot, new_mac)   ((void)0)
#endif

bool bt_pair_to(uint8_t i){
    if (i >= s_found_n) return false;
    gap_inquiry_stop();
    scan_active = false;
    s_list_mode = false;
    s_pairing = false;
    memcpy(device_addr, s_found[i].addr, 6);
    s_found_n = 0;   /* consumed: the web scan list clears on connect */
    /* Prefer an EMPTY slot: pairing a second headset must not silently eat
       the first (HW-reported: Bose replaced the AirPods, "web page only has
       one"). Only when both slots are taken does pairing replace the
       current slot. */
    uint8_t currect_slot = read_uint8_last_flash();
    uint8_t other_slot = (currect_slot == 0x1) ? 0x2 : 0x1;
    if (slot_mac_valid(currect_slot) && !slot_mac_valid(other_slot)) {
        currect_slot = other_slot;
        write_uint8_last_flash(currect_slot);   /* new device becomes default */
        printf("Web pair: slot %u is free — pairing there\n", currect_slot);
    }
    printf("Web pair: connecting to %s (%s) into slot %u\n",
           bd_addr_to_str(device_addr), s_found[i].name, currect_slot);
    slot_replace_purge(currect_slot, device_addr);
    if (currect_slot == 0x1) write_slot1_mac(device_addr);
    if (currect_slot == 0x2) write_slot2_mac(device_addr);
    write_slot_name(currect_slot, s_found[i].name);
    settings_saved_add(device_addr, s_found[i].name);   /* remembered forever */
    get_link_keys();
    avdtp_source_establish_stream();
    return true;
}


/* ---- phone pairing window ---------------------------------------------------
   Discoverable ONLY while this window is open (web "Pair phone" button):
   60 s, auto-closed by the timer or early by bt_phone_pair_window(false)
   (called when a phone connects). */
#define PHONE_PAIR_WINDOW_MS 60000
static btstack_timer_source_t s_pairwin_timer;
static volatile bool s_pairwin_open;

static void pairwin_timeout(btstack_timer_source_t *ts) {
    (void)ts;
    if (s_pairwin_open) {
        extern void hfp_battery_sdp_hide(bool hide);
        s_pairwin_open = false;
        gap_discoverable_control(0);
        hfp_battery_sdp_hide(false);
        printf("Phone pairing window CLOSED (timeout)\n");
    }
}

void bt_phone_pair_window(bool open) {
    extern void hfp_battery_sdp_hide(bool hide);
    btstack_run_loop_remove_timer(&s_pairwin_timer);
    if (open && !s_pairwin_open) {
        s_pairwin_open = true;
        hfp_battery_sdp_hide(true);
        gap_discoverable_control(1);
        btstack_run_loop_set_timer_handler(&s_pairwin_timer, pairwin_timeout);
        btstack_run_loop_set_timer(&s_pairwin_timer, PHONE_PAIR_WINDOW_MS);
        btstack_run_loop_add_timer(&s_pairwin_timer);
        printf("Phone pairing window OPEN (60 s) — discoverable as '%s'\n", s_bt_name);
    } else if (!open && s_pairwin_open) {
        s_pairwin_open = false;
        gap_discoverable_control(0);
        hfp_battery_sdp_hide(false);
        printf("Phone pairing window CLOSED\n");
    }
}

bool bt_phone_pair_window_open(void) { return s_pairwin_open; }

/* Dial a saved-list device directly WITHOUT touching the slots (the slots
   are favorites; row-Connect is a one-off). Auto-reconnect/boot still dial
   the current slot. */
bool bt_connect_saved(const uint8_t *mac){
    memcpy(device_addr, mac, 6);
    printf("Connect saved: %s\n", bd_addr_to_str(device_addr));
    avdtp_source_establish_stream();
    return true;
}

const char * get_device_addr_string(){
    return device_addr_string;
}

bd_addr_t * get_device_addr(){
    return &device_addr;
}

bd_addr_t * get_device_addr_from_list(uint8_t i){
    return &device_addr_list[i];
}

void gap_start_scanning(void){
    printf("Start scanning...\n");
    gap_inquiry_start(A2DP_SOURCE_DEMO_INQUIRY_DURATION_1280MS);
    scan_active = true;
    s_pairing = true;
    led_set_mode(LED_PAIRING);
}


/* Inbound-connection CoD cache (see HCI_EVENT_CONNECTION_REQUEST below). */
#define INBOUND_COD_N 4
static struct { bd_addr_t addr; uint32_t cod; } s_inbound_cod[INBOUND_COD_N];
static uint8_t s_inbound_cod_w;

uint32_t bt_inbound_cod(const uint8_t *addr){
    for (int k = 0; k < INBOUND_COD_N; k++) {
        if (memcmp(s_inbound_cod[k].addr, addr, 6) == 0) return s_inbound_cod[k].cod;
    }
    return 0;
}

static void hci_packet_handler_inner(uint8_t packet_type, uint16_t channel, uint8_t *packet, uint16_t size);
static void hci_packet_handler(uint8_t packet_type, uint16_t channel, uint8_t *packet, uint16_t size){
    watchdog_hw->scratch[7] = 0x70;   /* entered (hang forensics) */
    hci_packet_handler_inner(packet_type, channel, packet, size);
    watchdog_hw->scratch[7] = 0x7F;   /* exited cleanly */
}
static void hci_packet_handler_inner(uint8_t packet_type, uint16_t channel, uint8_t *packet, uint16_t size){
    UNUSED(channel);
    UNUSED(size);
    if (packet_type != HCI_EVENT_PACKET) return;
    uint8_t status;
    UNUSED(status);

    bd_addr_t address;
    uint32_t cod;

    // Service Class: Rendering | Audio, Major Device Class: Audio
    const uint32_t bluetooth_speaker_cod = 0x200000 | 0x040000 | 0x000400;

    switch (hci_event_packet_get_type(packet)){
        case HCI_EVENT_FLUSH_OCCURRED:
            /* THE universal damage signal: the controller discarded a packet
               that missed the 200 ms automatic-flush deadline
               (gap_enable_link_watchdog + the media CID registered flushable).
               This is the ONLY increment site (the SDK hci.c patch used to
               increment too — every event was double-counted, which is where
               the defect doc's "+2 events per manual flush" came from).
               With manual flushing on Enhanced Flush (0x39 below), an event
               here is the automatic timer firing — EXCEPT when the classic
               HCI_Flush escalation has fired (it emits 0x11 too): while
               src_flush_fallbacks/cflush reads 0, this counter alone answers
               the "does the CYW43 arm the timer" question; nonzero cflush
               marks the data contaminated. Wire-reference: macOS logged 1506 of these in 30 s
               at ~70 % loss (macos-link-recovery-report.md). */
            usbpods_flush_occurred_count++;
            break;
        case 0x39:  /* HCI_Enhanced_Flush_Complete — our manual jam-breakers.
                       Stock BTstack ignores this event (unlike Flush
                       Occurred, which upstream punishes with a disconnect),
                       and Enhanced Flush spares non-flushable control. */
            usbpods_enhanced_flush_done_count++;
            break;
        case HCI_EVENT_COMMAND_COMPLETE: {
            uint16_t op = hci_event_command_complete_get_command_opcode(packet);
            const uint8_t *rp = hci_event_command_complete_get_return_parameters(packet);
            if (op == 0x1405) {          /* Read RSSI: status, handle, rssi */
                printf("[link] rssi %d dBm (status %u)\n", (int8_t)rp[3], rp[0]);
            } else if (op == 0x1403) {   /* Read Link Quality */
                printf("[link] link quality %u/255 (status %u)\n", rp[3], rp[0]);
            } else if (op == 0x1401) {   /* Read Failed Contact Counter:
                                            increments ONLY on real flush
                                            failures — proof the controller
                                            truly discards */
                printf("[link] failed contact counter %u (status %u)\n",
                       little_endian_read_16(rp, 3), rp[0]);
            } else if (op == 0x0C27) {   /* Read Automatic Flush Timeout */
                uint16_t slots = little_endian_read_16(rp, 3);
                printf("[link] auto flush timeout READBACK: %u slots = %u ms (status %u)\n",
                       slots, (unsigned)(slots * 625 / 1000), rp[0]);
            }
            break;
        }
        case 0x1D: {                     /* Connection Packet Type Changed */
            uint8_t  st = packet[2];
            uint16_t pt = little_endian_read_16(packet, 5);
            printf("[link] packet types now 0x%04x (status %u)\n", pt, st);
            break;
        }
        case  BTSTACK_EVENT_STATE:
            if (btstack_event_state_get_state(packet) != HCI_STATE_WORKING) return;
            /* Headless first-boot auto-pair: hunt for a speaker ONLY when
               NOTHING is saved. The old gate compared device_addr_string,
               which stopped tracking the settings store — a dongle with a
               saved headset scanned anyway, and the inquiry fought the
               auto-connect page attempts (status 102 / 0x13 churn). */
            if (!slot_mac_valid(1) && !slot_mac_valid(2)){
                gap_start_scanning();
            }
            break;

        case HCI_EVENT_PIN_CODE_REQUEST:
            printf("Pin code request - using '0000'\n");
            hci_event_pin_code_request_get_bd_addr(packet, address);
            gap_pin_code_response(address, "0000");
            break;
        /* ---- BENCH DIAGNOSTIC (pairing-unsuccessful hunt, 2026-08-10):
           one line per SSP/auth stage so a single failed pairing attempt
           names the stage that dies. Cheap enough to keep. ---- */
        case HCI_EVENT_CONNECTION_COMPLETE: {
            uint8_t st = hci_event_connection_complete_get_status(packet);
            hci_event_connection_complete_get_bd_addr(packet, address);
            printf("[pair] ACL complete %s status 0x%02x handle 0x%04x\n",
                   bd_addr_to_str(address), st,
                   hci_event_connection_complete_get_connection_handle(packet));
            break;
        }
        case HCI_EVENT_IO_CAPABILITY_REQUEST:
            hci_event_io_capability_request_get_bd_addr(packet, address);
            printf("[pair] IO capability REQUEST from %s (stack auto-replies)\n", bd_addr_to_str(address));
            break;
        case HCI_EVENT_IO_CAPABILITY_RESPONSE:
            hci_event_io_capability_response_get_bd_addr(packet, address);
            printf("[pair] IO capability RESPONSE: io %u auth_req %u\n",
                   hci_event_io_capability_response_get_io_capability(packet),
                   hci_event_io_capability_response_get_authentication_requirements(packet));
            break;
        case HCI_EVENT_USER_CONFIRMATION_REQUEST:
            printf("[pair] user confirmation request (numeric %06lu) — stack auto-accept\n",
                   (unsigned long)hci_event_user_confirmation_request_get_numeric_value(packet));
            break;
        case HCI_EVENT_SIMPLE_PAIRING_COMPLETE:
            hci_event_simple_pairing_complete_get_bd_addr(packet, address);
            printf("[pair] simple pairing COMPLETE %s status 0x%02x\n",
                   bd_addr_to_str(address), hci_event_simple_pairing_complete_get_status(packet));
            break;
        case HCI_EVENT_LINK_KEY_REQUEST:
            printf("[pair] link key request (stack answers from store)\n");
            break;
        case HCI_EVENT_LINK_KEY_NOTIFICATION:
            printf("[pair] link key NOTIFICATION (new key stored)\n");
            break;
        case HCI_EVENT_AUTHENTICATION_COMPLETE:
            printf("[pair] authentication complete status 0x%02x\n",
                   hci_event_authentication_complete_get_status(packet));
            break;
        case HCI_EVENT_ENCRYPTION_CHANGE:
            printf("[pair] encryption change status 0x%02x enabled %u\n",
                   hci_event_encryption_change_get_status(packet),
                   hci_event_encryption_change_get_encryption_enabled(packet));
            break;
        case HCI_EVENT_REMOTE_NAME_REQUEST_COMPLETE: {
            /* Requested at relay-phone adoption: update the phone-list entry
               with the friendly name (dedupe keeps MRU position sane). */
            if (hci_event_remote_name_request_complete_get_status(packet) == 0) {
                hci_event_remote_name_request_complete_get_bd_addr(packet, address);
                const char *rn = hci_event_remote_name_request_complete_get_remote_name(packet);
                char nm[SETTINGS_SAVED_NAME_LEN];
                strncpy(nm, rn, sizeof(nm) - 1);
                nm[sizeof(nm) - 1] = 0;
                /* A SLOT device's name completion backfills the slot (v1-era
                   records saved MAC only — the web showed "Slot 2 is empty"
                   for a slot that had a device). Anything else is a relay
                   phone (the original purpose of this request). */
                bool was_slot = false;
                for (uint8_t sl = 1; sl <= 2; sl++) {
                    uint8_t m[6];
                    if (sl == 1) read_slot1_mac(m); else read_slot2_mac(m);
                    if (memcmp(m, address, 6) == 0) {
                        was_slot = true;
                        char cur[SLOT_NAME_LEN];
                        read_slot_name(sl, cur);
                        if (cur[0] == 0) {
                            write_slot_name(sl, nm);
                            settings_saved_add(address, nm);
                            printf("slot %u name backfilled: %s\n", sl, nm);
                        }
                    }
                }
                if (!was_slot) settings_phone_add(address, nm);
            }
            break;
        }
        case HCI_EVENT_CONNECTION_REQUEST: {
            /* Cache the peer's Class of Device: the sink relay's phone-vs-
               headset attribution needs more than "is it in a slot" (HW: an
               un-slotted Bose reconnecting inbound was adopted as a phone). */
            hci_event_connection_request_get_bd_addr(packet, address);
            uint32_t req_cod = hci_event_connection_request_get_class_of_device(packet);
            memcpy(s_inbound_cod[s_inbound_cod_w].addr, address, 6);
            s_inbound_cod[s_inbound_cod_w].cod = req_cod;
            s_inbound_cod_w = (uint8_t)((s_inbound_cod_w + 1) % INBOUND_COD_N);
            /* NOTE: no key-dropping here. A re-pairing phone REPLACES our key
               anyway; dropping at connection-request deleted a FRESH key when
               pair and connect arrived as separate requests in one window
               (encryption then failed -> phone terminated, reason 0x13). */
            break;
        }
        case HCI_EVENT_DISCONNECTION_COMPLETE: {
            /* WHO dropped and WHY — 0x08 = supervision timeout (radio lost
               the link, scatternet starvation), 0x13 = remote ended it,
               0x16 = we ended it. Key for relay disconnect diagnosis. */
            uint8_t reason = hci_event_disconnection_complete_get_reason(packet);
            const char *why = reason == 0x08 ? "supervision TIMEOUT (link lost)"
                            : reason == 0x13 ? "remote terminated"
                            : reason == 0x16 ? "local terminated"
                            : reason == 0x22 ? "LMP response timeout"
                            : "other";
            printf("ACL disconnected: handle 0x%04x reason 0x%02x (%s)\n",
                   hci_event_disconnection_complete_get_connection_handle(packet),
                   reason, why);
            {   /* headset-side power-off/disconnect: stop the auto-redial */
                extern void a2dp_source_remote_bye(uint16_t handle, uint8_t reason);
                a2dp_source_remote_bye(
                    hci_event_disconnection_complete_get_connection_handle(packet), reason);
            }
            break;
        }
        case GAP_EVENT_INQUIRY_RESULT:
            gap_event_inquiry_result_get_bd_addr(packet, address);
            // print info
            printf("Device found: %s ",  bd_addr_to_str(address));
            cod = gap_event_inquiry_result_get_class_of_device(packet);
            printf("with COD: %06" PRIx32, cod);
            if (gap_event_inquiry_result_get_rssi_available(packet)){
                printf(", rssi %d dBm", (int8_t) gap_event_inquiry_result_get_rssi(packet));
            }
            if (gap_event_inquiry_result_get_name_available(packet)){
                char name_buffer[240];
                int name_len = gap_event_inquiry_result_get_name_len(packet);
                memcpy(name_buffer, gap_event_inquiry_result_get_name(packet), name_len);
                name_buffer[name_len] = 0;
                printf(", name '%s'", name_buffer);
            }
            printf("\n");
            if (s_list_mode){
                /* Collect audio-class devices for the web list; no auto-connect. */
                if ((cod & bluetooth_speaker_cod) != bluetooth_speaker_cod) break;
                uint8_t idx = SCAN_MAX;
                for (uint8_t i = 0; i < s_found_n; i++){
                    if (memcmp(s_found[i].addr, address, 6) == 0){ idx = i; break; }
                }
                if (idx == SCAN_MAX){
                    if (s_found_n >= SCAN_MAX) break;
                    idx = s_found_n++;
                    memcpy(s_found[idx].addr, address, 6);
                    s_found[idx].name[0] = 0;
                    s_found[idx].rssi = 0;
                }
                if (gap_event_inquiry_result_get_rssi_available(packet)){
                    s_found[idx].rssi = (int8_t)gap_event_inquiry_result_get_rssi(packet);
                }
                if (gap_event_inquiry_result_get_name_available(packet)){
                    int nl = gap_event_inquiry_result_get_name_len(packet);
                    if (nl > 23) nl = 23;
                    memcpy(s_found[idx].name, gap_event_inquiry_result_get_name(packet), nl);
                    s_found[idx].name[nl] = 0;
                }
                break;
            }
            if ((cod & bluetooth_speaker_cod) == bluetooth_speaker_cod){
                memcpy(device_addr, address, 6);
                printf("Bluetooth speaker detected, trying to connect to %s...\n", bd_addr_to_str(device_addr));
                scan_active = false;
                s_pairing = false;
                gap_inquiry_stop();

                uint8_t currect_slot = read_uint8_last_flash();

                slot_replace_purge(currect_slot, device_addr);
                if (currect_slot == 0x1){
                    write_slot1_mac(device_addr);
                }
    
                if (currect_slot == 0x2){
                    write_slot2_mac(device_addr);
                }
                /* Persist the EIR name next to the MAC so the web UI can show
                   which headset lives in this slot (empty clears stale). */
                if (gap_event_inquiry_result_get_name_available(packet)){
                    char nm[SLOT_NAME_LEN];
                    int nl = gap_event_inquiry_result_get_name_len(packet);
                    if (nl > SLOT_NAME_LEN - 1) nl = SLOT_NAME_LEN - 1;
                    memcpy(nm, gap_event_inquiry_result_get_name(packet), nl);
                    nm[nl] = 0;
                    write_slot_name(currect_slot, nm);
                    settings_saved_add(device_addr, nm);
                } else {
                    write_slot_name(currect_slot, "");
                    settings_saved_add(device_addr, "");
                }
                led_set_mode(currect_slot == 0x2 ? LED_CONN_SLOT2 : LED_CONN_SLOT1);
                avdtp_source_establish_stream();
            }
            break;
        case GAP_EVENT_INQUIRY_COMPLETE:
            if (s_list_mode){
                /* Two inquiry rounds (~30 s), then present what we found. */
                if (++s_scan_rounds >= 2){
                    scan_active = false;
                    s_pairing = false;
                    led_set_mode(LED_IDLE);
                    printf("Web scan done: %u device(s)\n", s_found_n);
                } else {
                    gap_inquiry_start(A2DP_SOURCE_DEMO_INQUIRY_DURATION_1280MS);
                }
                break;
            }
            if (scan_active){
                printf("No Bluetooth speakers found, scanning again...\n");
                gap_inquiry_start(A2DP_SOURCE_DEMO_INQUIRY_DURATION_1280MS);
            }
            break;
        default:
            break;
    }
}


void get_link_keys(void){
    bd_addr_t  addr;
    link_key_t link_key;
    link_key_type_t type;
    btstack_link_key_iterator_t it;
    const char * addr_str;

    int ok = gap_link_key_iterator_init(&it);
    if (!ok) {
        printf("Link key iterator not implemented\n");
        return;
    }
    printf("Stored First link key: \n");

    if (gap_link_key_iterator_get_next(&it, addr, link_key, &type)){
        addr_str = bd_addr_to_str(addr);
        printf("%s - type %u, key: ", addr_str, (int) type);
        printf_hexdump(link_key, 16);
        strncpy(device_addr_string, addr_str, sizeof(device_addr_string) - 1);
        sscanf_bd_addr(device_addr_string, device_addr_list[0]);
    }

    //sscanf_bd_addr(device_addr_string, device_addr);

    if (gap_link_key_iterator_get_next(&it, addr, link_key, &type)){
        addr_str = bd_addr_to_str(addr);
        printf("%s - type %u, key: ", addr_str, (int) type);
        printf_hexdump(link_key, 16);
        strncpy(device_addr_string, addr_str, sizeof(device_addr_string) - 1);
        sscanf_bd_addr(device_addr_string, device_addr_list[1]);
    }

    /* ...and the REST (the old dump stopped at two — made the TLV look like
       it lost keys for anything beyond the first pair of devices). */
    while (gap_link_key_iterator_get_next(&it, addr, link_key, &type)){
        printf("%s - type %u, key: ", bd_addr_to_str(addr), (int) type);
        printf_hexdump(link_key, 16);
    }

    printf(".\n");
    gap_link_key_iterator_done(&it);

    uint8_t currect_slot = read_uint8_last_flash();

    printf("currect slot iss %d\n", currect_slot);

    if(currect_slot == 0x1){
        led_set_mode(LED_CONN_SLOT1);
        bd_addr_t  addr_flash_slot1;
        read_slot1_mac(addr_flash_slot1);
        printf("cur slot1 flash addr is %s\n ", bd_addr_to_str(addr_flash_slot1));
        memcpy(device_addr, addr_flash_slot1, sizeof(bd_addr_t)); 
    }

    if(currect_slot == 0x2){
        //printf("currect slot is 2\n");
        led_set_mode(LED_CONN_SLOT2);
        bd_addr_t  addr_flash_slot2;
        read_slot2_mac(addr_flash_slot2);
        printf("cur slot2 flash addr is %s\n ", bd_addr_to_str(addr_flash_slot2));
        memcpy(device_addr, addr_flash_slot2, sizeof(bd_addr_t)); 
    }
    
}


static btstack_packet_callback_registration_t hci_event_callback_registration;


static void packet_handler_inner(uint8_t packet_type, uint16_t channel, uint8_t *packet, uint16_t size);
static void packet_handler(uint8_t packet_type, uint16_t channel, uint8_t *packet, uint16_t size){
    watchdog_hw->scratch[7] = 0x72;   /* entered (hang forensics) */
    packet_handler_inner(packet_type, channel, packet, size);
    watchdog_hw->scratch[7] = 0x7F;   /* exited cleanly */
}
static void packet_handler_inner(uint8_t packet_type, uint16_t channel, uint8_t *packet, uint16_t size){
    UNUSED(size);
    UNUSED(channel);
    bd_addr_t local_addr;
    if (packet_type != HCI_EVENT_PACKET) return;
    switch(hci_event_packet_get_type(packet)){
        case BTSTACK_EVENT_STATE:
            if (btstack_event_state_get_state(packet) != HCI_STATE_WORKING) return;
            gap_local_bd_addr(local_addr);
            printf("BTstack up and running on %s.\n", bd_addr_to_str(local_addr));
            if (memcmp(settings()->bt_mac, local_addr, 6) != 0) {
                memcpy(settings()->bt_mac, local_addr, 6);
                settings_mark_dirty();   /* USB name suffix reads this cache */
            }
            {   /* license binds bt_mac: a blob that failed at boot only
                   because the MAC cache was empty passes now */
                extern void license_recheck(void);
                license_recheck();
            }
            snprintf(s_bt_name, sizeof s_bt_name, "USBPods %02X%02X",
                     local_addr[4], local_addr[5]);
            gap_set_local_name(s_bt_name);
            printf("BT name: %s\n", s_bt_name);
            break;
        default:
            break;
    }
}


static btstack_packet_callback_registration_t hci_state_callback_registration;

void bt_hci_init(void){

    /* SEPARATE registration node: this struct was reused for BOTH handlers
       below — the second hci_add_event_handler re-linked the SAME node with
       its callback overwritten, so packet_handler (BT-up MAC capture, the
       "BTstack up and running" print) NEVER received a single event. */
    hci_state_callback_registration.callback = &packet_handler;
    hci_add_event_handler(&hci_state_callback_registration);

    // Request role change on reconnecting headset to always use them in slave mode
    hci_set_master_slave_policy(0);
    // enabled EIR
    hci_set_inquiry_mode(INQUIRY_MODE_RSSI_AND_EIR);

    // Set local name with a template Bluetooth address, that will be automatically
    // replaced with a actual address once it is available, i.e. when BTstack boots
    // up and starts talking to a Bluetooth module.
    {   /* BTstack LATCHES the local name at power-on — a later
           gap_set_local_name doesn't reach the air (HW: phone scan showed
           plain "USBPods"). Build the suffixed name BEFORE power-up from
           the settings MAC cache; all-zero only on a chip's first-ever
           boot (the HCI-up handler below writes the cache for next time). */
        const uint8_t *m = settings()->bt_mac;
        if (m[0] | m[1] | m[2] | m[3] | m[4] | m[5])
            snprintf(s_bt_name, sizeof s_bt_name, "USBPods %02X%02X", m[4], m[5]);
    }
    gap_set_local_name(s_bt_name);
    /* NOT discoverable by default — a web button opens a 60 s pairing
       window (bt_phone_pair_window). CoD = Audio service + Audio/Video
       major + Loudspeaker minor so phone UIs group us under audio devices
       when the window is open. Known phones reconnect regardless (page
       scan stays on — connectable != discoverable). */
    gap_discoverable_control(0);
    gap_set_class_of_device(0x200414);

    /* Register for HCI events */
    hci_event_callback_registration.callback = &hci_packet_handler;
    hci_add_event_handler(&hci_event_callback_registration);

    get_link_keys();

}