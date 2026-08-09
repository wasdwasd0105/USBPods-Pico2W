/* HFP-AG, battery-only. See the header for what; this file is mostly WHY.
 *
 * The SLC is established once per ACL, from a deferred timer, and the AG
 * advertises exactly ONE feature: HF Indicators. Everything else — voice
 * recognition, in-band ring, three-way calling, codec negotiation — is
 * deliberately absent, because each of those bits is a spec-legal reason for
 * the headset to open a SCO voice link, and a SCO link steals ACL bandwidth
 * from the A2DP stream. Stock hfp_ag auto-accepts headset-initiated SCO with
 * no app veto (hfp.c flags accept_sco on the HCI request; hfp_ag_run accepts
 * unconditionally), so refusal has to be reactive: on AUDIO_CONNECTION_
 * ESTABLISHED we release it within one event cycle. Do NOT try to reject SCO
 * with gap_register_classic_connection_filter instead — hfp_ag still sends
 * its accept, the command-status error is unhandled, and its internal
 * hfp_sco_establishment_active latches forever: the AG wedges.
 *
 * Battery events run in the BT background IRQ like every other BTstack
 * handler here: store-and-return only (the AVRCP battery handler pattern).
 */
#include <stdio.h>
#include <string.h>

#include "btstack.h"
#include "classic/hfp_ag.h"
#include "hardware/watchdog.h"   /* scratch[7] hang forensics, 0x80/0x8F */

#include "btstack_hfp_battery.h"
#include "btstack_aap.h"         /* aap_protocol_seen: skip AirPods (AAP owns them) */
#include "btstack_sink_relay.h"  /* bt_sink_relay_streaming: defer the dial */

/* SDP: 0x10001 AVRCP TG, 0x10002 A2DP src, 0x10003 AVRCP CT, 0x10004 relay sink */
#define HFP_BATTERY_SDP_HANDLE 0x10005
#define HFP_BATTERY_RFCOMM_CH  1

#define DIAL_DELAY_MS   2000    /* post-signaling settle before the SLC dial  */
#define DIAL_DEFER_MS   5000    /* back off while streaming is fresh / relay  */
#define DIAL_DEFER_MAX  6

static int8_t  s_biev_pct  = -1;   /* HF Indicator #2, 0-100                  */
static int8_t  s_apple_pct = -1;   /* IPHONEACCEV key 1, scaled (lvl+1)*10    */
static bool    s_slc_up;
static bool    s_dialed;           /* one dial per ACL — no retry storms      */
static bool    s_vra_active;
static bd_addr_t s_peer;
static btstack_timer_source_t s_dial_timer;
static uint8_t s_dial_defers;
static uint32_t s_stream_started_ms;

/* canonical AG indicator table (hfp_ag_demo.c) — AT+CIND needs it to
   complete the SLC; we never change any of these values */
static const hfp_ag_indicator_t s_ag_indicators[] = {
    {1, "service",   0, 1, 1, 0, 0, 0},
    {2, "call",      0, 1, 0, 1, 1, 0},
    {3, "callsetup", 0, 3, 0, 1, 1, 0},
    {4, "battchg",   0, 5, 3, 0, 0, 0},
    {5, "signal",    0, 5, 5, 0, 1, 0},
    {6, "roam",      0, 1, 0, 0, 1, 0},
    {7, "callheld",  0, 2, 0, 1, 1, 0},
};
/* MUST be {uuid 1},{uuid 2} IN ORDER: AT+BIEV's assigned number is used as a
   1-based ARRAY INDEX by this BTstack (hfp_parse_indicator_index), not a uuid
   lookup. A battery-only {{2,1}} table sends every AT+BIEV=2,x into the
   out-of-range ERROR branch — silent total failure. */
static const hfp_generic_status_indicator_t s_hf_indicators[] = { {1, 1}, {2, 1} };

static const uint8_t s_codecs[] = { HFP_CODEC_CVSD };

static uint8_t s_sdp_buf[150];

int8_t hfp_battery_get_percent(void){
    return s_biev_pct >= 0 ? s_biev_pct : s_apple_pct;
}

static void hfp_battery_handler_inner(uint8_t packet_type, uint8_t *packet, uint16_t size);
static void hfp_battery_packet_handler(uint8_t packet_type, uint16_t channel,
                                       uint8_t *packet, uint16_t size){
    (void)channel;
    watchdog_hw->scratch[7] = 0x80;   /* entered (hang forensics) */
    hfp_battery_handler_inner(packet_type, packet, size);
    watchdog_hw->scratch[7] = 0x8F;   /* exited cleanly */
}

static void hfp_battery_handler_inner(uint8_t packet_type, uint8_t *packet, uint16_t size){
    if (packet_type != HCI_EVENT_PACKET) return;
    if (hci_event_packet_get_type(packet) != HCI_EVENT_HFP_META) return;

    switch (hci_event_hfp_meta_get_subevent_code(packet)){
        case HFP_SUBEVENT_SERVICE_LEVEL_CONNECTION_ESTABLISHED: {
            uint8_t status = hfp_subevent_service_level_connection_established_get_status(packet);
            s_dialed = true;              /* even on failure: one attempt per ACL */
            if (status == ERROR_CODE_SUCCESS){
                s_slc_up = true;
                printf("[hfp] SLC up\n");
            } else {
                printf("[hfp] SLC failed status 0x%02x (no retry this link)\n", status);
            }
            break;
        }
        case HFP_SUBEVENT_SERVICE_LEVEL_CONNECTION_RELEASED:
            s_slc_up = false; s_vra_active = false;
            s_biev_pct = s_apple_pct = -1;
            printf("[hfp] SLC released\n");
            break;

        case HFP_SUBEVENT_HF_INDICATOR:     /* PRIMARY: AT+BIEV=2,<0-100> */
            if (hfp_subevent_hf_indicator_get_uuid(packet) == HFP_HF_INDICATOR_UUID_BATTERY_LEVEL){
                uint8_t v = hfp_subevent_hf_indicator_get_value(packet);
                if (v <= 100){
                    s_biev_pct = (int8_t)v;
                    printf("[hfp] battery %u%% (HF indicator)\n", v);
                }
            }
            break;

        case HFP_SUBEVENT_APPLE_BATTERY_LEVEL: {
            /* RAW offset, not the generated getter: this BTstack's emitter
               writes the 0-9 level at packet[3] in a 4-byte event with no
               acl_handle; the getters assume a 6-byte layout and read past
               the payload. Verified against hfp_ag.c's emitter. */
            if (size < 4) break;
            uint8_t lvl = packet[3];
            if (lvl <= 9){
                s_apple_pct = (int8_t)((lvl + 1) * 10);
                printf("[hfp] battery %d%% (IPHONEACCEV %u)\n", s_apple_pct, lvl);
            }
            break;
        }

        case HFP_SUBEVENT_VOICE_RECOGNITION_ACTIVATED:
            /* AT+BVRA auto-OKs in stock code with NO feature-bit gate — a
               non-compliant headset can reach here despite our BRSF. Track
               it: the SCO teardown below must deactivate VRA FIRST (release
               returns COMMAND_DISALLOWED while VRA is active, and the
               deactivate path drops the SCO it opened). */
            s_vra_active = true;
            break;
        case HFP_SUBEVENT_VOICE_RECOGNITION_DEACTIVATED:
            s_vra_active = false;
            break;

        case HFP_SUBEVENT_AUDIO_CONNECTION_ESTABLISHED: {
            /* No call ever exists here, so ANY SCO is unwanted: tear it down
               within this event cycle (see file header for why not an HCI
               filter). One HCI round trip of SCO, no sustained reservation. */
            hci_con_handle_t acl = hfp_subevent_audio_connection_established_get_acl_handle(packet);
            printf("[hfp] unexpected SCO — releasing\n");
            if (s_vra_active) hfp_ag_deactivate_voice_recognition(acl);
            else              hfp_ag_release_audio_connection(acl);
            break;
        }

        case HFP_SUBEVENT_PLACE_CALL_WITH_NUMBER:   /* ATD from a weird HF */
            hfp_ag_outgoing_call_rejected();
            break;

        default:
            break;
    }
}

/* ---- SLC dial timing ----------------------------------------------------
   Once per ACL, post-signaling, and never against fresh streaming: the SLC
   is only ~7 AT round trips but the connect window is the session's most
   fragile moment (AVDTP discovery, jitter handshakes, AAP monsoon). */
static void dial_timer_handler(btstack_timer_source_t *ts){
    (void)ts;
    if (s_dialed || s_slc_up) return;

    /* AirPods: AAP owns them (exact per-pod percentages); a second control
       surface on the same buds risks disturbing it. Real AirPods have
       answered AAP well before this timer fires. */
    if (aap_protocol_seen()){
        s_dialed = true;
        printf("[hfp] AirPods (AAP live) — no SLC\n");
        return;
    }
    bool fresh_stream = s_stream_started_ms &&
        (btstack_run_loop_get_time_ms() - s_stream_started_ms) < 5000;
    if (bt_sink_relay_streaming() || fresh_stream){
        if (++s_dial_defers <= DIAL_DEFER_MAX){
            btstack_run_loop_set_timer(&s_dial_timer, DIAL_DEFER_MS);
            btstack_run_loop_add_timer(&s_dial_timer);
        } else {
            s_dialed = true;   /* give up quietly for this ACL */
        }
        return;
    }
    s_dialed = true;
    printf("[hfp] dialing SLC\n");
    hfp_ag_establish_service_level_connection(s_peer);
}

void hfp_battery_arm_dial(bd_addr_t addr){
    memcpy(s_peer, addr, sizeof(bd_addr_t));
    s_dial_defers = 0;
    /* most headsets dial RFCOMM to us within ~1 s of seeing the AG record;
       incoming auto-creates the AG connection and this timer then no-ops */
    btstack_run_loop_remove_timer(&s_dial_timer);
    btstack_run_loop_set_timer(&s_dial_timer, DIAL_DELAY_MS);
    btstack_run_loop_add_timer(&s_dial_timer);
}

void hfp_battery_link_down(void){
    btstack_run_loop_remove_timer(&s_dial_timer);
    s_slc_up = false; s_dialed = false; s_vra_active = false;
    s_biev_pct = s_apple_pct = -1;
    s_stream_started_ms = 0;
}

void hfp_battery_note_stream_started(void){
    s_stream_started_ms = btstack_run_loop_get_time_ms();
    if (s_stream_started_ms == 0) s_stream_started_ms = 1;
}

void hfp_battery_init(void){
    rfcomm_init();
    hfp_ag_init(HFP_BATTERY_RFCOMM_CH);
    /* HF Indicators ONLY — overrides the 3-way + in-band-ring default. The
       bit is mandatory on BOTH sides for the +BIND leg to run at all. */
    hfp_ag_init_supported_features(1 << HFP_AGSF_HF_INDICATORS);
    hfp_ag_init_codecs(sizeof(s_codecs), s_codecs);
    hfp_ag_init_ag_indicators(7, s_ag_indicators);
    hfp_ag_init_hf_indicators(2, s_hf_indicators);
    /* features bit 1 = battery reporting in the +XAPL reply; without this
       the AG answers ERROR to AT+XAPL/AT+IPHONEACCEV and the Apple path is
       dead. (This tree's hfp_ag already carries the '=' reply-format fix.) */
    hfp_ag_init_apple_identification("USBPods", 2);
    hfp_ag_register_packet_handler(&hfp_battery_packet_handler);
    btstack_run_loop_set_timer_handler(&s_dial_timer, &dial_timer_handler);
}

void hfp_battery_register_sdp(void){
    hfp_ag_create_sdp_record_with_codecs(s_sdp_buf, HFP_BATTERY_SDP_HANDLE,
        HFP_BATTERY_RFCOMM_CH, "USBPods AG",
        0 /* no call reject */, 0 /* SDP SupportedFeatures: none */,
        sizeof(s_codecs), s_codecs);
    sdp_register_service(s_sdp_buf);
}
