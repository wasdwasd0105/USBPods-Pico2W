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
#include "../settings.h"         /* hfp_dis kill switch */
#include "btstack_avdtp_source.h" /* a2dp_source_is_playing */

/* SDP: 0x10001 AVRCP TG, 0x10002 A2DP src, 0x10003 AVRCP CT, 0x10004 relay sink */
#define HFP_BATTERY_SDP_HANDLE 0x10005
#define HFP_BATTERY_RFCOMM_CH  1

#define DIAL_DELAY_MS   2000    /* post-signaling settle before the SLC dial  */
#define DIAL_DEFER_MS   5000    /* re-check cadence while playing / relay busy */

static int8_t  s_biev_pct  = -1;   /* HF Indicator #2, 0-100                  */
static int8_t  s_apple_pct = -1;   /* IPHONEACCEV key 1, scaled (lvl+1)*10    */
static bool    s_slc_up;
static bool    s_dialed;           /* one dial per ACL — no retry storms      */
static bool    s_vra_active;
static bd_addr_t s_peer;
static btstack_timer_source_t s_dial_timer;
static uint8_t s_redials;          /* bounded SLC re-dials after a mid-link drop */
/* Peers that tore the session down when our SLC came up. RAM-only and tiny:
   the point is to stop the SECOND collapse in a session, not to remember
   forever - hfp_dis is the durable answer. A Bose QC (field report, 1.0.1)
   answers our SLC, then immediately closes AVDTP, HFP, AVRCP and the media
   channel while KEEPING the ACL, which poisons every later reconnect. */
static bd_addr_t s_hostile[2];
static uint8_t   s_hostile_n;
static uint32_t  s_slc_up_ms;      /* when an SLC WE dialed last came up    */
static bool      s_slc_ours;       /* that SLC was our dial, not incoming   */
static bool      s_slc_dialed_by_us; /* we called establish() this session   */
static bool      s_ag_ready;       /* init() really registered the AG        */
static uint32_t  s_dial_sent_ms;   /* when we called establish()              */
/* A real outbound SLC costs an SDP query + RFCOMM setup + ~7 AT round trips.
   Anything faster is the PEER's inbound SLC completing - and BTstack's
   establish() is a silent no-op when a connection for that address already
   exists below SLC-established, so our flag would otherwise credit us for
   the headset's own dial and blame it for any later drop. */
#define HFP_MIN_OWN_DIAL_MS 250

static bool hfp_peer_hostile(bd_addr_t a){
    for (uint8_t i = 0; i < s_hostile_n; i++)
        if (memcmp(s_hostile[i], a, sizeof(bd_addr_t)) == 0) return true;
    return false;
}
static void hfp_peer_mark_hostile(bd_addr_t a){
    if (hfp_peer_hostile(a)) return;
    if (s_hostile_n < (uint8_t)(sizeof(s_hostile)/sizeof(s_hostile[0]))){
        memcpy(s_hostile[s_hostile_n++], a, sizeof(bd_addr_t));
    } else {                       /* keep the newest, drop the oldest */
        memmove(s_hostile[0], s_hostile[1], sizeof(bd_addr_t));
        memcpy(s_hostile[1], a, sizeof(bd_addr_t));
    }
    printf("[hfp] %s tore the link down on SLC - no more battery SLC for it "
           "this session (settings: HFP battery off to make it permanent)\n",
           bd_addr_to_str(a));
}

/* Called from the AVDTP teardown path. A session that dies within this window
   of our SLC coming up did not die of natural causes - our optional battery
   channel is the only thing that changed. */
#define HFP_BLAME_WINDOW_MS 3000
void hfp_battery_note_link_teardown(bool local){
    if (!s_slc_up_ms) return;
    /* Blame only what we can actually be responsible for: an SLC WE dialed,
       a teardown the local side did NOT ask for, and a collapse close enough
       to the SLC to be attributable. A headset that dialed the AG itself, or
       a link the user dropped ('d', slot switch, pairing), must never latch -
       that would silently delete battery for a healthy peer. */
    if (!local && s_slc_ours &&
        btstack_run_loop_get_time_ms() - s_slc_up_ms <= HFP_BLAME_WINDOW_MS)
        hfp_peer_mark_hostile(s_peer);
    s_slc_up_ms = 0;
    s_slc_ours  = false;
}

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
static bool s_sdp_registered;

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
            /* BEFORE the latch below overwrites it: s_dialed is set by
               dial_timer_handler just before it calls establish(), so its
               value HERE is what distinguishes our dial from an SLC the
               headset opened itself. Reading it after the assignment would
               make every SLC look like ours and blame innocent peers. */
            bool slc_was_ours = s_slc_dialed_by_us && s_dial_sent_ms &&
                (btstack_run_loop_get_time_ms() - s_dial_sent_ms) >= HFP_MIN_OWN_DIAL_MS;
            s_slc_dialed_by_us = false;
    s_dial_sent_ms = 0;   /* CONSUME it: otherwise a later
                                             peer-initiated SLC on the same
                                             ACL inherits "ours" and an
                                             unrelated collapse blames a
                                             healthy headset. */
            s_dialed = true;              /* even on failure: one attempt per ACL */
            if (status == ERROR_CODE_SUCCESS){
                s_slc_up = true;
                s_slc_ours  = slc_was_ours;
                s_slc_up_ms = btstack_run_loop_get_time_ms();
                if (!s_slc_up_ms) s_slc_up_ms = 1;
                printf("[hfp] SLC up\n");
            } else {
                printf("[hfp] SLC failed status 0x%02x (no retry this link)\n", status);
            }
            break;
        }
        case HFP_SUBEVENT_SERVICE_LEVEL_CONNECTION_RELEASED:
            s_slc_up = false; s_vra_active = false;
            /* Battery values survive a mid-link SLC drop deliberately: the
               headset ITSELF dials HFP (it thinks we are a phone), sometimes
               racing our timer — the race's loser gets torn down and this
               event fires while the A2DP link is fine. Wiping here lost a
               freshly-reported value (HW: Bose, power-on auto-connect).
               The true end of the link wipes in hfp_battery_link_down().
               And because a lost race is transient, re-dial — bounded, so a
               headset that genuinely keeps hanging up gets 3 attempts total,
               never a storm. */
            printf("[hfp] SLC released\n");
            if (s_redials < 2){
                s_redials++;
                s_dialed = false;
                btstack_run_loop_remove_timer(&s_dial_timer);
                btstack_run_loop_set_timer(&s_dial_timer, DIAL_DEFER_MS);
                btstack_run_loop_add_timer(&s_dial_timer);
            }
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

    if (settings()->hfp_dis){          /* user escape hatch */
        s_dialed = true;
        return;
    }
    if (hfp_peer_hostile(s_peer)){     /* it killed the link last time */
        s_dialed = true;
        return;
    }

    /* AirPods: AAP owns them (exact per-pod percentages); a second control
       surface on the same buds risks disturbing it. Real AirPods have
       answered AAP well before this timer fires. */
    if (aap_protocol_seen()){
        s_dialed = true;
        printf("[hfp] AirPods (AAP live) — no SLC\n");
        return;
    }
    /* NEVER dial into audio that is actually playing. The old rule only
       deferred while the stream was FRESH (<5 s) and then dialed into the
       live stream anyway - which is exactly what killed the Bose: music was
       up, we forced an SLC in, and it reset the whole profile stack (field
       report, fw 1.0.1). Battery percentage is a nicety; audio is the
       product. Defer while playing, then give up quietly - the sink can
       still dial US at any time, and most do.
       a2dp_source_is_playing() (stream up AND host feeding), NOT
       check_is_streaming(): the latter is the AVDTP STARTED latch, which
       stays true for the whole session once set, so gating on it would
       never dial for ANY headset - silently deleting the feature. */
    if (bt_sink_relay_streaming() || a2dp_source_is_playing()){
        /* Keep waiting, do NOT give up: a2dp_source_is_playing() stays true
           for as long as the user listens, so a bounded defer would expire
           mid-album and the battery would simply never appear for anyone who
           plays for more than ~30 s - the same silent feature deletion, just
           slower. Re-arming means the SLC goes out at the first pause, which
           costs nothing and never interrupts audio. The timer dies with the
           link (hfp_battery_link_down), so this cannot outlive the session. */
        btstack_run_loop_set_timer(&s_dial_timer, DIAL_DEFER_MS);
        btstack_run_loop_add_timer(&s_dial_timer);
        return;
    }
    s_dialed = true;
    s_slc_dialed_by_us = true;   /* set ONLY here - the give-up paths above
                                    also set s_dialed, so s_dialed alone would
                                    mark peer-initiated SLCs as ours and blame
                                    innocent headsets. */
    s_dial_sent_ms = btstack_run_loop_get_time_ms();
    if (!s_dial_sent_ms) s_dial_sent_ms = 1;
    printf("[hfp] dialing SLC\n");
    hfp_ag_establish_service_level_connection(s_peer);
}

void hfp_battery_arm_dial(bd_addr_t addr){
    /* Gate on what init ACTUALLY did, never on the live setting: WCMD 46
       writes settings()->hfp_dis into the RAM cache immediately, so a user
       who disables, reboots, then re-enables from the web page would pass a
       live-setting check while the AG was never initialised - arming a timer
       whose .process is still NULL (BTstack dispatches it unguarded) and
       branching to address 0. Reboot-to-apply, enforced here. */
    if (!s_ag_ready) return;
    memcpy(s_peer, addr, sizeof(bd_addr_t));
    s_redials = 0;
    s_slc_dialed_by_us = false;
    /* most headsets dial RFCOMM to us within ~1 s of seeing the AG record;
       incoming auto-creates the AG connection and this timer then no-ops */
    btstack_run_loop_remove_timer(&s_dial_timer);
    btstack_run_loop_set_timer(&s_dial_timer, DIAL_DELAY_MS);
    btstack_run_loop_add_timer(&s_dial_timer);
}

void hfp_battery_link_down(void){
    btstack_run_loop_remove_timer(&s_dial_timer);
    s_slc_up = false; s_dialed = false; s_vra_active = false;
    s_redials = 0;
    s_biev_pct = s_apple_pct = -1;
}

void hfp_battery_note_stream_started(void){
    /* Vestigial: the dial used to defer on "stream younger than 5 s" via a
       timestamp kept here. The gate is now a2dp_source_is_playing(), read
       live, so there is nothing to stamp. Kept as a no-op because the AVDTP
       source calls it unconditionally on stream-established. */
}

void hfp_battery_init(void){
    /* The kill switch has to cover the INCOMING path too. Gating only our
       outgoing dial leaves the AG service registered, and BTstack accepts a
       headset-initiated SLC on its own - so a sink that reacts badly to an AG
       on the link would still get one. Skipping init means no rfcomm service
       and no AG at all; register_sdp() below then publishes no record either,
       so nothing advertises HFP. Reboot to apply, as the setting says. */
    /* Install the timer handler unconditionally, before any early return:
       a queued timer with a NULL .process is a branch to zero, and this
       function has an early return below. Cheap insurance against every
       future edit that adds another one. */
    btstack_run_loop_set_timer_handler(&s_dial_timer, &dial_timer_handler);
    if (settings()->hfp_dis){
        printf("[hfp] battery AG disabled by setting\n");
        return;
    }
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
    s_ag_ready = true;   /* arm_dial gates on THIS, not the live setting */
}

void hfp_battery_register_sdp(void){
    if (settings()->hfp_dis) return;   /* no AG -> advertise no AG record */
    hfp_ag_create_sdp_record_with_codecs(s_sdp_buf, HFP_BATTERY_SDP_HANDLE,
        HFP_BATTERY_RFCOMM_CH, "USBPods AG",
        0 /* no call reject */, 0 /* SDP SupportedFeatures: none */,
        sizeof(s_codecs), s_codecs);
    sdp_register_service(s_sdp_buf);
    s_sdp_registered = true;
}

/* Hide the AG record while the phone-pairing window is open. A pairing phone
   browses our SDP at its most suspicious moment, and an Audio Gateway record
   on a headphones-class device reads as "this is another phone" — observed
   as the phone dropping the ACL (reason 0x13) mid-pairing and reporting
   "pairing unsuccessful" the moment this record first shipped. Headsets are
   unaffected: they browse on connect, not during our phone window, and the
   outgoing SLC dial never needs our own record. */
void hfp_battery_sdp_hide(bool hide){
    /* With the AG disabled, register_sdp() never staged s_sdp_buf, so the
       restore branch below would hand sdp_register_service() 150 zero bytes
       and latch s_sdp_registered on a record that does not exist. Harmless
       today (the parser rejects DE_NIL and returns SDP_HANDLE_INVALID) but
       it prints "AG record restored" for nothing and inverts our own state. */
    if (!s_ag_ready) return;
    if (hide && s_sdp_registered){
        sdp_unregister_service(HFP_BATTERY_SDP_HANDLE);
        s_sdp_registered = false;
        printf("[hfp] AG record hidden (phone pairing)\n");
    } else if (!hide && !s_sdp_registered){
        sdp_register_service(s_sdp_buf);   /* record bytes still staged */
        s_sdp_registered = true;
        printf("[hfp] AG record restored\n");
    }
}
