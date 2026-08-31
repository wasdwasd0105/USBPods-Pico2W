#include <sys/cdefs.h>
#include "hardware/watchdog.h"   /* scratch[7] hang forensics */
/*
 * Copyright (C) 2016 BlueKitchen GmbH
 *
 * Redistribution and use in source and binary forms, with or without
 * modification, are permitted provided that the following conditions
 * are met:
 *
 * 1. Redistributions of source code must retain the above copyright
 *    notice, this list of conditions and the following disclaimer.
 * 2. Redistributions in binary form must reproduce the above copyright
 *    notice, this list of conditions and the following disclaimer in the
 *    documentation and/or other materials provided with the distribution.
 * 3. Neither the name of the copyright holders nor the names of
 *    contributors may be used to endorse or promote products derived
 *    from this software without specific prior written permission.
 * 4. Any redistribution, use, or modification is done solely for
 *    personal benefit and not for any commercial purpose or for
 *    monetary gain.
 *
 * THIS SOFTWARE IS PROVIDED BY BLUEKITCHEN GMBH AND CONTRIBUTORS
 * ``AS IS'' AND ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT
 * LIMITED TO, THE IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS
 * FOR A PARTICULAR PURPOSE ARE DISCLAIMED. IN NO EVENT SHALL MATTHIAS
 * RINGWALD OR CONTRIBUTORS BE LIABLE FOR ANY DIRECT, INDIRECT,
 * INCIDENTAL, SPECIAL, EXEMPLARY, OR CONSEQUENTIAL DAMAGES (INCLUDING,
 * BUT NOT LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS OR SERVICES; LOSS
 * OF USE, DATA, OR PROFITS; OR BUSINESS INTERRUPTION) HOWEVER CAUSED
 * AND ON ANY THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT LIABILITY,
 * OR TORT (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY OUT OF
 * THE USE OF THIS SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF
 * SUCH DAMAGE.
 *
 * Please inquire about commercial licensing options at 
 * contact@bluekitchen-gmbh.com
 *
 */


/*
 * 
*/

#include <stdint.h>
#include <stdatomic.h>
#include <stdio.h>
#include <string.h>
#include "btstack.h"
#include "btstack_avdtp_source.h"
#include "../tinyusb/uac.h"
#include "btstack_aap.h"
#include "btstack_hfp_battery.h"
#include "pico/multicore.h"
#include "pico/util/queue.h"
#include "hardware/sync.h"      /* IRQ-off windows for main-loop FDK mallocs */

#include "btstack_hci.h"

#include "../pico_w_led.h"
#include "../settings.h"
#include "../console.h"
#include "a2dp_engine.h"
#include "codec/codec.h"
#include "../resamp44.h"
#include "btstack_sink_relay.h"

/* HAVE_AAC_FDK is a TARGET-WIDE define (CMakeLists.txt): it used to live
   here, and when the codecs were split into src/btstack/codec/*.c the AAC
   module's own #ifdef HAVE_AAC_FDK could no longer see it — its stream
   endpoint was silently never created, so every AAC-LC headset fell back to
   SBC with "Set AAC Connection Result is 2". Never define it per-file. */

#define HAVE_LDAC_ENCODER
#include <ldacBT.h>

#define A2DP_CODEC_VENDOR_ID_SONY 0x12d
#define A2DP_SONY_CODEC_LDAC 0xaa

#define A2DP_CODEC_VENDOR_ID_APPLE 0x004C
#define A2DP_APPLE_CODEC_AAC_ELD 0x8001


#include <aacenc_lib.h>


#define A2DP_CODEC_VENDOR_ID_FRAUNHOFER 0x08A9
#define A2DP_FRAUNHOFER_CODEC_LC3PLUS 0x0001

/* AVDTP_MAX_* moved to a2dp_engine.h */

#define VOLUME_REDUCTION 2





#ifdef HAVE_BTSTACK_STDIN

#endif

/* a2dp_media_sending_context_t moved to a2dp_engine.h */



#ifdef HAVE_AAC_FDK
HANDLE_AACENCODER handleAAC;    /* shared with codec_aac/codec_aaceld (codec_fdk.h) */
AACENC_InfoStruct aacinf;
#endif

a2dp_media_sending_context_t media_tracker;

static int current_sample_rate = 48000;

static uint8_t sdp_avdtp_source_service_buffer[150];

uint16_t     num_remote_seps;
static uint16_t     cur_num_remote_seps;

static uint8_t cur_capability = 0;
/* Async SET_CONFIGURATION reject handling: which codec we last offered, and
   the ones this session's sink has already refused (so the ladder makes
   progress instead of re-offering a rejected codec forever). Cleared when a
   fresh capability scan starts. */
static uint8_t src_attempt_codec;
static uint8_t src_codec_reject_mask;
int set_next_codec(uint8_t num);   /* defined below — the reject handler above
                                      it re-runs the ladder */
static bool is_streaming = false;

/* WEDGED-PEER BRAKE (field report, fw 1.0.1 - Bose QC endless reconnect).
   The only automatic loop-stopper used to be a2dp_source_remote_bye(), which
   needs an HCI disconnect with reason 0x13/0x15. A sink can instead close
   every profile channel and KEEP the ACL alive: no disconnect event ever
   arrives, that brake never fires, and connmode-3 refills the dial budget
   every 10 s - an infinite loop by construction (37 cycles on ONE ACL handle
   in the field log, the headset chiming each time).

   The success test is REACHING AUDIO, deliberately not "an AVDTP command was
   accepted": BTstack emits SIGNALING_ACCEPT for DISCOVER as well, so a peer
   that answers discovery and then dies would look like progress forever. A
   session that never streams is a failed session, whatever it answered.

   The hold needs its own flag. src_user_dc_hold cannot serve: it is cleared
   at every SIGNALING_CONNECTION_ESTABLISHED (:'a live link voids the d hold')
   - which a wedged peer reaches on every single cycle - and again by
   uac.c's unconditional playback-stopped clear. Both would unlatch this
   brake within one cycle. src_wedge_hold is cleared ONLY by deliberate user
   intent (a2dp_source_wedge_clear from the connect entry points). */
#define SRC_WEDGE_MAX 4
static uint8_t src_nostream_run;     /* consecutive sessions that never streamed */
static bool    src_session_streamed; /* this session reached AVDTP STREAMING     */
static bool    src_wedge_hold;       /* wedged peer: stop dialing until asked    */

bool a2dp_source_wedge_hold(void){ return src_wedge_hold; }
void a2dp_source_wedge_clear(void){ src_wedge_hold = false; src_nostream_run = 0; }
uint8_t cur_codec = 0;
static int8_t  s_avrcp_batt = -1;   /* AVRCP battery enum, -1 = not reported */

uint8_t get_cur_codec(void)    { return cur_codec; }
int8_t  get_avrcp_battery(void){ return s_avrcp_batt; }

static void avrcp_packet_handler(uint8_t packet_type, uint16_t channel, uint8_t *packet, uint16_t size);
static void avrcp_target_packet_handler(uint8_t packet_type, uint16_t channel, uint8_t *packet, uint16_t size);
static void avrcp_controller_packet_handler(uint8_t packet_type, uint16_t channel, uint8_t *packet, uint16_t size);

typedef struct {
    uint8_t track_id[8];
    uint32_t song_length_ms;
    avrcp_playback_status_t status;
    uint32_t song_position_ms; // 0xFFFFFFFF if not supported
} avrcp_play_status_info_t;


avrcp_track_t tracks[] = {
    {{0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x02}, 2, "USB Audio", "Pico", "wasdwasd0105", "vivid", 12345},
};

avrcp_play_status_info_t play_info;

static uint8_t sdp_avrcp_target_service_buffer[200];
static uint8_t sdp_avrcp_controller_service_buffer[200];


uint16_t                  media_codec_config_len;
uint8_t                   media_codec_config_data[AVDTP_MAX_MEDIA_CODEC_CONFIG_LEN];

remote_sep_entry_t remote_seps[AVDTP_MAX_SEP_NUM];   /* type: a2dp_engine.h */
uint8_t selected_remote_sep_index;

a2dp_stream_context_t sc;   /* type: a2dp_engine.h */


// setup stream endpoints



static uint8_t is_cmd_triggered_localy = 0;

static bool a2dp_is_connected_flag = false;
static bool src_stream_established = false;  /* MEDIA (streaming) channel up —
                                                set ONLY at STREAMING_CONNECTION
                                                _ESTABLISHED, cleared at releases
                                                and at each fresh signaling
                                                connection */

static bool finish_scan_avdtp_codec = false;
/* A media-codec CONFIGURATION landed on the HEADSET link (matched by
   avdtp_cid, so relay-phone configs never set it). When the sink drives the
   reconnect it can configure us BEFORE our capability scan finishes AND
   before the media channel opens - src_stream_established alone missed that
   window: the select loop fought the remote config with 17 COMMAND_
   DISALLOWED set_configuration attempts, gave up, and the START then found
   finish_scan_avdtp_codec false, so the pump never ran: AVDTP said
   "streaming" while we encoded nothing. Silence until replug (HW: Bose QC,
   the reconnect right after the wedge-brake fix). */
static bool src_codec_configured = false;

uint8_t audio_timer_interval = 5;

// on pico 2w the max stable aac bit rate under 512 simples without vbr is around 220000
uint8_t aac_audio_timer_interval = 12;
uint16_t acc_num_simples = 1024;      /* INPUT frames per encode slot */
uint16_t rtp_samples_per_frame = 1024; /* RTP clock units per frame (codec rate) */
bool     eld_resample = false;         /* 44.1 pipeline -> 48 k ELD encoder */
int16_t  eld_rs_buf[480 * 2];          /* one resampled ELD granule, stereo */





#ifdef HAVE_AAC_FDK

#endif


// configurations for local stream endpoints

#ifdef HAVE_AAC_FDK
#endif

bool get_a2dp_connected_flag(){
    return a2dp_is_connected_flag;
}

static int a2dp_sample_rate(void){
    return current_sample_rate;
}

int get_current_sample_rate(void){ return current_sample_rate; }
void set_current_sample_rate(int hz){ current_sample_rate = hz; }

#ifdef HAVE_AAC_FDK
void codec_fdk_release(void){
    if (handleAAC != NULL) {
        aacEncClose(&handleAAC);
        handleAAC = NULL;
    }
}
#endif

void configure_sample_rate(int sampling_frequency){
    switch (sampling_frequency){
        case AVDTP_SBC_48000:
            current_sample_rate = 48000;
            break;
        case AVDTP_SBC_44100:
            current_sample_rate = 44100;
            break;
        case AVDTP_SBC_32000:
            current_sample_rate = 32000;
            break;
        case AVDTP_SBC_16000:
            current_sample_rate = 16000;
            break;
        default:
            break;
    }

    media_tracker.codec_storage_count = 0;
    media_tracker.samples_ready = 0;
}

static const char * codec_name_for_type(avdtp_media_codec_type_t codec_type){
    switch (codec_type){
        case AVDTP_CODEC_SBC:            return "SBC";
        case AVDTP_CODEC_MPEG_1_2_AUDIO: return "MPEG_1_2_AUDIO";
        case AVDTP_CODEC_MPEG_2_4_AAC:   return "MPEG_2_4_AAC";
        case AVDTP_CODEC_ATRAC_FAMILY:   return "ATRAC_FAMILY";
        case AVDTP_CODEC_NON_A2DP:       return "NON_A2DP/Vendor";
        default:                         return "Unknown";
    }
}

uint32_t src_tx_pkts;   /* media packets actually sent — a frozen value
                                  in the 'R' dump = the source stream is DEAD
                                  (timer stopped / start lost), not just quiet */




static uint8_t aaceld_debug_count = 0;

static uint32_t src_eld_last_send_ms;   /* wall time of the previous ELD packet */
static uint32_t src_ts_snaps;           /* holes accounted into the RTP clock   */
static volatile uint32_t src_started_ms;     /* last START-accept           */
static bool     src_eld_tap;            /* 'E': per-packet ELD one-liners */
/* LIVE SPLICE = NO STAMP (HW-verified both ways): an EP close that lands
   MID-STREAM (Ubuntu/PipeWire Analog<->S/PDIF switch: close + reopen in
   ~50 ms with music flowing) does NOT remove stream time — the pump's
   credit is wall-clock-driven and bridges the closed window with silence
   AUs, so the on-air timeline stays continuous. Any stamp there is a
   PHANTOM clock advance. Two shipped variants proved it on hardware:
   threshold stamping fired ~2 phantoms per 7 switches (flush zeroes the
   credit that would have suppressed them) -> garble then decoder MUTE;
   always-stamping fired on every switch -> same wedge, faster. The relay
   era hit the identical disease ("393 snaps = 10+ s phantom advance ->
   buds muted"). Cure = the TX-stall drop treatment: zero the stamp
   reference in the flush so the next send skips the hole math entirely —
   seq continuous, clock continuous, fully seamless splice. */
static uint32_t src_live_splices;       /* seamless mid-stream EP closes ('R') */
/* SPLICE-CHURN AUTO-RECOVERY: repeated LIVE splices (EP close with PCM
   actively flowing — Ubuntu/PipeWire profile switches) wedge the AirPods'
   ELD decoder even though the stream stays provably clean; the buds emit
   no distress and only a stream refresh ('K') or an AACP jitter-set
   revives them (full story: airpods-eld-splice-wedge.md). Detect >=2 live
   splices in 10 s -> fire ONE automatic 'K' refresh (30 s cooldown), from
   pump-tick context. Idle closes (Windows per-sound, host pauses) never
   count: the close handlers only flag liveness when PCM arrived within
   the last few audio_task passes. */
static uint32_t src_splice_win_ms;          /* 10 s detection window start  */
static uint8_t  src_splice_win_n;           /* live splices in the window   */
volatile bool src_auto_kick_pending; /* consumed by the pump tick    */
static bool src_auto_kick_enable = true;    /* gates the kick machine, storm
                                               ladder and parked-session
                                               watchdog; 'X' key toggles.
                                               Investigation concluded (two
                                               recovery-off walks): full wire
                                               parity — transport AND bitstream
                                               (SBR+VBR) — does not make the
                                               buds self-recover from loss
                                               holes; macOS heals them by
                                               actively changing the stream
                                               (link adaptation). Active
                                               recovery is the correct standing
                                               countermeasure. */
uint32_t usbpods_flush_occurred_count;      /* HCI Flush Occurred (0x11) events,
                                               counted ONCE in btstack_hci.c.
                                               With manual flushing moved to
                                               Enhanced Flush (event 0x39),
                                               any 0x11 can ONLY be the CYW43's
                                               automatic timer firing — this
                                               counter IS the defect verdict */
uint32_t usbpods_enhanced_flush_done_count; /* Enhanced Flush Complete (0x39):
                                               our manual flushes, counted in
                                               btstack_hci.c */
uint32_t usbpods_flushable_tx_count;
static uint32_t src_manual_flushes;     /* HCI_Flush we issued ourselves     */
static bool     src_diag_log;           /* 'V': 1 Hz [diag] telemetry, off   */
bool     src_proto_log;          /* 'C': AVDTP capability/SEP scan +
                                           raw codec-config dumps. Off: every
                                           reconnect used to spill ~40 lines
                                           of SEP enumeration over the console */
static uint32_t src_manflush_ms;        /* time of the last manual flush     */
static uint32_t src_full_since;         /* TX queue continuously-full since; file
                                           scope so timer_start/release can clear
                                           it (a suspend mid-jam froze the old
                                           block-static and fired an early flush
                                           on the first post-resume tick) */
static uint8_t  src_manflush_streak;    /* jam-breakers fired while the queue
                                           stayed CONTINUOUSLY jammed — an
                                           effective flush frees buffers and
                                           resets this via the freeslots path */
static uint32_t src_flush_fallbacks;    /* classic HCI_Flush escalations; while
                                           this is 0, every Flush Occurred
                                           (0x11) is the automatic timer —
                                           nonzero marks contaminated autoflush
                                           data (fallbacks emit 0x11 too) */
static int      src_freeacl_latch = -1; /* last sampled free controller TX bufs */
static uint32_t src_rec_txmark;         /* tx_pkts at the last recovery check */
static volatile bool src_ptype_pending;     /* retry 0xCC18 until it sends   */
static volatile uint8_t src_link_probe_step;/* 'R': async link probe sequencer:
                                               4=rssi 3=lq 2=failed-contact
                                               1=read-back flush timeout      */
/* Read Automatic Flush Timeout (0x0C27) — absent from BTstack's command
   table; verifies the controller actually STORED the 200 ms we wrote. */
static const hci_cmd_t usbpods_hci_read_auto_flush_timeout = { 0x0C27, "H" };
/* Enhanced Flush (0x0C5F, packet_type 0): discards ONLY auto-flushable
   packets — queued AVDTP/AACP control (PB=0b00) survives, unlike HCI_Flush's
   handle-wide dump — and completes with Enhanced Flush Complete (0x39),
   which stock BTstack ignores. HCI_Flush's Flush Occurred (0x11) makes
   stock BTstack DISCONNECT the ACL (only a local SDK patch suppresses
   that), so Enhanced Flush is also the fresh-SDK-safe choice. */
static const hci_cmd_t usbpods_hci_enhanced_flush = { 0x0C5F, "H1" };        /* media ACLs actually sent with
                                               PB=0b10 (SDK patch counts) —
                                               proof the marking chain works */
static uint8_t  src_rec_state;      /* 0 idle, 1 nudge sent, 2 beacon wait,
                                       3 kick beacon wait                    */
uint32_t src_splice_last_ms; /* last live splice (quiesce gate)      */
static uint32_t src_rtp_ms_base;    /* wall-clock base for the 1 kHz RTP stamp */
static uint32_t src_rec_t0;         /* nudge send time                      */
static uint8_t  src_rec_escal;      /* k-escalations this episode (backoff) */
static uint32_t src_auto_nudges;    /* gapless recoveries attempted         */
static uint32_t src_park_starts;    /* parked-session watchdog STARTs       */
/* PREVENTIVE NUDGE: constant cadence took churn tolerance from ~3-7 to ~15
   switches; the residual accumulation is wiped by firing the proven
   inaudible jitter reset (value change -> restore, rebuilds the buds'
   decode pipeline per the 0x0e/0x55 beacons) at EVERY live splice, rate-
   limited, before damage can reach audibility. */
static volatile bool src_pnudge_pending;    /* set per live splice          */
static uint32_t src_pnudge_ms;              /* rate limit (2 s)             */
static uint32_t src_pnudge_restore_ms;      /* pending 150 ms-value restore */
static uint32_t src_pnudges;                /* total fired ('R' report)     */
static uint32_t src_auto_kick_ms;           /* last auto-kick (cooldown)    */
static uint32_t src_auto_kicks;             /* total fired ('R' report)     */

/* USB-context entry point — executes IN USB/tud_task context DELIBERATELY:
   that is the same context as audio_slot_push_samples, so flush and push
   are serialized by construction (they can never interleave on the
   filling-slot state). A deferred-to-pump variant was tried and REVERTED:
   it un-serialized flush vs push, and a push landing mid-flush could see
   filling_slot_valid still true after the slot was already requeued —
   the index then circulated in BOTH queues (two writers tearing one
   buffer = persistent garble that kicks don't clear; HW: 'buggy sound
   needs a long time to recover, no-sound recovers fast'). The original
   credit race this deferral targeted is closed by _Atomic samples_ready:
   the flush's store and the pump's RMWs can no longer tear. */
/* SPLICE-EDGE FADES (prevention, not recovery): the buds' ELD decoder is
   damaged by hard waveform edges at live splices (music cut mid-sample ->
   zeros -> music resuming mid-sample) — every transmitted property was
   proven clean, leaving the edges as the only stimulus. Keep a rolling
   5 ms tail of the last real audio; at a live splice synthesize a ramp-
   down into the first silence slot and ramp the first real slot back up.
   The encoder then sees fast clean envelopes, never a cliff. */
#define SRC_FADE_FRAMES 240              /* 5 ms @ 48 k */
static int16_t src_tail[SRC_FADE_FRAMES * 2];
static volatile bool src_fade_out_pending, src_fade_in_pending;

static void audio_slot_flush_exec(bool live);
void audio_slot_request_flush(bool live) {
    audio_slot_flush_exec(live);
}

/* LIVE-CLOSE GRACE WINDOW: a host profile-switch closes and reopens the EP
   in 40-70 ms. Treating that close as an event at all was the damage — any
   bridge (silence, faded, stamped or seamless) is a discontinuity the buds
   degrade on. So a LIVE close now does NOTHING for up to 80 ms: the pool
   (~60 ms) keeps feeding the encoder, the pump real-waits briefly, and on
   the typical fast reopen the air stream never changes in ANY property.
   Only an expired grace (genuine stop) runs the old close actions — from
   the pump tick, which is push-free by then (EP closed), with the pool
   frozen against the reopen race. Idle closes bypass grace entirely. */
static volatile uint32_t src_grace_deadline;   /* 0 = no grace pending */
void audio_slot_live_close_grace(void) {
    /* REVERTED — see uac.c close handler. Kept for ABI; never armed. */
    (void)src_grace_deadline;
}
void audio_slot_grace_cancel(void) {           /* EP reopened in time */
    src_grace_deadline = 0;
}
/* On-air payload validator: switching wedges survived every clean-stream
   proof (content, clock, seq, cadence all measured clean) — the one layer
   no counter sees is the actual bytes. A single packet whose Apple header
   chain is internally inconsistent desyncs the buds' in-payload demuxer:
   everything after parses as garbage -> garble, then hard MUTE. Walk the
   chain on every send; scream with hex on any violation, and dump the
   first packets after each live splice unconditionally. */
static volatile uint8_t src_splice_dump;   /* packets left to hex-dump      */
static uint32_t src_tx_malformed;          /* header-chain violations ('R') */
/* DELIVERY instrumentation: payloads are proven byte-perfect at the send
   call, yet switching still wedges the buds — the one unwatched seam is
   whether the packet actually LEAVES. Media send rc was always ignored; a
   failed send = on-air Apple seq gap (the documented mute trigger) that
   every device-side counter is blind to. Profile switches also fire host
   volume writes -> AVRCP absolute-volume on the SAME ACL link — count
   those too (storm suspect for the buffer pressure). */
static uint32_t src_send_fails;            /* media sends that returned !=0 */
static uint8_t  src_send_fail_rc;          /* last failing status           */
static uint32_t src_vol_cmds;              /* set_bt_volume-driven AVRCP     */
/* Malformed-packet RAM snapshot: the console ring drops the MALFORMED hex
   lines exactly when they matter (splice bursts flood it). Capture the
   first offender's bytes + walk state here instead — printed by 'R',
   readable over SWD, immune to ring pressure. */
static uint8_t  src_mal_snap[40];
static uint16_t src_mal_len, src_mal_nfr, src_mal_walkoff, src_mal_walkn;
static uint16_t src_mal_seqctr;            /* aaceld_frame_sequence at capture */
static uint16_t aaceld_frame_sequence = 1;   /* Apple 12-bit AU counter (declared
                                                here for the tap; owned by the
                                                encode fill path below) */

void a2dp_source_eld_send(void) {
    /* TIMESTAMP TRUTH: the buds discipline their ELD playout clock on our
       RTP timestamps. A send hole (host EP close, slot starvation, clock
       handoff) used to pass silently — stamps claimed a perfect timeline
       while arrival times slipped, and the desync ACCUMULATED per hole
       (Windows' per-sound EP close/open walked the decoder off in ~3 tones:
       dirty -> mute). Account every real hole into the RTP clock and flag
       it with the RTP marker bit so each gap is an explicit, self-healing
       discontinuity instead of hidden drift. */
    uint32_t now_ms = btstack_run_loop_get_time_ms();
    uint32_t prev_send_ms = src_eld_last_send_ms;   /* pre-update, for the tap */
    uint8_t rtp_marker = 0;
    /* DECODER-LOCK WINDOW: no stamps in the first 3 s after a START — the
       buds are still locking onto the stream, and a clock jump mid-lock is
       audible as garble ('noisy for a short time after reconnect', HW).
       Early gaps become seamless splices instead; the small playout shift
       is absorbed by the jitter buffer. */
    if (src_eld_last_send_ms != 0 && media_tracker.rtp_timestamp != 0 &&
        now_ms - src_started_ms >= 3000) {
        uint32_t gap = now_ms - src_eld_last_send_ms;
        /* A delayed send is NOT automatically a hole: the pump catches up
           with a multi-AU batch whose CONTENT covers most of the gap (pause
           transitions do this constantly — snapping the full gap there
           double-counted time and made resumes dirty). Stamp only the
           EXCESS the batch does not cover: that part is genuinely missing
           audio time (host EP close, true starvation). */
        uint32_t covered = 10u * (uint32_t)((media_tracker.codec_num_frames > 0)
                                            ? media_tracker.codec_num_frames : 1);
        /* PENDING CREDIT COUNTS AS COVERED: the pacing credit delivers the
           rest of a late tick's audio in the immediately-following packets
           — stamping that as a "hole" advanced the RTP clock ~25 ms per
           delayed tick with NOTHING actually missing. Long relay sessions
           accumulated 393 snaps ≈ 10+ s of phantom timestamp advance: the
           buds' playout clock drifted until they MUTED, then self-resynced
           ("no sound after long sbc relay, recovers by itself"). Credit is
           capped at 50 ms, so true outages (EP close, real starvation)
           still stamp their genuine excess. */
        uint32_t credit_ms = (uint32_t)(media_tracker.samples_ready / 48u);
        if (gap > 25 && gap > covered + credit_ms + 15u) {
            /* FLUSH-WORLD RULE (macos-link-recovery-report.md brownout2,
               Priority 1b): DO NOT touch the RTP clock. A delayed send
               loses no content — the controller's 200 ms flush already
               turned staleness into on-air packet loss the buds conceal;
               stamping here is a phantom clock advance (the relay-era mute
               disease). macOS never adjusts its clock through 70 % loss.
               Keep only the RF-stress note for the beacon-verified
               post-episode recovery. */
            src_ts_snaps++;               /* counter kept as a stress metric */
            src_splice_last_ms = now_ms;
            src_auto_kick_pending = true;
        }
    }
    src_eld_last_send_ms = now_ms;
    // Debug: print first few packets
    // if (aaceld_debug_count < 5) {
    //     printf("AAC-ELD TX: %d bytes, %d frames, ts=%u, first 8: ",
    //            media_tracker.codec_storage_count, media_tracker.codec_num_frames,
    //            (unsigned)media_tracker.rtp_timestamp);
    //     for (int k = 0; k < 8 && k < media_tracker.codec_storage_count; k++) {
    //         printf("%02x ", media_tracker.codec_storage[k]);
    //     }
    //     printf("\n");
    //     aaceld_debug_count++;
    // }

    /* On-air payload validation (see src_tx_malformed). Runs in BT context,
       bounded: <=3 headers to walk, prints only on violation or during the
       8-packet post-splice window. */
    if (media_tracker.codec_storage_count > 0) {
        uint16_t off = 0, nau = 0, sprev = 0;
        bool bad = false, first = true;
        while (off + 4 <= media_tracker.codec_storage_count) {
            const uint8_t *h = &media_tracker.codec_storage[off];
            uint16_t hseq = (uint16_t)(((h[0] & 0x0F) << 8) | h[1]);
            uint16_t hsz  = (uint16_t)(((h[2] & 0x0F) << 8) | h[3]);
            if ((h[0] & 0xF0) != 0xB0 || (h[2] & 0xF0) != 0x10) { bad = true; break; }
            if (!first && hseq != ((sprev + 1) & 0x0FFF))       { bad = true; break; }
            first = false; sprev = hseq;
            off = (uint16_t)(off + 4 + hsz); nau++;
        }
        if (off != media_tracker.codec_storage_count ||
            nau != media_tracker.codec_num_frames) bad = true;
        if (bad && src_tx_malformed == 0) {   /* snapshot the FIRST offender */
            uint16_t sn = media_tracker.codec_storage_count;
            if (sn > sizeof(src_mal_snap)) sn = sizeof(src_mal_snap);
            memcpy(src_mal_snap, media_tracker.codec_storage, sn);
            src_mal_len     = media_tracker.codec_storage_count;
            src_mal_nfr     = media_tracker.codec_num_frames;
            src_mal_walkoff = off;
            src_mal_walkn   = nau;
            src_mal_seqctr  = aaceld_frame_sequence;
        }
        if (bad || src_splice_dump) {
            if (!bad && src_splice_dump) src_splice_dump--;
            if (bad) src_tx_malformed++;
            /* The post-splice sample dump is telemetry — it rides the 'E'
               tap. A MALFORMED header chain is a real defect and always
               prints (it also bumps tx_malformed in the 'R' report). */
            if (!bad && !src_eld_tap) goto txchk_done;
            uint16_t dn = media_tracker.codec_storage_count;
            if (dn > 24) dn = 24;
            char hx[24 * 2 + 1];
            static const char hd[] = "0123456789abcdef";
            for (uint16_t b = 0; b < dn; b++) {
                hx[b * 2]     = hd[media_tracker.codec_storage[b] >> 4];
                hx[b * 2 + 1] = hd[media_tracker.codec_storage[b] & 0x0F];
            }
            hx[dn * 2] = '\0';
            printf("[txchk] %s len %u n %u walk_off %u walk_n %u hex %s\n",
                   bad ? "MALFORMED" : "splice", media_tracker.codec_storage_count,
                   media_tracker.codec_num_frames, off, nau, hx);
        }
        txchk_done: ;
    }

    src_tx_pkts++;
    {
        uint8_t send_rc = a2dp_source_stream_send_media_payload_rtp(media_tracker.avdtp_cid, media_tracker.local_seid, rtp_marker, media_tracker.rtp_timestamp, &media_tracker.codec_storage[0], media_tracker.codec_storage_count);
        if (send_rc != ERROR_CODE_SUCCESS) {
            src_send_fails++;
            src_send_fail_rc = send_rc;
            printf("[txchk] SEND FAILED rc %u pkt #%lu (on-air seq gap!)\n",
                   send_rc, (unsigned long)src_tx_pkts);
        }
    }

    if (src_eld_tap) {
        /* codec_ready_to_send gates the pump, so no fill ran since this
           batch was staged — the first AU's seq is simply counter-minus-n. */
        uint16_t nfr  = (media_tracker.codec_num_frames > 0) ? media_tracker.codec_num_frames : 1;
        uint16_t seq0 = (uint16_t)((aaceld_frame_sequence - nfr) & 0x0FFF);
        printf("[eld] #%lu t %lu seq %u n %u len %u ts %lu gap %lu%s\n",
               (unsigned long)src_tx_pkts, (unsigned long)now_ms, seq0, nfr,
               media_tracker.codec_storage_count,
               (unsigned long)media_tracker.rtp_timestamp,
               (unsigned long)(prev_send_ms ? now_ms - prev_send_ms : 0),
               rtp_marker ? " SNAP" : "");
    }

    /* RTP timestamp: MILLISECOND clock stamped from the REAL clock, exactly
       as macOS does (wire, 3732 pkts: 1 kHz units, per-packet deltas
       jittering 29-33 with mean 30.00 — i.e. sampled from a millisecond
       clock, NOT accumulated from AU counts).
       Accumulating (n_AUs x 10) was tried and FAILED HARD: our packets are
       often 2 AUs, so the clock advanced +20 while 22 ms of wall time
       passed — a playout clock running slower than real time starves the
       renderer and the buds go SILENT within seconds (HW). Stamping from
       the run-loop clock is immune to AU batching, resampling and pump
       jitter by construction. Base offset keeps the first stamp near 0. */
    if (src_rtp_ms_base == 0) src_rtp_ms_base = now_ms;
    media_tracker.rtp_timestamp = now_ms - src_rtp_ms_base;

    media_tracker.codec_storage_count = 0;
    media_tracker.codec_ready_to_send = 0;
    media_tracker.codec_num_frames = 0;
}



uint32_t get_vendor_id(const uint8_t *codec_info) {
    uint32_t vendor_id = 0;
    vendor_id |= codec_info[0];
    vendor_id |= codec_info[1] << 8;
    vendor_id |= codec_info[2] << 16;
    vendor_id |= codec_info[3] << 24;
    return vendor_id;
}

uint16_t get_codec_id(const uint8_t *codec_info) {
    uint16_t codec_id = 0;
    codec_id |= codec_info[4];
    codec_id |= codec_info[5] << 8;
    return codec_id;
}

static void a2dp_demo_send_media_packet(void) {
    /* Dispatch on cur_codec — the latched truth — NOT the remote endpoint's
       codec_type: BTstack zeroes remote_configuration during reconnect
       churn, zeroed reads as SBC, and a can-send grant landing mid-teardown
       then entered the SBC send with an uninitialized encoder = precise
       bus fault + reboot (HW: hard fault at btstack_sbc_encoder_sbc_buffer_
       length during the weak-signal walk). Same trap, same fix as the
       zombie-pump dispatch above. */
    switch (cur_codec){
        case 1: codec_sbc_ops.send();             break;
#ifdef HAVE_AAC_FDK
        case 2: codec_aac_ops.send();             break;
        case 4: codec_aaceld_ops.send();          break;
#endif
        case 3: codec_ldac_ops.send();            break;
        case 5: codec_lhdc_ops.send();            break;
        default:
            /* cur_codec 0 = no completed setup: nothing sane to send */
            media_tracker.codec_storage_count = 0;
            media_tracker.codec_ready_to_send = 0;
            media_tracker.codec_num_frames = 0;
            break;
    }
}



// --- Slot queue audio buffer ---
typedef struct {
    int16_t data[AUDIO_SLOT_MAX_INT16];
} audio_slot_t;

static audio_slot_t slot_pool[AUDIO_SLOT_COUNT_MAX];

/* codec files stage PCM out of the slot pool through this accessor */
int16_t *audio_slot_data(uint8_t slot_idx){ return slot_pool[slot_idx].data; }
static queue_t free_queue;
static queue_t ready_queue;
static uint8_t active_slot_count = AUDIO_SLOT_COUNT_SBC; // current queue depth

// USB-side filling state (written only from Core 0)
static volatile uint32_t usb_slot_push_drops;   /* USB pushes lost to a full pool (while streaming) */
static volatile uint32_t usb_slot_ready_full_drops; /* filled slot bounced: ready queue full */
static volatile uint32_t usb_slot_scrubs;       /* stale slots dropped to restore pool headroom */
static uint8_t  filling_slot_idx;
static uint16_t filling_offset    = 0;
static bool     filling_slot_valid = false;
static uint16_t slot_frame_int16  = AUDIO_SLOT_MAX_INT16; // set per codec

static bool is_usb_streaming = false;
bd_addr_t cur_active_device;

// --- AAC-ELD Core 1 encoder infrastructure ---
#define ENCODED_FRAME_MAX_SIZE 500
#define ENCODED_FRAME_QUEUE_DEPTH 8

typedef struct {
    uint8_t  data[ENCODED_FRAME_MAX_SIZE];  // Apple header + raw AU
    uint16_t length;
} encoded_frame_t;

static encoded_frame_t encoded_frame_pool[ENCODED_FRAME_QUEUE_DEPTH];
static queue_t encoded_free_queue;   // Core 1 gets empty frames from here
static queue_t encoded_ready_queue;  // Core 1 pushes encoded frames here, Core 0 pops

static volatile bool core1_encoder_active = false;
static volatile uint32_t core1_encode_count = 0;  // debug: frames encoded by Core 1
static volatile uint32_t core1_state = 0;          // debug: 0=idle, 1=active, 2=got_raw, 3=got_enc, 4=encoding, 5=encoded
/* aaceld_frame_sequence: moved up with the ELD send statics (the tap needs it) */
bool finish_setup_aac = false;
bool allow_switch_slot = true;
static bool src_slot_redial_pending;   /* console slot switch: dial the new
                                          slot once the release completes */
static bool src_user_dc_hold;          /* user pressed disconnect: suppress every
                                          auto-redial (stale-drop, connmode 3)
                                          until playback restarts, a dial is
                                          asked for, or a link comes up */
bool a2dp_user_dc_hold(void){ return src_user_dc_hold; }
void a2dp_user_dc_clear(void){ src_user_dc_hold = false; }

// --- Suspend recovery: restart stream if suspend response never arrives ---
static btstack_timer_source_t suspend_recovery_timer;
static bool suspend_recovery_pending = false;
#define SUSPEND_RECOVERY_TIMEOUT_MS 3000

static void a2dp_demo_timer_start(a2dp_media_sending_context_t * context);

static void suspend_recovery_handler(btstack_timer_source_t *timer) {
    (void)timer;
    if (!suspend_recovery_pending) return;
    suspend_recovery_pending = false;

    printf("Suspend recovery: no response after %d ms, restarting stream\n", SUSPEND_RECOVERY_TIMEOUT_MS);
    // Try to start the stream directly (skip suspend since it timed out)
    avdtp_source_start_stream(media_tracker.avdtp_cid, media_tracker.local_seid);
}

void audio_slot_queue_init(void) {
    active_slot_count = AUDIO_SLOT_COUNT_SBC;  // default
    queue_init(&free_queue,  sizeof(uint8_t), AUDIO_SLOT_COUNT_MAX);
    queue_init(&ready_queue, sizeof(uint8_t), AUDIO_SLOT_COUNT_MAX);
    for (uint8_t i = 0; i < active_slot_count; i++) {
        queue_try_add(&free_queue, &i);
    }
    filling_slot_valid = false;
    filling_offset = 0;
}

/* Drop all STAGED host PCM + the pacing credit, keep the configuration.
   Called at UAC2 EP close: Windows closes the ISO EP per SOUND, and any
   staged-but-unplayed samples would otherwise play BEFORE the next sound's
   audio — each close/open cycle then adds a sliver of latency ("AirPods
   drift out of sync on Windows"; UAC1 didn't accumulate it). */
static volatile uint32_t usb_ep_flushes;        /* EP-close flush count      */
static volatile uint32_t usb_ep_flushed_slots;  /* staged slots dropped total */

static void audio_slot_flush_exec(bool live) {
    uint8_t idx;
    while (queue_try_remove(&ready_queue, &idx)) {
        queue_try_add(&free_queue, &idx);
        usb_ep_flushed_slots++;
    }
    if (filling_slot_valid) {
        /* LEAK FIX (HW: "free 0 of 6" after 26 flushes — pool starved dead):
           the half-filled slot must go BACK to the pool, not just be
           forgotten — one orphaned index per flush emptied all 6 slots. */
        queue_try_add(&free_queue, &filling_slot_idx);
        usb_ep_flushed_slots++;
    }
    filling_slot_valid = false;
    filling_offset = 0;
    media_tracker.samples_ready = 0;   /* pacing credit dies with the PCM */
    usb_ep_flushes++;
    /* Mid-stream close = live splice: kill the stamp reference so the next
       send cannot stamp a phantom hole (see src_live_splices comment).
       USB context: single-word stores only toward BT-side readers (safe);
       the credit zero below is an atomic store against the pump's RMWs.
       Idle closes end in timer_start which re-zeroes it anyway. */
    if (is_streaming) {
        src_eld_last_send_ms = 0;
        if (live) {
            /* Preventive per-splice nudge REVERTED (HW: buggy at 6 switches
               vs 15 without — forcing a pipeline re-lock every 2 s DURING
               splices compounds damage; resets only help on a quiet
               stream, which is exactly what the quiesce-gated recovery
               already does). */
            src_fade_out_pending = true;   /* splice-edge fades (see src_tail) */
            src_fade_in_pending  = true;
            src_live_splices++;
            src_splice_dump = 8;   /* hex the next packets for the validator */
            uint32_t now = btstack_run_loop_get_time_ms();
            src_splice_last_ms = now;   /* quiesce gate for the recovery */
            if (now - src_splice_win_ms > 10000) {
                src_splice_win_ms = now;
                src_splice_win_n = 1;
            } else if (++src_splice_win_n >= 2) {
                /* LATCH, never discard: the pump tick defers firing through
                   the cooldown, so churn that ends mid-cooldown still gets
                   its final kick (end-of-churn wedges were left dead when
                   this check discarded during cooldown). */
                src_auto_kick_pending = true;
            }
        }
    }
}

static volatile bool slot_pool_frozen;   /* reconfigure in flight: USB pushes
                                            drop instead of racing the drain/
                                            repopulate (bounded, counted) */

void audio_slot_queue_configure_with_count(uint16_t samples_per_slot, uint8_t slot_count) {
    slot_pool_frozen = true;
    // Drain both queues
    uint8_t idx;
    while (queue_try_remove(&ready_queue, &idx)) {}
    while (queue_try_remove(&free_queue, &idx)) {}
    // Return filling slot if any
    if (filling_slot_valid) {
        filling_slot_valid = false;
    }
    filling_offset = 0;
    slot_frame_int16 = samples_per_slot * 2; // stereo
    active_slot_count = (slot_count <= AUDIO_SLOT_COUNT_MAX) ? slot_count : AUDIO_SLOT_COUNT_MAX;
    // Repopulate free queue with new count
    for (uint8_t i = 0; i < active_slot_count; i++) {
        queue_try_add(&free_queue, &i);
    }
    slot_pool_frozen = false;
    PLOG("Slot queue: %d slots x %d samples\n", active_slot_count, samples_per_slot);
}

void audio_slot_queue_configure(uint16_t samples_per_slot) {
    audio_slot_queue_configure_with_count(samples_per_slot, active_slot_count);
}

/* Software USB volume (swvol mode): the host's FU volume becomes a gain on
   USB PCM only — the headset's absolute volume and the relay's phone audio
   stay untouched (ESP32 design). CUBIC taper: amplitude = (pct/100)^3 —
   mirrors macOS's own slider-to-loudness curve so the web bar tracks the
   Mac slider position. */
static volatile uint16_t usb_sw_gain_q15 = 32768u;   /* unity */

void set_usb_sw_gain_pct(uint8_t pct) {
    if (pct > 100) pct = 100;
    uint32_t q = ((uint32_t)pct * pct * 32768u) / 10000u;   /* (pct/100)^2 */
    usb_sw_gain_q15 = (uint16_t)((q * pct) / 100u);         /* ^3          */
}

/* For the 'b' raw-PCM dump header: the dump is taken in the USB rx callback,
   i.e. BEFORE this gain is applied here — so a dump can look perfectly loud
   while the encoder is fed near-silence. Printing the gain makes each dump
   self-describing instead of needing a separate SWD read to interpret. */
uint16_t get_usb_sw_gain_q15(void) { return usb_sw_gain_q15; }

void audio_slot_push_samples(const int16_t *src, uint16_t stereo_pair_count) {
    if (slot_pool_frozen) {            /* reconfigure mid-flight: drop, count */
        usb_slot_push_drops++;
        return;
    }
    uint16_t remaining = stereo_pair_count * 2; // total int16_t values
    uint16_t src_offset = 0;

    while (remaining > 0) {
        // Acquire a filling slot if we don't have one
        if (!filling_slot_valid) {
            if (!queue_try_remove(&free_queue, &filling_slot_idx)) {
                // Only meaningful while the pump drains — the connect phase
                // (pump not started yet) always overflows the pool and would
                // poison the counter with a fat, benign burst.
                if (is_streaming) usb_slot_push_drops++;
                return; // no free slots, drop samples
            }
            filling_slot_valid = true;
            filling_offset = 0;
        }

        // Copy as much as fits into the current slot
        uint16_t space = slot_frame_int16 - filling_offset;
        uint16_t to_copy = (remaining < space) ? remaining : space;
        uint16_t g = usb_sw_gain_q15;
        if (g == 32768u) {
            memcpy(&slot_pool[filling_slot_idx].data[filling_offset],
                   &src[src_offset], to_copy * sizeof(int16_t));
        } else {
            int16_t *dst = &slot_pool[filling_slot_idx].data[filling_offset];
            for (uint16_t i = 0; i < to_copy; i++) {
                dst[i] = (int16_t)(((int32_t)src[src_offset + i] * g) >> 15);
            }
        }
        filling_offset += to_copy;
        src_offset += to_copy;
        remaining -= to_copy;

        // If slot is full, push to ready queue
        if (filling_offset >= slot_frame_int16) {
            if (!queue_try_add(&ready_queue, &filling_slot_idx)) {
                // Ready queue full, return slot to free (drop data)
                queue_try_add(&free_queue, &filling_slot_idx);
                if (is_streaming) usb_slot_ready_full_drops++;
            } else {
            }
            filling_slot_valid = false;
            filling_offset = 0;
        }
    }
}

// BT-side helpers: pop a ready slot or get a silence slot.
// The sink relay mixes phone PCM onto every popped slot (real USB data or
// silence) — this is the single choke point all codec fillers pass through,
// the Pico's equivalent of the ESP32's pcm_fill() mix.
static volatile uint32_t slot_real_waits;   /* pops deferred awaiting live USB data */

/* START PRIME (ELD, non-gaming): a stream (re)start or the host's first
   packets land on an EMPTY queue — the pump then runs hand-to-mouth with
   the pacing credit pegged at its cap (HW: credit ~1800/2400, real_waits
   +130/s = "sound buggy after connect, settles later"). Until 2 ready
   slots (~20 ms) are staged, encode SILENCE — gapless AUs, credit stays
   balanced, real audio starts ~20 ms late. GAMING MODE SKIPS THIS:
   latency beats the cleaner start there (user call). */
static volatile bool src_prime_pending;

bool audio_slot_pop(uint8_t *slot_idx) {
    if (src_prime_pending && is_usb_streaming && cur_codec == 4) {
        extern bool aap_get_game(void);
        if (aap_get_game() || queue_get_level(&ready_queue) >= 2) {
            src_prime_pending = false;          /* primed (or gaming: skip) */
        } else if (queue_try_remove(&free_queue, slot_idx)) {
            memset(slot_pool[*slot_idx].data, 0, slot_frame_int16 * sizeof(int16_t));
            bt_sink_relay_mix(slot_pool[*slot_idx].data, slot_frame_int16 / 2);
            return true;
        }
        /* no free slot: fall through — the ready pop below covers it */
    }
    if (queue_try_remove(&ready_queue, slot_idx)) {
        bt_sink_relay_mix(slot_pool[*slot_idx].data, slot_frame_int16 / 2);
        int16_t *d = slot_pool[*slot_idx].data;
        uint16_t frames = slot_frame_int16 / 2;
        uint16_t n = frames < SRC_FADE_FRAMES ? frames : SRC_FADE_FRAMES;
        if (src_fade_in_pending) {
            src_fade_in_pending = false;
            for (uint16_t f = 0; f < n; f++) {
                d[2*f]   = (int16_t)((int32_t)d[2*f]   * f / n);
                d[2*f+1] = (int16_t)((int32_t)d[2*f+1] * f / n);
            }
        }
        memcpy(src_tail, &d[(frames - n) * 2], (size_t)n * 2 * sizeof(int16_t));
        return true;
    }
    if (is_usb_streaming) {
        /* The host is live and the next real slot is ~a millisecond away.
           Encoding a silence slot HERE splices 10-21 ms of silence INTO the
           music whenever the pump tick wins the race against USB delivery.
           At 48 k that race was the "rare drop" (once per minutes, clock
           drift); at 44.1 k (441 frames/tick against the host's 44/45
           packet cadence) it fires constantly — the "buggy at 44.1" SBC
           audio. Wait for real data instead: samples_ready carries the
           pacing credit to the next tick. */
        slot_real_waits++;
        return false;
    }
    // Host idle — produce silence to keep the stream alive
    if (queue_try_remove(&free_queue, slot_idx)) {
        int16_t *d = slot_pool[*slot_idx].data;
        memset(d, 0, slot_frame_int16 * sizeof(int16_t));
        if (src_fade_out_pending) {
            src_fade_out_pending = false;
            uint16_t frames = slot_frame_int16 / 2;
            uint16_t n = frames < SRC_FADE_FRAMES ? frames : SRC_FADE_FRAMES;
            for (uint16_t f = 0; f < n; f++) {
                d[2*f]   = (int16_t)((int32_t)src_tail[2*f]   * (n - f) / n);
                d[2*f+1] = (int16_t)((int32_t)src_tail[2*f+1] * (n - f) / n);
            }
        }
        bt_sink_relay_mix(d, slot_frame_int16 / 2);
        return true;
    }
    return false;
}

// Check if real audio data is available (not silence)
static bool audio_slot_has_data(void) {
    return queue_get_level(&ready_queue) > 0;
}

void audio_slot_release(uint8_t slot_idx) {
    queue_try_add(&free_queue, &slot_idx);
}

// --- Core 1 AAC-ELD encoder ---

static void encoded_frame_queue_init(void) {
    queue_init(&encoded_free_queue,  sizeof(uint8_t), ENCODED_FRAME_QUEUE_DEPTH);
    queue_init(&encoded_ready_queue, sizeof(uint8_t), ENCODED_FRAME_QUEUE_DEPTH);
    for (uint8_t i = 0; i < ENCODED_FRAME_QUEUE_DEPTH; i++) {
        queue_try_add(&encoded_free_queue, &i);
    }
}

void core1_aaceld_encoder_loop(void) {
    // FDK-AAC encoder buffers
    AACENC_BufDesc in_buf = {0}, out_buf = {0};
    AACENC_InArgs  in_args = {0};
    AACENC_OutArgs out_args = {0};
    int in_identifier = IN_AUDIO_DATA;
    int in_size, in_elem_size = 2;
    int out_identifier = OUT_BITSTREAM_DATA;
    int out_size, out_elem_size = 1;
    void *in_ptr, *out_ptr;

    in_buf.numBufs           = 1;
    in_buf.bufs              = &in_ptr;
    in_buf.bufferIdentifiers = &in_identifier;
    in_buf.bufSizes          = &in_size;
    in_buf.bufElSizes        = &in_elem_size;

    out_buf.numBufs           = 1;
    out_buf.bufs              = &out_ptr;
    out_buf.bufferIdentifiers = &out_identifier;
    out_buf.bufSizes          = &out_size;
    out_buf.bufElSizes        = &out_elem_size;

    while (true) {
        if (!core1_encoder_active) {
            core1_state = 0;
            sleep_ms(1);
            continue;
        }
        core1_state = 1;

        // Get a raw audio slot (real data or silence)
        uint8_t raw_slot_idx;
        if (!audio_slot_pop(&raw_slot_idx)) {
            sleep_us(500);
            continue;
        }
        core1_state = 2;

        // Get an encoded frame slot
        uint8_t enc_idx;
        if (!queue_try_remove(&encoded_free_queue, &enc_idx)) {
            // No free encoded slots — drop raw data
            audio_slot_release(raw_slot_idx);
            sleep_us(500);
            continue;
        }
        core1_state = 3;

        // Encode with FDK-AAC
        unsigned int num_samples = acc_num_simples * aacinf.inputChannels;
        in_ptr               = slot_pool[raw_slot_idx].data;
        in_size              = num_samples * sizeof(int16_t);
        in_args.numInSamples = num_samples;

        // Leave 4 bytes at start for Apple header
        out_ptr  = &encoded_frame_pool[enc_idx].data[4];
        out_size = ENCODED_FRAME_MAX_SIZE - 4;

        core1_state = 4;
        AACENC_ERROR err = aacEncEncode(handleAAC, &in_buf, &out_buf, &in_args, &out_args);
        core1_state = 5;

        // Release raw slot immediately
        audio_slot_release(raw_slot_idx);

        if (err != AACENC_OK || out_args.numOutBytes == 0) {
            queue_try_add(&encoded_free_queue, &enc_idx);
            continue;
        }

        core1_encode_count++;

        // Prepend Apple AAC-ELD Size Header
        uint16_t seq = aaceld_frame_sequence;
        aaceld_frame_sequence = (aaceld_frame_sequence + 1) & 0x0FFF;
        uint16_t au_size = out_args.numOutBytes;
        encoded_frame_pool[enc_idx].data[0] = 0xB0 | ((seq >> 8) & 0x0F);
        encoded_frame_pool[enc_idx].data[1] = seq & 0xFF;
        encoded_frame_pool[enc_idx].data[2] = 0x10 | ((au_size >> 8) & 0x0F);
        encoded_frame_pool[enc_idx].data[3] = au_size & 0xFF;
        encoded_frame_pool[enc_idx].length = 4 + out_args.numOutBytes;

        // Push to ready queue for Core 0
        if (!queue_try_add(&encoded_ready_queue, &enc_idx)) {
            queue_try_add(&encoded_free_queue, &enc_idx);
        }
    }
}

bool check_is_streaming(){
    return is_streaming;
}

/* AVRCP PLAY STATUS — ALWAYS "playing", deliberately.
   We are a live audio PIPE, not a player: the dongle cannot know whether the
   computer is really playing music (the USB stream stays open, carrying
   silence, while a paused player sits idle), so any attempt to mirror a real
   play state is guesswork. What we DO know is the failure mode: a headset
   that believes it is paused mutes its own speakers, and then a host that is
   actually playing produces no sound.
   play_info was also left zero-initialised forever and STOPPED == 0, so a
   headset assumed the dongle had never played and its first press sent PLAY.
   So: report PLAYING always, and re-assert it after every transport press.
   The headset therefore never mutes itself, and its button becomes a plain
   "toggle the computer" key — the computer owns actual playback. */
/* PLAYBACK-STATUS NOTIFICATIONS: intentionally NOT sent. The saga (HW,
   2026-08-03, two headsets with opposite personalities):
   - always report PLAYING  -> a Bose reconciles state and storms transport
     commands to force the point; each one toggled the host = flapping.
   - report the honest state + only toggle the host when intent differs ->
     the Bose's own internal toggle never syncs with ours, so every OTHER
     press "already matched" and was dropped: the user had to press twice
     per action. Status changes may also re-trigger its reconciliation.
   Verdict: the transport button must be a DUMB TOGGLE (like the dongle's
   own button) and the status channel must stay quiet. play_info.status is
   seeded PLAYING at connect purely so a GET_PLAY_STATUS query never reads
   the zero-initialised STOPPED (some headsets mute on "never played"). */

void set_usb_streaming(bool flag){
    if (!is_usb_streaming && flag && cur_codec == 4)
        src_prime_pending = true;   /* start prime — see audio_slot_pop */
    is_usb_streaming = flag;
}

/* The address of the peer ACTUALLY connected. NOT get_device_addr(), which is
   the DIAL TARGET taken from the current slot — any paired headset can page US
   instead, and then the two differ (HW: auto-connect was dialling slot 2's ROSE
   while the Bose connected inbound, so every UI label read "ROSE OpenFeel").
   All-zero when nothing is connected. */
const uint8_t *a2dp_source_peer_addr(void){
    return cur_active_device;
}

/* Media sends can FAIL: BTstack returns MEMORY_CAPACITY_EXCEEDED when the
   payload plus the RTP header exceeds the peer's L2CAP MTU, and the packet is
   simply never sent. Only the ELD path checked this, so an oversized codec
   payload was silent, total audio loss that looked like RF trouble. Every
   codec's send now reports here. */
void a2dp_source_note_send(uint8_t rc){
    if (rc == ERROR_CODE_SUCCESS) return;
    src_send_fails++;
    src_send_fail_rc = rc;
    printf("[txchk] SEND FAILED rc %u pkt #%lu (payload too big for the link MTU?)\n",
           rc, (unsigned long) src_tx_pkts);
}

bool get_allow_switch_slot(){
    return allow_switch_slot;
}





#ifdef HAVE_AAC_FDK

static volatile bool src_eld_reset_pending;  /* TX-stall ELD reset: run in main loop */
/* Codec teardown also runs in the main loop, and for a harder reason than the
   ELD reset: every release() frees, and pico's free() takes malloc_mutex via
   mutex_enter_blocking (pico_multicore is linked, so PICO_USE_MALLOC_MUTEX
   defaults on). Called from the BT background IRQ it can preempt a thread-mode
   malloc that already holds that mutex — and thread mode can never run to
   release it, because any IRQ outranks it. That deadlock presents as the loop
   starving with IRQs still alive. */
static volatile bool src_codec_release_pending;
static uint32_t src_avrcp_op_ms;             /* last accepted transport press
                                                (storm guard, see AVRCP target) */
static volatile uint32_t src_storm_ms;       /* last signal-bad recovery    */
static volatile bool     src_storm_pending;  /* clean-air hygiene armed     */
static uint8_t  src_storm_passes;    /* hygiene-ladder rungs done this episode */
static uint32_t src_ladder_last_ms;  /* last ladder activity (pass or storm)   */
/* src_started_ms lives with the ELD send statics (declared early: the send
   path's lock-window guard reads it) */
static btstack_timer_source_t src_dial_retry_timer;  /* post-reboot stale-session dials */
static uint8_t  src_dial_tries;
static uint8_t  src_dial_total;       /* attempts in the current recovery window */
#define SRC_DIAL_MAX 8                /* dials per window before giving up */
static uint32_t src_dial_window_ms;   /* window start (arm); 0 = no window       */
static void src_dial_retry_arm(void);
static void src_dial_maybe_redial_after_drop(void);

/* AAC-ELD encoder rate — web setting (AirPods card > More settings, saved
   in settings()->eld_rate; menu 192/256/320, default 256 — 480 k was
   refused by AirPods). Packets-per-second follows AU size vs the 986 B
   Apple MTU: the fill loop and the send trigger both use
   src_eld_au_space(), so the AUs-per-packet count adapts to ANY rate.
   (A hardcoded 400 B threshold structurally broke rates ~390-460 k: the
   fill/send-trigger gap caused an encode error storm.) */
static uint32_t src_eld_bitrate(void) {
    extern bool aap_get_game(void);
    uint32_t r;
    if (aap_get_game()) r = 192000;      /* gaming: smallest AUs — latency over fidelity
                                            (overrides the menu, setting untouched) */
    else switch (settings()->eld_rate) {
        case 1:  r = 192000; break;
        case 3:  r = 320000; break;
        default: r = 256000; break;      /* 0 (unset/zero-fill) and 2 */
    }
    /* 44.1 pipeline: hold ELD at gaming's exact stream (192k CBR, sbr/vbr
       already forced off by the vbr_now guard). 256k CBR here transmitted
       fine but rendered SILENT on the buds (HW 2026-08-09: 44 pkt/s, AUs
       staged, AAP live, no audio), and 256k VBR+SBR wedges the FDK rate
       loop. 192k CBR is the one 44.1 config with weeks of field proof —
       gaming has shipped it since day one. Full 256k VBR+SBR engages on
       the 48 k host path, where it is Apple parity and proven. */
    if (!aap_get_game() && (settings()->usbset & USBSET_RATE44) && r > 192000)
        r = 192000;
    /* dual-stream coex: while a relay phone streams (AAC-LC in + ELD out
       share the radio), cap ELD at 164 kbps — applied via the suspend->
       rebuild->restart dance in the ladder, never a live SetParam. */
    if (bt_sink_relay_streaming() && r > 164000) r = 164000;
    return r;
}

/* Live on-air bitrate of the CURRENT codec, kbps (0 = unknown/not streaming).
   Asked from the encoder rather than the setting, so it reflects what is
   actually on the air: the ELD relay/gaming caps, the AAC-LC coex ladder,
   and a live LDAC retune all show up here. SBC would need a bitpool ->
   bitrate computation; it reports 0 for now (the UI just omits it). */
uint32_t a2dp_source_live_kbps(void) {
    switch (cur_codec) {
        case 3: return (uint32_t)codec_ldac_live_kbps();
        case 5: return (uint32_t)codec_lhdc_live_kbps();
#ifdef HAVE_AAC_FDK
        case 2: return (uint32_t)codec_aac_live_kbps();   /* VBR-safe, clamped */
        case 4: {
            /* ELD: GetParam is trustworthy under CBR; clamp anyway and fall
               back to the effective setting (incl. relay-low cap / gaming). */
            if (handleAAC == NULL) return 0;
            uint32_t kb = (uint32_t)aacEncoder_GetParam(handleAAC, AACENC_BITRATE) / 1000u;
            if (kb >= 32 && kb <= 1000) return kb;
            return src_eld_bitrate() / 1000u;
        }
#endif
        default: return 0;
    }
}
/* macOS parity: BTAudioHALPlugin runs AAC-ELD in VBR (BitRateControlMode=3,
   ~227 kbps measured / 256 kbps ceiling), dropping to CBR only under link
   adaptation. FDK VBR_5 = 96 k/ch (~192 k stereo), nearest preset to Apple's
   average; the 256 k peak cap makes maxBitsPerFrame 320 B — exactly the max
   AU size seen on Apple's wire. 0 = legacy CBR at src_eld_bitrate(). */
static uint8_t src_eld_vbr_mode = 5;
/* Step 2 of encoder parity: LD-SBR, DOWNSAMPLED (Apple runs ratio=1 —
   AACEnhancedLowDelaySBREncoder, granule stays 480/10 ms; the old "SBR
   unstable" attempt predates transport parity and likely ran FDK's dual-
   rate default, the opposite shape). 0 = SBR off. */
static uint8_t src_eld_sbr = 1;
#define SRC_ELD_PEAK_BITRATE 256000u
/* Apple's own link-adapt pattern: CBR whenever a SPECIFIC rate must bind.
   Under VBR, FDK ignores AACENC_BITRATE entirely — which silently
   neutralised the relay 164k coex cap, the gaming 192k override and the
   192/320 menu choices (review-4ae498d-findings.md). VBR_5 only for the
   default 256k steady-state case, exactly like macOS. */
static uint8_t src_eld_vbr_now(void) {
    extern bool aap_get_game(void);
    if (src_eld_vbr_mode == 0) return 0;
    /* 44.1 pipeline: plain CBR, no VBR and (via sbr_now's coupling) no SBR.
       VBR5+SBR over the 441->480 resampler wedges FDK's rate loop — caught
       at FDKaacEnc_dynBitCount over SWD on a build with STOCK FDK, and the
       non-game bootloop reproduced within minutes every time this guard was
       absent. The buds decode SBR-less ELD fine — gaming (192k CBR sbr=0)
       has shipped that way for weeks. (This guard was here once before and
       got reverted on a mistaken noise attribution: the noise came from
       since-removed FDK iteration caps that shipped in the same build, not
       from sbr=0. Full story: private/docs + the 2026-08-09 commits.) */
    if (settings()->usbset & USBSET_RATE44) return 0;
    if (bt_sink_relay_streaming()) return 0;   /* coex cap must bind */
    if (aap_get_game()) return 0;              /* deterministic airtime */
    if (settings()->eld_rate == 1 || settings()->eld_rate == 3) return 0;
    return src_eld_vbr_mode;
}
/* SBR only rides the VBR path. FDK's own ELD/SBR auto-config table
   (eldSbrAutoConfigTab) drops SBR for 2ch/48k at >=128 kbps, so forcing it
   on at a CBR rate leaves the quantizer a budget it cannot meet: the rate
   loop in FDKaacEnc_QCMain (qc_main.cpp, while(!quantizationDone) /
   do..while(!constraintsFulfilled)) then never converges and never returns.
   HW 2026-08-06: hung there in game mode (CBR 192k + SBR) with real music,
   caught over SWD — BT-IRQ stuck inside aacEncEncode, run loop starved,
   watchdog reset, bootloop. VBR has the slack to absorb SBR's overhead,
   which is why the 256k Apple-parity path has been stable for days. */
static uint8_t src_eld_sbr_now(void){ return src_eld_vbr_now() ? src_eld_sbr : 0; }
static int src_eld_au_space(void) {
    /* budget the LARGEST possible AU: under VBR that is the peak cap, not
       the average — a peak AU past the packet budget would oversize L2CAP */
    uint32_t r = src_eld_bitrate();
    if (src_eld_vbr_now() && r < SRC_ELD_PEAK_BITRATE) r = SRC_ELD_PEAK_BITRATE;
    return (int)(r / 800) + 4 + 64;  /* AU + hdr + wiggle */
}
static uint8_t src_eld_slot_count(void) {
    extern bool aap_get_game(void);
    if (aap_get_game())   /* gaming wins over 320k; queue depth per level */
        return (settings()->game_level == 2) ? AUDIO_SLOT_COUNT_ELD_GAME
                                             : AUDIO_SLOT_COUNT_ELD;
    if (settings()->eld_rate == 3) return AUDIO_SLOT_COUNT_ELD_320;
    return AUDIO_SLOT_COUNT_ELD;
}

int codec_fdk_fill(a2dp_media_sending_context_t *context) {
    int          total_samples_read               = 0;

    if (src_eld_reset_pending) return 0;   /* encoder rebuild in flight (main loop) */
    if (handleAAC == NULL) return 0;       /* released; same guard as lhdc_fill */

    unsigned int num_audio_samples_per_aac_buffer = acc_num_simples;

    btstack_assert(num_audio_samples_per_aac_buffer <= AUDIO_SLOT_MAX_SAMPLES);

    unsigned required_samples = num_audio_samples_per_aac_buffer * aacinf.inputChannels;

    AACENC_BufDesc in_buf   = { 0 }, out_buf = { 0 };
    AACENC_InArgs  in_args  = { 0 };
    AACENC_OutArgs out_args = { 0 };
    int in_identifier = IN_AUDIO_DATA;
    int in_size, in_elem_size;
    int out_identifier = OUT_BITSTREAM_DATA;
    int out_size, out_elem_size;
    void *in_ptr, *out_ptr;

    in_elem_size             = 2;
    in_buf.numBufs           = 1;
    in_buf.bufs              = &in_ptr;
    in_buf.bufferIdentifiers = &in_identifier;
    in_buf.bufSizes          = &in_size;
    in_buf.bufElSizes        = &in_elem_size;

    out_elem_size             = 1;
    out_buf.numBufs           = 1;
    out_buf.bufs              = &out_ptr;
    out_buf.bufferIdentifiers = &out_identifier;
    out_buf.bufSizes          = &out_size;
    out_buf.bufElSizes        = &out_elem_size;

    // AAC-ELD: Apple per-frame header (BTAudioHALPlugin adds "AAC-ELD Size Header")
    int aaceld_hdr_size = (cur_codec == 4) ? 4 : 0;

    // For AAC-ELD, reserve enough space for a full frame (~350 bytes + header)
    int min_space = (cur_codec == 4) ? src_eld_au_space() : (aaceld_hdr_size + 50);

    // Relay coex: cap encode work per pass. A multi-granule catch-up batch
    // (up to 4 granules = 10-30 ms of CPU on real music) monopolizes the
    // shared core — measured as 27-46 ms max_tick_gap spikes -> TX jitter ->
    // sink-side concealment bursts. One granule per tick spreads the load
    // (pump ticks ~83/s, music needs ~47 granules/s — throughput unharmed).
    /* AAC-LC: 1 granule/tick suffices (83 ticks/s vs 47 needed). ELD needs
       ~100 AUs/s at its 10 ms tick — a cap of 1 is parity with NO catch-up
       margin (chronic starvation), so ELD gets 2. Uncapped ELD during relay
       bursts 10-30 ms of encode CPU into one radio-context pass. */
    int max_aus = 8;
    if (bt_sink_relay_streaming()) {
        if (cur_codec == 2)      max_aus = 1;
        else if (cur_codec == 4) max_aus = 2;
    }

    /* NO time budget here, deliberately (it came and went twice on
       2026-08-09). A 6 ms elapsed-time bound on this loop was added to stop
       the VBR+SBR-at-44.1 overload wedge, but that config is now impossible
       (src_eld_vbr_now forces CBR on the 44.1 pipeline, and 48 k VBR+SBR ran
       for weeks without a budget). Every build carrying the budget produced
       noise-then-silence in non-game ELD while every budget-free build with
       the same encoder config played clean — mechanism never identified by
       inspection (the early exit looks equivalent to the 3-frame break
       below), but the hardware correlation was 100%. If an encode-overload
       config ever returns, bound the CONFIG, not this loop. */
    while (context->samples_ready >= num_audio_samples_per_aac_buffer &&
           (context->max_media_payload_size - context->codec_storage_count) > min_space &&
           max_aus-- > 0) {

        uint8_t slot_idx;
        if (!audio_slot_pop(&slot_idx)) break;

        int hdr_offset = context->codec_storage_count + aaceld_hdr_size;

        if (cur_codec == 4 && eld_resample) {
            /* 441 frames of 44.1 in the slot -> exactly 480 frames @ 48 k. */
            resamp44_process((const int16_t *)slot_pool[slot_idx].data, 441,
                             eld_rs_buf, 480);
            in_ptr               = eld_rs_buf;
            in_size              = 480 * 2 * sizeof(int16_t);
            in_args.numInSamples = 480 * 2;
        } else {
            in_ptr               = slot_pool[slot_idx].data;
            in_size              = required_samples * sizeof(int16_t);
            in_args.numInSamples = required_samples;
        }
        out_ptr              = &context->codec_storage[hdr_offset];
        out_size             = sizeof(context->codec_storage) - hdr_offset;
        out_buf.bufs         = &out_ptr;
        out_buf.bufSizes     = &out_size;
        AACENC_ERROR err;

        watchdog_hw->scratch[7] = 0x3B;   /* entering aacEncEncode */
        err = aacEncEncode(handleAAC, &in_buf, &out_buf, &in_args, &out_args);
        watchdog_hw->scratch[7] = 0x3C;   /* encode returned */
        if (err != AACENC_OK) {
            printf("Error in AAC encoding %d, storage=%d/%d, out_size=%d\n",
                   err, context->codec_storage_count, (int)sizeof(context->codec_storage), out_size);
            audio_slot_release(slot_idx);
            break;
        }

        // Add Apple AAC-ELD Size Header per frame
        if (cur_codec == 4 && out_args.numOutBytes > 0) {
            uint16_t seq = aaceld_frame_sequence;
            aaceld_frame_sequence = (aaceld_frame_sequence + 1) & 0x0FFF;  // 12-bit wrap
            uint16_t au_size = out_args.numOutBytes;
            context->codec_storage[context->codec_storage_count + 0] = 0xB0 | ((seq >> 8) & 0x0F);
            context->codec_storage[context->codec_storage_count + 1] = seq & 0xFF;
            context->codec_storage[context->codec_storage_count + 2] = 0x10 | ((au_size >> 8) & 0x0F);
            context->codec_storage[context->codec_storage_count + 3] = au_size & 0xFF;
            context->codec_storage_count += 4 + out_args.numOutBytes;
        } else {
            context->codec_storage_count += out_args.numOutBytes;
        }

        total_samples_read += num_audio_samples_per_aac_buffer;
        context->codec_num_frames++;
        context->samples_ready -= num_audio_samples_per_aac_buffer;

        audio_slot_release(slot_idx);

        // AAC-ELD: max 3 frames per RTP packet
        if (cur_codec == 4 && context->codec_num_frames >= 3) break;
    }
    return total_samples_read;
}

#endif

static void a2dp_demo_timer_pause(a2dp_media_sending_context_t * context);
static void a2dp_demo_timer_stop(a2dp_media_sending_context_t * context);

/* Relay bitrate ladder (ESP32's decisive scatternet lever): while the phone
   streams into the sink relay, our TX airtime must shrink so the phone's RX
   slots survive — drop the AAC/ELD source encoder to 128 kbps, restore the
   negotiated rate when the relay stops. Applied ONLY from the pump tick (the
   single encoder owner — retuning from BT-event context was an ESP32
   use-after-free); FDK re-inits internally on the next aacEncEncode. */
uint32_t src_full_bitrate;      /* set at each encoder init */
uint8_t  src_vbr_mode;          /* BITRATEMODE applied at init (0 = CBR) */
bool     src_relay_rate_low;
static uint32_t src_tx_stall_drops;    /* 100ms TX outages toward the headset */
static uint32_t src_max_tick_gap_ms;   /* worst pump-tick scheduling latency  */
static uint32_t src_tick_last_ms;      /* zeroed at timer_start (pause guard) */

/* Deferred FDK work that must NOT run in BT/radio context (malloc storms
   deadlock against a main-loop malloc in flight). Called from main().
   IRQ-off around the storm: with interrupts held off, no BT-context
   allocation can interleave with ours, so the malloc lock is uncontended
   by construction. ~3-5 ms once per stall — the stream is suspended. */

/* ---- per-link TX power cap (CYW43439 vendor HCI) ---------------------------
   Infineon/Cypress Set_Transmit_Power: OGF 0x3F, OCF 0x026 — caps the
   closed-loop link power control for ONE ACL connection. BR/EDR range on
   the CYW43439 is up to +8 dBm. Lowering a link's cap trades its range
   for cleaner air on the other link (dual-stream coex lever). */
static const hci_cmd_t hci_vs_set_tx_power = { 0xFC26, "H1" };
static uint16_t src_con_handle;               /* headset ACL handle */

void bt_link_tx_power(uint16_t con_handle, int8_t dbm) {
    if (con_handle == 0) { printf("tx-power: no link\n"); return; }
    if (dbm > 8) dbm = 8;
    if (dbm < -24) dbm = -24;
    printf("tx-power: handle 0x%04x cap -> %d dBm\n", con_handle, dbm);
    hci_send_cmd(&hci_vs_set_tx_power, con_handle, (uint8_t)dbm);
}

uint16_t a2dp_source_con_handle(void) {
    return a2dp_is_connected_flag ? src_con_handle : 0;
}

/* Teardown resets the LED to idle — but NOT while a pairing scan is running.
   'p' sets LED_PAIRING and then drops the link, so the release events land
   AFTER it: the naive reset turned the fast pairing flash back into the solid
   idle LED and pressing 'p' looked like nothing happened. */
static void led_idle_unless_pairing(void){
    if (bt_pairing_active()) return;
    led_set_mode(LED_IDLE);
}

/* The headset itself closed the ACL (0x13 remote user terminated / 0x15
   remote power-off): a deliberate action — arm the same no-auto-redial hold
   as a console 'd' and kill any retry window already ticking. The AVDTP
   release arrives BEFORE the ACL reason, so one redial may already be in
   flight; it fails against a powered-off headset and this stops everything
   after it. Cleared the usual ways: playback stop+restart, an explicit dial,
   or the headset coming back on its own. Supervision timeout (0x08, walked
   out of range) deliberately keeps the redial — that one is worth retrying. */
void a2dp_source_remote_bye(uint16_t handle, uint8_t reason){
    if (reason != 0x13 && reason != 0x15) return;
    if (handle == 0 || handle != src_con_handle) return;
    /* Not every 0x13 is the headset's idea: after WE drop AVDTP ('d', 'p', a
       slot switch) the headset tidies up the now-idle ACL and that also
       reports 0x13. Those teardowns are already deliberate — announcing
       "headset ended the connection" there is just noise. */
    if (src_user_dc_hold) return;      /* we already stopped on purpose */
    if (bt_pairing_active()) return;   /* 'p': our drop, scan is running */
    src_user_dc_hold = true;
    src_dial_tries = 0;
    src_dial_window_ms = 0;
    /* A headset that says goodbye over HCI is the OPPOSITE of a wedged peer:
       the AVDTP release arrives first and already charged the wedge counter,
       so un-charge it here or four ordinary idle power-offs would latch the
       wedge brake on a perfectly healthy headset. */
    src_nostream_run = 0;
    btstack_run_loop_remove_timer(&src_dial_retry_timer);
    printf("headset ended the connection — auto-redial off (press c, or power it back on)\n");
}

/* ---- deferred AACP jitter push ---------------------------------------------
   User-prescribed game-mode order: "first set to 192k, THEN toggle game" —
   the jitter command must never hit the buds mid-decode right before a
   suspend/rate change (that crashed them). Queued here, pushed 500 ms after
   the next START-accept; sent immediately when no stream is running. */
static volatile uint8_t src_pending_jitter;   /* raw ms, 0 = none */
static btstack_timer_source_t src_jitter_timer;
static void src_jitter_handler(btstack_timer_source_t *ts){
    (void)ts;
    if (!src_pending_jitter) return;
    extern void aap_set_jitter_raw(uint8_t value);
    aap_set_jitter_raw(src_pending_jitter);
    src_pending_jitter = 0;
}
void a2dp_source_queue_jitter(uint8_t ms){
    extern void aap_set_jitter_raw(uint8_t value);
    if (!is_streaming){ aap_set_jitter_raw(ms); return; }
    src_pending_jitter = ms;
}

/* ---- suspended-stream self-heal (ESP32 port) -------------------------------
   AirPods suspend the stream when they sleep (case/out-of-ear/idle) and our
   single immediate "Auto-restarting stream..." START is rejected or ignored
   by the half-asleep buds — the stream then sat parked FOREVER (no sound
   after wake until a manual reconnect). ESP32 lesson: a suspended stream
   with no owner restarts itself — dwell 3 s, bounded to 3 tries per
   episode; the AAP wake/ear signal expedites it (fresh tries). For ELD the
   re-START gets fresh seq/ts + a main-loop encoder rebuild, the same
   hygiene as the TX-stall recovery (AirPods decoder resyncs cleanly). */
static uint32_t src_suspend_ms;      /* nonzero = SUSPENDED since this tick   */
static uint8_t  src_resume_tries;    /* bounded re-STARTs per suspend episode */
static btstack_timer_source_t src_selfheal_timer;

/* SIGNALING-WEDGE ESCAPE. A weak-signal walk can lose an AVDTP signaling
   exchange mid-flight (e.g. the buds' AVRCP STOP queued a CLOSE whose
   response died in the air) — BTstack has NO retransmit timeout, so the
   pending command wedges the initiator FOREVER: every later START/SUSPEND
   returns ERROR_CODE_COMMAND_DISALLOWED (12). Result on HW (SWD autopsy of
   a live mute: endpoint STREAMING, stop_stream=1 stuck, tx flowing): the
   stream "plays" into deaf buds and no refresh can ever be signaled — the
   worst state, indefinitely silent but connected. Policy (user spec): the
   ONLY acceptable outcomes are sound-recovered or disconnected. So: a
   disallowed signaling attempt while we think we're streaming = strike one
   + a 2 s retry probe (a legit in-flight transaction clears by then, and a
   probe suspend that goes through auto-restarts the stream = the cure); a
   second strike = the link is wedged -> drop the ACL (gap_disconnect works
   at HCI level, a stuck AVDTP command cannot block it). The teardown
   handler + redial machinery then rebuild a clean session with sound. */
static uint8_t  src_wedge_strikes;
static uint32_t src_wedge_last_ms;
static btstack_timer_source_t src_wedge_timer;
static void src_wedge_retry_handler(btstack_timer_source_t *ts);
static void src_sig_disallowed(const char *who) {
    uint32_t now = btstack_run_loop_get_time_ms();
    if (now - src_wedge_last_ms > 15000) src_wedge_strikes = 0;  /* stale episode */
    src_wedge_last_ms = now;
    src_wedge_strikes++;
    printf("signaling disallowed (%s) — wedge strike %u/2\n", who, src_wedge_strikes);
    if (src_wedge_strikes >= 2) {
        src_wedge_strikes = 0;
        uint16_t ch = a2dp_source_con_handle();
        printf("signaling WEDGED (stuck pending command) — dropping ACL 0x%04x for a clean reconnect\n", ch);
        /* open the dial-recovery window BEFORE dropping: the release handler
           calls src_dial_maybe_redial_after_drop(), which sees the window and
           redials in 1.5 s — the user gets sound back, not just silence-free */
        src_dial_window_ms = now;
        src_dial_total = 0;
        if (ch) gap_disconnect(ch);
        return;
    }
    btstack_run_loop_remove_timer(&src_wedge_timer);
    btstack_run_loop_set_timer_handler(&src_wedge_timer, src_wedge_retry_handler);
    btstack_run_loop_set_timer(&src_wedge_timer, 2000);
    btstack_run_loop_add_timer(&src_wedge_timer);
}
static void src_wedge_retry_handler(btstack_timer_source_t *ts) {
    (void)ts;
    if (!a2dp_is_connected_flag) { src_wedge_strikes = 0; return; }
    /* Probe with a suspend: if it is ACCEPTED the suspend-accept handler
       auto-restarts the stream with fresh seq/ts — i.e. the probe itself
       completes the refresh the wedge interrupted. If it is still
       disallowed, that is strike two -> ACL drop above. */
    int st = avdtp_source_suspend(media_tracker.avdtp_cid, media_tracker.local_seid);
    if (st == ERROR_CODE_SUCCESS) {
        printf("wedge probe: suspend accepted — stream refreshing\n");
    } else {
        src_sig_disallowed("wedge probe");
    }
}

static void src_resume_suspended(const char *why) {
    src_suspend_ms = btstack_run_loop_get_time_ms();   /* re-arm the dwell */
    printf("suspended stream: re-START (%s, try %u/3)%s\n",
           why, src_resume_tries, cur_codec == 4 ? " + ELD reset" : "");
    if (cur_codec == 4) {
        aaceld_frame_sequence = 1;
        media_tracker.rtp_timestamp = 0;
        src_eld_reset_pending = true;    /* encoder rebuilt in the main loop */
    }
    uint8_t st = avdtp_source_start_stream(media_tracker.avdtp_cid, media_tracker.local_seid);
    if (st == ERROR_CODE_COMMAND_DISALLOWED) src_sig_disallowed(why);
}

static void src_selfheal_handler(btstack_timer_source_t *ts) {
    btstack_run_loop_set_timer(ts, 1000);
    btstack_run_loop_add_timer(ts);
    if (!a2dp_is_connected_flag || src_suspend_ms == 0) return;
    if (suspend_recovery_pending) return;      /* TX-stall recovery owns it  */
    if (src_resume_tries >= 3) return;         /* gave up: user/wake resets  */
    if (btstack_run_loop_get_time_ms() - src_suspend_ms < 3000) return;
    src_resume_tries++;
    src_resume_suspended("self-heal");
}

/* AAP wake/ear hook: a bud coming back (worn, case-open wake) is the user
   returning — resume NOW with a fresh try budget instead of dwelling. */
void a2dp_source_on_aap_wake(void) {
    if (!a2dp_is_connected_flag || src_suspend_ms == 0) return;
    src_resume_tries = 1;                      /* fresh episode */
    src_resume_suspended("AAP wake");
}

void a2dp_source_selfheal_init(void) {
    btstack_run_loop_set_timer_handler(&src_selfheal_timer, src_selfheal_handler);
    btstack_run_loop_set_timer(&src_selfheal_timer, 1000);
    btstack_run_loop_add_timer(&src_selfheal_timer);
}

/* ---- parked-session watchdog ----------------------------------------------
   The recovery above only sees a RUNNING stream. A kick whose restart chain
   fails (HW: kick #8 -> buds dropped the ACL -> START rc 0x02 -> pump parked,
   pool jammed 6/6 while the host kept delivering) leaves a session nobody
   watches. Poll from BT-timer context: host streaming + session up + no A2DP
   stream for >5 s -> START, min 15 s between attempts. */
static btstack_timer_source_t src_park_timer;
static uint32_t src_park_since, src_park_start_ms;
static void src_park_check(btstack_timer_source_t *ts){
    btstack_run_loop_set_timer(ts, 2000);
    btstack_run_loop_add_timer(ts);
    uint32_t pnow = btstack_run_loop_get_time_ms();
    {   /* deferred link maintenance: packet types + diagnostics */
        uint16_t lch = a2dp_source_con_handle();
        if (lch != 0 && hci_can_send_command_packet_now()) {
            if (src_ptype_pending) {
                src_ptype_pending = false;
                hci_send_cmd(&hci_change_connection_packet_type, lch, 0xCC18);
                printf("[link] packet types 0xCC18 requested; role %s\n",
                       gap_get_role(lch) == HCI_ROLE_MASTER ? "central" : "PERIPHERAL");
            } else if (src_link_probe_step != 0) {
                switch (src_link_probe_step--) {
                    case 4: hci_send_cmd(&hci_read_rssi, lch);                       break;
                    case 3: hci_send_cmd(&hci_read_link_quality, lch);               break;
                    case 2: hci_send_cmd(&hci_read_failed_contact_counter, lch);     break;
                    default: hci_send_cmd(&usbpods_hci_read_auto_flush_timeout, lch); break;
                }
            }
        }
    }
    if (is_usb_streaming && !is_streaming && cur_codec == 4 &&
        media_tracker.avdtp_cid != 0) {
        if (src_park_since == 0) { src_park_since = pnow; return; }
        if (pnow - src_park_since > 5000 &&
            pnow - src_park_start_ms > 15000) {
            src_park_start_ms = pnow;
            if (!src_auto_kick_enable) {
                printf("[watchdog] stream parked with host live — START suppressed (experiment mode)\n");
            } else {
                src_park_starts++;
                printf("[watchdog] stream parked with host live -> START (total %lu)\n",
                       (unsigned long)src_park_starts);
                avdtp_source_start_stream(media_tracker.avdtp_cid, media_tracker.local_seid);
            }
        }
    } else {
        src_park_since = 0;
    }
}
void a2dp_source_parkwatch_init(void){
    btstack_run_loop_set_timer_handler(&src_park_timer, src_park_check);
    btstack_run_loop_set_timer(&src_park_timer, 2000);
    btstack_run_loop_add_timer(&src_park_timer);
}

/* Live bitrate change (web AirPods > More settings): rebuild the ELD
   encoder at the new rate via the deferred main-loop path — the same
   hygiene as the recovery STARTs (fresh seq/ts so the AirPods decoder
   resyncs cleanly). No-op unless an ELD stream is up; otherwise the new
   rate simply applies at the next encoder init. */
void a2dp_source_apply_eld_rate(void) {
    if (cur_codec != 4 || !a2dp_is_connected_flag) return;
    aaceld_frame_sequence = 1;
    media_tracker.rtp_timestamp = 0;
    src_eld_reset_pending = true;      /* encoder rebuilt in the main loop */
    printf("AAC-ELD bitrate -> %lu — pausing stream to apply\n",
           (unsigned long)src_eld_bitrate());
    /* Suspend first (user feedback: hot-swapping the encoder under a LIVE
       stream glitches the AirPods decoder). The SUSPEND-accept handler
       auto-restarts the stream, which lands on the freshly rebuilt encoder
       — the same choreography as the TX-stall recovery. */
    if (is_streaming) {
        a2dp_demo_timer_stop(&media_tracker);
        int st = avdtp_source_suspend(media_tracker.avdtp_cid, media_tracker.local_seid);
        if (st != ERROR_CODE_SUCCESS) {
            printf("suspend failed (%d) — applying live instead\n", st);
            a2dp_demo_timer_start(&media_tracker);
            /* disallowed = possible signaling wedge: the "live" apply cannot
               unmute wedged buds — count it so the escape ladder runs */
            if (st == ERROR_CODE_COMMAND_DISALLOWED) src_sig_disallowed("refresh suspend");
        } else {
            /* if the suspend-accept never arrives, the recovery timer (and
               behind it the self-heal) restarts the stream */
            suspend_recovery_pending = true;
            btstack_run_loop_remove_timer(&suspend_recovery_timer);
            btstack_run_loop_set_timer_handler(&suspend_recovery_timer, suspend_recovery_handler);
            btstack_run_loop_set_timer(&suspend_recovery_timer, SUSPEND_RECOVERY_TIMEOUT_MS);
            btstack_run_loop_add_timer(&suspend_recovery_timer);
        }
    }
}

void a2dp_source_main_work(void) {
    if (src_codec_release_pending) {
        src_codec_release_pending = false;
        /* Walk THE registry rather than naming codecs. The old list was
           hand-written (FDK + LDAC); LHDC was added to the registry afterwards
           and never added here, so an LHDC session leaked its encoder and the
           next AAC-ELD open failed with error 33 — the same shape as the Bose
           bug, one codec later. Every codec's release is NULL-safe and
           idempotent (AAC and ELD deliberately share codec_fdk_release).

           TWO guards, both load-bearing (HW 2026-08-09, three SWD autopsies):

           cur_codec == 0: a release event is not always the END of activity —
           an AirPods connect BEGINS with one (stale-session cleanup, then
           accept). Deferring the sweep let it fire after the accept had
           already opened the new encoder, freeing handleAAC out from under a
           live pump: use-after-free, then a wedge inside whatever FDK stage
           read the recycled memory first (rate loop, section writer and the
           MDCT, on successive crashes). If a session opened since the event,
           there is nothing stale to free — its configure path replaced any
           same-family handle. The rare loss of this race merely re-creates
           the old leak for one session (error 33, recoverable), never a free
           of live state.

           IRQ-off: the check and the sweep must be one atomic step against
           the BT IRQ, or a configure could open a handle mid-sweep and have
           it freed a microsecond later. Freeing with IRQs off in thread mode
           is the house pattern (the ELD rebuild below) and also sidesteps the
           malloc_mutex deadlock that killed the IRQ-context sweep. */
        uint32_t irq = save_and_disable_interrupts();
        if (cur_codec == 0) {
            for (int i = 0; codec_registry[i]; i++)
                if (codec_registry[i]->release) codec_registry[i]->release();
            restore_interrupts(irq);
            printf("Codecs released (main loop)\n");
        } else {
            restore_interrupts(irq);
        }
    }
    if (!src_eld_reset_pending) return;
    uint32_t irq = save_and_disable_interrupts();
    if (handleAAC != NULL) {
        aacEncClose(&handleAAC);
        handleAAC = NULL;
    }
    /* 0x03 = AAC core + SBR module. FDK's AACENC_SBR_MODE SetParam is
       SILENTLY ignored when the SBR module was not allocated at open
       (hEnvEnc NULL guard) — 0x01 made every past SBR attempt a no-op. */
    if (aacEncOpen(&handleAAC, 0x03, 2) == AACENC_OK) {
        aacEncoder_SetParam(handleAAC, AACENC_AOT, 39);
        src_full_bitrate = src_eld_bitrate();   /* ladder restore tracks the setting */
        aacEncoder_SetParam(handleAAC, AACENC_BITRATE, src_eld_bitrate());
        aacEncoder_SetParam(handleAAC, AACENC_SAMPLERATE, 48000);
        aacEncoder_SetParam(handleAAC, AACENC_CHANNELMODE, 2);
        aacEncoder_SetParam(handleAAC, AACENC_GRANULE_LENGTH, 480);
        aacEncoder_SetParam(handleAAC, AACENC_SBR_MODE, src_eld_sbr_now());
        if (src_eld_sbr_now())
            aacEncoder_SetParam(handleAAC, AACENC_SBR_RATIO, 1);
        aacEncoder_SetParam(handleAAC, AACENC_BITRATEMODE, src_eld_vbr_now());
        if (src_eld_vbr_now())
            aacEncoder_SetParam(handleAAC, AACENC_PEAK_BITRATE, SRC_ELD_PEAK_BITRATE);
        aacEncoder_SetParam(handleAAC, AACENC_AFTERBURNER, 0);
        aacEncoder_SetParam(handleAAC, AACENC_TRANSMUX, TT_MP4_RAW);
        aacEncEncode(handleAAC, NULL, NULL, NULL, NULL);
        aacEncInfo(handleAAC, &aacinf);
    }
    __dmb();
    src_eld_reset_pending = false;   /* cleared AFTER the new handle is live */
    restore_interrupts(irq);
    /* Queue depth tracks bitrate/gaming mode (9/4/6 slots) — resize only on
       an actual change so TX-stall recoveries don't drop buffered audio. */
    if (active_slot_count != src_eld_slot_count())
        audio_slot_queue_configure_with_count(acc_num_simples, src_eld_slot_count());
    printf("AAC-ELD: encoder reset (main loop)\n");
}
#define SRC_RELAY_BITRATE 128000

static void src_bitrate_ladder_tick(void) {
    bool want_low = bt_sink_relay_streaming();
    if (want_low == src_relay_rate_low) return;
    if (handleAAC == NULL || src_full_bitrate == 0) return;
    if (cur_codec == 4) {
        /* ELD coex: NEVER a live SetParam (re-inits inside the next
           aacEncEncode — unstable on AirPods). Run the proven suspend ->
           rebuild -> restart dance instead; the rebuild reads
           src_eld_bitrate(), which caps at 164 k while the relay streams
           and restores the menu rate when it stops. */
        src_relay_rate_low = want_low;
        printf("ELD coex: relay %s -> re-apply at %lu bps\n",
               want_low ? "streaming" : "idle",
               (unsigned long)src_eld_bitrate());
        a2dp_source_apply_eld_rate();
        return;
    }
    /* AAC-LC source: live SetParam is safe for LC (FDK re-inits cleanly). */
    if (cur_codec != 2) return;
    src_relay_rate_low = want_low;
    if (want_low) {
        /* Force CBR — in VBR mode AACENC_BITRATE is ignored, and we want
           deterministic airtime while sharing the radio with the phone. */
        aacEncoder_SetParam(handleAAC, AACENC_BITRATEMODE, 0);
        aacEncoder_SetParam(handleAAC, AACENC_BITRATE, SRC_RELAY_BITRATE);
    } else {
        aacEncoder_SetParam(handleAAC, AACENC_BITRATEMODE, src_vbr_mode);
        aacEncoder_SetParam(handleAAC, AACENC_BITRATE, src_full_bitrate);
    }
    printf("A2DP source bitrate -> %lu (relay %s)\n",
           (unsigned long)(want_low ? SRC_RELAY_BITRATE : src_full_bitrate),
           want_low ? "streaming" : "idle");
}

static void avdtp_audio_timeout_handler_inner(btstack_timer_source_t * timer);
static void avdtp_audio_timeout_handler(btstack_timer_source_t * timer){
    watchdog_hw->scratch[7] = 0x30;   /* tick entered (hang forensics)      */
    avdtp_audio_timeout_handler_inner(timer);
    watchdog_hw->scratch[7] = 0x31;   /* tick exited cleanly — a stuck 0x30/
                                         0x3F/0x3A = spin INSIDE the tick;
                                         0x31 = spin is elsewhere (SDK)     */
}
static void avdtp_audio_timeout_handler_inner(btstack_timer_source_t * timer){

    a2dp_media_sending_context_t * context = (a2dp_media_sending_context_t *) btstack_run_loop_get_timer_context(timer);
    btstack_run_loop_set_timer(&context->audio_timer, audio_timer_interval);
    btstack_run_loop_add_timer(&context->audio_timer);

    /* Live-close grace expiry (see audio_slot_live_close_grace): the host
       did not reopen in time — this is a genuine stop. Push-free window
       (EP closed), pool frozen against a last-instant reopen racing the
       flush. */
    if (src_grace_deadline != 0 &&
        (int32_t)(btstack_run_loop_get_time_ms() - src_grace_deadline) > 0) {
        src_grace_deadline = 0;
        extern uint16_t usb_stop_delay;
        extern void set_usb_streaming(bool flag);
        slot_pool_frozen = true;
        usb_stop_delay = 101;
        set_usb_streaming(false);
        audio_slot_flush_exec(true);
        slot_pool_frozen = false;
    }

    /* Preventive per-splice nudge (see src_pnudge_pending): BT context,
       2 s rate limit; restore lands 1 s later. Inaudible by test. */
    {
        uint32_t pnow = btstack_run_loop_get_time_ms();
        extern void aap_set_jitter_raw(uint8_t value);
        extern uint8_t aap_game_jitter(void);
        if (src_pnudge_pending && cur_codec == 4 && is_streaming &&
            pnow - src_pnudge_ms > 2000) {
            src_pnudge_pending = false;
            src_pnudge_ms = pnow;
            src_pnudge_restore_ms = pnow + 1000;
            src_pnudges++;
            aap_set_jitter_raw(60);
        }
        if (src_pnudge_restore_ms != 0 &&
            (int32_t)(pnow - src_pnudge_restore_ms) > 0) {
            src_pnudge_restore_ms = 0;
            aap_set_jitter_raw(settings()->game_level ? aap_game_jitter() : 150);
        }
    }

    /* FLUSH-DELTA recovery trigger: controller flushes are on-air holes the
       send side cannot see (no send gap, no stamp — HW: desk-range wedge
       after an early-session 52-flush burst with a flawless local stream).
       Any new flush arms the beacon-verified recovery; the quiesce gate
       defers it until the episode calms. This is the universal damage
       signal — covers fades, coex bursts and reconnect housekeeping alike. */
    {
        static uint32_t src_flush_seen;
        if (usbpods_flush_occurred_count != src_flush_seen) {
            /* Controller discarded stale media past the 200 ms deadline —
               normal, healthy behaviour on a fading link (question settled
               2026-07-29: the CYW43 does arm the timer). It still ARMS
               RECOVERY below; only the running commentary is telemetry, so
               it rides 'V' with the rest of the flush counters. */
            if (src_diag_log)
                printf("[flush] +%lu (total %lu)\n",
                       (unsigned long)(usbpods_flush_occurred_count - src_flush_seen),
                       (unsigned long)usbpods_flush_occurred_count);
            src_flush_seen = usbpods_flush_occurred_count;
            src_splice_last_ms = btstack_run_loop_get_time_ms();
            src_auto_kick_pending = true;
        }
    }

    /* ---- 1 Hz backlog telemetry (walk-test diagnostic) --------------------
       free ACL slots = controller TX buffers not yet acked. A healthy link
       sits at/near the total; a backlog pins it at 0 while we keep handing
       the controller packets it cannot deliver. Printed only while an ELD
       stream is actually running, so an idle dongle stays quiet. */
    {
        static uint32_t src_diag_ms;
        uint32_t dnow = btstack_run_loop_get_time_ms();
        uint16_t dh = a2dp_source_con_handle();
        int freeslots = (dh != 0) ? hci_number_free_acl_slots_for_handle(dh) : -1;
        src_freeacl_latch = freeslots;

        /* ---- MANUAL FLUSH: belt-and-braces behind the 200 ms deadline -----
           2026-07-28/29 history, in order: builds that REGISTERED the media
           CID auto-flushable logged flush_occurred climbing 0->53->81 during
           walks with no manual-flush code — the CYW43 timer apparently
           working. Commit 4ae498d then removed the registration (believing
           the SDK patch never existed), all media went out PB=0b00
           NON-flushable, and the timer legitimately went silent — the
           "controller never arms the timer" measurement was made against
           mis-marked packets (review-4ae498d-findings.md). Registration is
           back (STREAM_ESTABLISHED); this jam-breaker stays as insurance:
           if the controller cannot free a single TX buffer for 200 ms, we
           flush by hand. Enhanced Flush (0x39 completion) is used so any
           Flush Occurred (0x11) can ONLY be the automatic timer — the walk
           discriminates the defect question by event type alone. */
        if (is_streaming && cur_codec == 4 && dh != 0 && freeslots == 0) {
            if (src_full_since == 0) src_full_since = dnow;
            if (dnow - src_full_since >= 200 && dnow - src_manflush_ms >= 200 &&
                hci_can_send_command_packet_now()) {
                src_manflush_ms = dnow;
                src_full_since  = dnow;      /* re-arm the dwell */
                src_manual_flushes++;
                /* EFFECT-based escalation, never support-based: Enhanced
                   Flush Complete (0x39) arrives even when NOTHING was
                   flushable and zero packets were discarded (spec 7.3.66) —
                   completion is not proof of effect, so gating a fallback on
                   the 0x39 count could leave the jam-breaker permanently
                   inert exactly when media lost its flushable marking. But
                   an EFFECTIVE flush frees buffers: freeslots rises for at
                   least one tick and the else-branch resets the streak. A
                   queue still CONTINUOUSLY jammed 200 ms after a jam-breaker
                   fired means it freed nothing (unmarked media, unsupported
                   command) — THIS attempt escalates to the flushability-
                   blind classic HCI_Flush. Per-jam, no permanent latch. */
                bool classic = (src_manflush_streak > 0);
                if (classic) {
                    src_flush_fallbacks++;
                    hci_send_cmd(&hci_flush, dh);
                } else {
                    hci_send_cmd(&usbpods_hci_enhanced_flush, dh, 0x00);
                }
                src_manflush_streak++;
                printf("[flush] MANUAL %s on 0x%04x (jam >200 ms; total %lu)\n",
                       classic ? "HCI_Flush (enhanced freed nothing)" : "Enhanced_Flush",
                       dh, (unsigned long)src_manual_flushes);
                src_splice_last_ms = dnow;   /* damage signal: arm recovery */
                src_auto_kick_pending = true;
            }
        } else {
            src_full_since = 0;
            src_manflush_streak = 0;
        }

        /* 1 Hz link telemetry — the instrument the flush campaign was built
           on, now OFF by default: it is a wall of text during normal use.
           'V' turns it back on for a walk test. */
        if (src_diag_log && is_streaming && cur_codec == 4 && dnow - src_diag_ms >= 1000) {
            src_diag_ms = dnow;
            printf("[diag] freeacl %d  autoflush %lu  eflush %lu  manflush %lu  cflush %lu  txpkts %lu  snaps %lu  stalls %lu  usbdrops %lu\n",
                   freeslots,
                   (unsigned long)usbpods_flush_occurred_count,
                   (unsigned long)usbpods_enhanced_flush_done_count,
                   (unsigned long)src_manual_flushes,
                   (unsigned long)src_flush_fallbacks,
                   (unsigned long)src_tx_pkts,
                   (unsigned long)src_ts_snaps,
                   (unsigned long)src_tx_stall_drops,
                   (unsigned long)usb_slot_push_drops);
        }
    }

    /* Splice-churn auto-recovery v3 — KICK FIRST, 0x55 AS RECEIPT. v2 led
       with the gapless jitter nudge and burned 3-5 s of beacon-wait before
       the kick that actually heals; hardware shows the suspend->rebuild->
       restart kick relocks the buds in ~2 s at any wedge depth (macOS's own
       link adaptation is the same medicine — change the stream to re-lock
       the decoder). So: damage trigger -> quiesce gate -> kick NOW -> await
       the buds' 0x55 lock beacon as receipt; a silent 4 s arms a retry
       under the escalating cooldown. Escalations back off 12->30->60 s per
       episode: eight kicks in ~90 s once provoked the buds into dropping
       the whole ACL. Manual 'J' keeps the gapless nudge for experiments. */
    {
        uint32_t rnow = btstack_run_loop_get_time_ms();
        if (src_rec_escal && rnow - src_auto_kick_ms > 120000) src_rec_escal = 0;
        uint32_t rec_cool = (src_rec_escal == 0) ? 12000
                          : (src_rec_escal == 1) ? 30000 : 60000;
        extern uint32_t aap_last_lock_ms(void);
        extern bool     aap_renderer_live(void);
        extern uint32_t aap_last_render_ms(void);
        /* ---- 0x0E renderer beacon: RECEIPT, not a detector -----------------
           HW-PROVEN 2026-07-29: the buds emit NOTHING while wedged. Through a
           full weak-signal wedge (link quality 255/255, contact counter 0,
           tx_pkts still climbing, buds silent) not one AACP frame arrived.
           The 0x0E STOPPED frame only appeared 201 ms AFTER our own suspend —
           it is a RESPONSE to a host transition, never a distress signal.
           An earlier revision armed the kick on "renderer STOPPED while
           streaming"; that can never fire and has been removed.
           What it IS good for: on recovery the order is
             0x0E-stopped -> 0x55=25 -> [START] -> 0x0E-live -> 0x55=50,
           and 0x0E-live lands ~200 ms BEFORE the 0x55=50 lock report, so it
           confirms a successful kick sooner than waiting on 0x55 alone.
           The real damage signal is HCI Flush Occurred (btstack_hci.c). */
        switch (src_rec_state) {
            case 0:
                /* QUIESCE GATE: never attempt recovery while damage is
                   still landing (a splice every 2-3 s makes any attempt
                   futile, and mid-churn kick storms once provoked an ACL
                   drop). The latched pending fires 3 s after the LAST
                   damage signal — prompt post-churn cleanup instead of
                   serving the anti-storm backoff (HW: 'buggy sound needs a
                   long time to recover'). */
                if (src_auto_kick_pending && src_auto_kick_enable &&
                    rnow - src_splice_last_ms > 3000 &&
                    rnow - src_auto_kick_ms > rec_cool) {
                    /* No cancel branch at the retry gate — deliberately.
                       Any single-instant health snapshot here (beacon OR
                       link metrics: txmark is 30-60 s stale at gate-open,
                       freeacl is one sample) can declare victory over buds
                       that are still mute. A kick on a genuinely healthy
                       stream costs a ~200 ms blip; a cancelled retry on
                       mute buds costs permanent silence. The kick fires. */
                    if (cur_codec == 4 && is_streaming &&
                        rnow - src_started_ms >= 5000) {
                        /* LATCH FIX: only consume the request on a path that
                           actually kicks. Clearing it before the is_streaming
                           test threw the recovery away whenever the gate opened
                           while the stream was parked/suspended — and nothing
                           re-arms it, so the wedge became permanent. */
                        src_auto_kick_pending = false;
                        src_auto_kick_ms = rnow;
                        src_rec_t0 = rnow;
                        src_auto_kicks++;
                        printf("[auto] damage calm -> stream refresh NOW (kick %lu, backoff %u)\n",
                               (unsigned long)src_auto_kicks, src_rec_escal);
                        src_rec_state = 3;
                        a2dp_source_apply_eld_rate();
                        return;
                    }
                }
                break;
            case 3:   /* confirm on LINK HEALTH, not on a bud beacon */
                /* ---- WHY THE 0x55 RECEIPT WAS REMOVED (HW 2026-07-29) ----
                   0x55=50 (and 0x0E-live) prove only that the buds' render
                   pipeline RE-INITIALISED. They do NOT prove audio is coming
                   out. Measured counter-example: two consecutive kicks, each
                   running the full SUSPEND -> encoder reset -> START cycle,
                   each answered with 0x0E-live + 0x55=50 — and NO SOUND
                   throughout, because the real fault was a controller backlog
                   the kick cannot drain (see cyw43-flush-defect.md). Closing
                   the episode on the beacon declared victory over a silent
                   link AND reset the backoff, so nothing retried.
                   The buds cannot tell us "I am audible" — they emit nothing
                   at all while wedged. So confirm on what we CAN observe:
                   the link is no longer jammed. Healthy == we have gone
                   HEALTH_MS without needing a manual flush, TX is advancing,
                   and the controller has buffers free. That is the same
                   condition under which audio was actually restored on HW.
                   The beacons are kept as log-only breadcrumbs. */
                {
                    const uint32_t HEALTH_MS = 3000;
                    bool tx_advancing = (src_tx_pkts != src_rec_txmark);
                    bool link_free    = (src_freeacl_latch > 0);
                    bool no_recent_flush = (src_manflush_ms == 0) ||
                                           (rnow - src_manflush_ms > HEALTH_MS);
                    /* damage_quiet closes the auto-flush blind spot: with the
                       media CID registered, a degraded link auto-flushes every
                       200 ms — buffers get freed and TX keeps advancing, so
                       the first three legs all pass WHILE audio is being
                       discarded. Every auto-flush stamps src_splice_last_ms
                       (flush-delta trigger); a confirm without this term
                       reset the backoff each time and re-enabled the exact
                       kick-storm cadence the escalation exists to prevent. */
                    bool damage_quiet = (rnow - src_splice_last_ms > HEALTH_MS);
                    if (tx_advancing && link_free && no_recent_flush && damage_quiet &&
                        rnow - src_rec_t0 > HEALTH_MS) {
                        printf("[auto] kick confirmed (link healthy %lums: tx advancing, freeacl %d, no manflush)%s\n",
                               (unsigned long)(rnow - src_rec_t0), src_freeacl_latch,
                               (aap_last_lock_ms() != 0 &&
                                (int32_t)(aap_last_lock_ms() - src_rec_t0) > 0)
                               ? " [0x55 also seen]" : "");
                        src_rec_escal = 0;
                        src_rec_state = 0;
                    } else if (rnow - src_rec_t0 > 10000) {
                        src_rec_escal++;
                        src_auto_kick_pending = true;   /* still sick: retry */
                        printf("[auto] link still unhealthy after 10s -> retry armed (backoff %u)\n",
                               src_rec_escal);
                        src_rec_state = 0;
                    }
                    src_rec_txmark = src_tx_pkts;
                }
                break;
            default:
                src_rec_state = 0;
                break;
        }

        /* SYNC RE-ASSERT: EP open/close churn ratchets the buds' ADAPTIVE
           jitter buffer upward — audio drifts audibly out of sync while OUR
           whole pipeline holds zero staged samples (HW 2026-07-29: 97 EP
           closes, slots 0/6, credit 5 ms, freeacl ~6 — yet clearly late).
           Five SUSPEND->START kicks during the same churn did NOT drain it;
           ONE gapless AACP jitter-set at the SAME nominal value snapped
           sync back instantly (user-confirmed, mid-playback, inaudible).
           So: after a churn episode ends (any live splice, then 5 s calm —
           never mid-churn, the proven law), re-assert the nominal jitter
           once. Deferred while the recovery machine owns the stream. */
        {
            static uint32_t src_sync_splice_seen;
            static uint32_t src_sync_reassert_ms;
            if (src_live_splices != src_sync_splice_seen &&
                src_rec_state == 0 && !src_auto_kick_pending &&
                cur_codec == 4 && is_streaming &&
                rnow - src_splice_last_ms > 5000 &&
                rnow - src_sync_reassert_ms > 15000) {
                extern void aap_set_jitter_raw(uint8_t value);
                extern uint8_t aap_game_jitter(void);
                src_sync_splice_seen = src_live_splices;
                src_sync_reassert_ms = rnow;
                aap_set_jitter_raw(settings()->game_level ? aap_game_jitter() : 150);
                printf("[sync] post-churn jitter re-assert (splices %lu — drains the buds' ratcheted buffer)\n",
                       (unsigned long)src_live_splices);
            }
        }
    }
    uint32_t now = btstack_run_loop_get_time_ms();

    /* Latency probe: how late can a pump tick get? (run-loop congestion —
       decode/encode pile-ups, transport bursts — shows up here long before
       any counter trips). Read + reset via the 'R' dump. src_tick_last_ms
       is ZEROED at timer_start so pause/resume spans don't fake 30-second
       gaps (HW: a suspend/resume read as "max_tick_gap 36337ms"). */
    if (src_tick_last_ms != 0) {
        uint32_t gap = now - src_tick_last_ms;
        if (gap > src_max_tick_gap_ms) src_max_tick_gap_ms = gap;
    }
    src_tick_last_ms = now;

    watchdog_hw->scratch[7] = 0x3F;   /* pump encode body done */
    src_bitrate_ladder_tick();

    /* POST-STORM HYGIENE LADDER. A signal-bad episode can leave the buds'
       decoder MUTE while everything on our side streams perfectly (HW, SWD-
       verified: real PCM in, clean TX, healthy signaling, no sound). ONE
       refresh after the storm is NOT a reliable cure — the same dance fired
       manually minutes later unmuted the buds when the +5 s pass had failed.
       There is no mute sensor in A2DP, so ladder: refresh at +5 s clean
       air, again at +20 s, again at +45 s — each a ~200 ms blip, harmless
       if the sound was already back. After pass 3 the episode ends
       OPTIMISTICALLY (a clean recovery must never be rewarded with a
       disconnect — HW: 'it was playing good' when the old unconditional
       final rung dropped the link). The ACL-drop last resort moved to the
       RELAPSE path: a real storm arriving after 3 refreshes (see the
       storm-suspend site) means the refreshes are not holding — THAT is
       when a guaranteed-fresh session is worth a disconnect. An episode
       idle >2 min resets the pass count. ELD only: the mute phenomenon is
       the AirPods decoder — an SBC/AAC-LC headset must never ride a no-op
       ladder into a disconnect. */
    if (src_storm_pending && src_auto_kick_enable && is_streaming && cur_codec == 4) {
        static const uint32_t ladder_dwell[3] = { 5000, 20000, 45000 };
        uint32_t dwell = ladder_dwell[src_storm_passes > 2 ? 2 : src_storm_passes];
        if (now - src_storm_ms >= dwell &&
            now - src_started_ms >= 5000) {   /* stream itself stable too — a
                                                 stale pending flag refreshing a
                                                 FRESH stream parked it (HW:
                                                 "no sound on reconnect") */
            src_ladder_last_ms = now;
            src_storm_passes++;
            src_storm_ms = now;
            printf("post-storm hygiene: pass %u/3 — refreshing stream\n", src_storm_passes);
            a2dp_source_apply_eld_rate();
            if (src_storm_passes >= 3) src_storm_pending = false;   /* optimistic end */
        }
    }

    uint32_t update_period_ms = audio_timer_interval;
    if (context->time_audio_data_sent > 0){
        update_period_ms = now - context->time_audio_data_sent;
    }

    //printf("update_period_ms is %d\n", update_period_ms);
    //printf("a2dp_sample_rate() is %d\n", a2dp_sample_rate());


    uint32_t num_samples = (update_period_ms * a2dp_sample_rate()) / 1000;
    context->acc_num_missed_samples += (update_period_ms * a2dp_sample_rate()) % 1000;
    
    while (context->acc_num_missed_samples >= 1000){
        num_samples++;
        context->acc_num_missed_samples -= 1000;
    }
    context->time_audio_data_sent = now;
    context->samples_ready += num_samples;

    // Cap to prevent unbounded accumulation when USB audio pauses.
    // Sized in TIME (50 ms), not slots: the old 4-slot cap was 512 samples
    // for SBC — less than ONE tick's 441-sample credit at 44.1 k, so every
    // real-wait deferral clamped the credit and triggered the scrub below,
    // which then threw away LIVE audio slots (HW: scrubs 13094 in one
    // session = the "even worse" 44.1 audio).
    uint32_t max_ready = a2dp_sample_rate() / 20;
    if (max_ready < 4u * acc_num_simples) max_ready = 4u * acc_num_simples;
    if (context->samples_ready > max_ready) {
        context->samples_ready = max_ready;
        // The pacing debt above the cap was just discarded — scrub the
        // matching audio backlog too (oldest slots), or the pool stays
        // pinned near-full with only 4 slots of headroom and every later
        // run-loop hiccup drops live USB pushes (the ESP32 scrub lesson).
        // ONLY while the host is idle: that is the stale-backlog case the
        // scrub exists for. Mid-stream, the backlog is LIVE audio the
        // deferred pump will encode on the next tick — never drop it.
        if (!is_usb_streaming) {
            uint8_t stale;
            while (queue_get_level(&ready_queue) > 2 &&
                   queue_try_remove(&ready_queue, &stale)) {
                queue_try_add(&free_queue, &stale);
                usb_slot_scrubs++;
            }
        }
    }

    //printf("num_samples is %d\n", num_samples);
    //printf("samples_ready is %d\n", context->samples_ready);

    {
        static uint32_t send_wait_ticks = 0;
        static uint32_t fail_count = 0;
        if (context->codec_ready_to_send) {
            send_wait_ticks++;
            /* Flush-world stall budget: 3 s, not 100 ms. macOS keeps
               encoding + transmitting at full rate through 30 s of ~70 %
               loss; the old 100 ms drop manufactured the very mid-stream
               discontinuity that wedges the decoder (Priority 1b). With
               auto-flush freeing controller buffers, grants keep flowing —
               a genuinely absent grant for 3 s means the link is dying and
               the ladder/supervision own that. */
            if (send_wait_ticks > (3000 / audio_timer_interval)) {
                // Can't send for 100ms — drop data and retry
                /* SEQ REWIND (ELD): the staged AUs being dropped already
                   consumed Apple 12-bit sequence numbers at encode time —
                   discarding them punched a GAP in the on-air seq stream,
                   and the buds' decoder can wedge MUTE on such gaps with no
                   reset in sight (HW, SWD-verified: 8 grace-ridden stalls at
                   a bad-RF reconnect = 8 seq gaps = silent buds under a
                   perfectly healthy stream). Rewind the counter by the
                   dropped count so the air-visible sequence stays strictly
                   continuous — a drop becomes a plain content hole + RTP
                   snap, which the buds demonstrably tolerate (Windows'
                   per-sound EP closes). */
                if (cur_codec == 4 && context->codec_num_frames > 0) {
                    aaceld_frame_sequence = (uint16_t)
                        ((aaceld_frame_sequence - context->codec_num_frames) & 0x0FFF);
                }
                /* ...and suppress the RTP snap for the post-drop packet: with
                   the seq now CONTINUOUS, stamping the stall gap into the RTP
                   clock hands the buds contradictory signals (seq says
                   nothing lost, clock says 100 ms jumped) — audible as
                   noise/time-compression artifacts after drops (HW). Zeroing
                   the reference makes the drop a fully seamless splice: seq
                   continuous, clock continuous, content just skips. The
                   ~100 ms playout lag this leaves is absorbed by the buds'
                   jitter buffer and re-zeroed at the next ladder refresh. */
                src_eld_last_send_ms = 0;
                context->codec_ready_to_send = 0;
                context->codec_storage_count = 0;
                context->codec_num_frames = 0;
                context->samples_ready = 0;
                send_wait_ticks = 0;
                fail_count++;
                src_tx_stall_drops++;
                src_splice_last_ms = btstack_run_loop_get_time_ms();
                src_auto_kick_pending = true;   /* RF-stress recovery trigger */
                printf("TX stall: dropped ~100ms of staged audio (total %lu)\n",
                       (unsigned long)src_tx_stall_drops);
                /* defense in depth: EVERY stall-drop arms the hygiene ladder
                   (not only threshold storms) — sub-threshold drop clusters
                   at a bad-RF reconnect were invisible to it, leaving a
                   wedged decoder with no scheduled refresh. Consecutive
                   drops coalesce into one +5 s clean-air pass. Drops are
                   seamless splices now, so after a COMPLETED ladder a lone
                   drop is not worth re-opening the episode (and never worth
                   the relapse drop — that is reserved for real storms). */
                if (cur_codec == 4) {
                    uint32_t snow = btstack_run_loop_get_time_ms();
                    if (snow - src_ladder_last_ms > 120000) src_storm_passes = 0;
                    if (src_storm_passes < 3) {
                        src_ladder_last_ms = snow;
                        src_storm_ms = snow;
                        src_storm_pending = true;
                    }
                }

                // AAC-ELD: suspend immediately on first failure to reset AirPods
                // decoder. For other codecs, wait for 3 failures. GRACE WINDOW:
                // for ~8 s after a relay phone connects, its connect burst
                // starves our TX for a few hundred ms — ride through up to 3
                // stalls (300 ms of drops) instead of turning the blip into a
                // seconds-long suspend/encoder-reset/restart cycle (HW: every
                // relay start opened with "no sound -> unstable -> normal").
                int suspend_threshold = (cur_codec == 4) ? 1 : 3;
                if (cur_codec == 4 && bt_sink_relay_conn_age_ms() < 8000) {
                    suspend_threshold = 3;
                }
                /* STARTUP GRACE: right after a START the buds/baseband need
                   a beat (sniff exit, media path opening) — the first grants
                   routinely run >100 ms late. Threshold 1 turned that
                   settling delay into an instant suspend + seq/ts reset,
                   looping 3+ recovery cycles at EVERY stream start (HW logs:
                   started/stall/suspend/restart x3) — the buds' decoder
                   never locked: silent start, then noise while locking.
                   Ride the first 3 s out like the relay grace instead. */
                if (cur_codec == 4 &&
                    btstack_run_loop_get_time_ms() - src_started_ms < 3000) {
                    suspend_threshold = 3;
                }

                if (fail_count >= suspend_threshold) {
                    if (src_suspend_ms != 0) {
                        /* Stream is ALREADY suspended (self-heal armed) — a
                           suspend-for-recovery is both futile (status 12) and
                           the restart-on-failure re-fed this very stall loop
                           (HW: "dead loop" of stall/suspend-fail/restart at
                           10 Hz after a USB suspend). Park the pump; the
                           self-heal owns recovery. */
                        printf("TX stall while suspended — pump parked, self-heal owns recovery\n");
                        a2dp_demo_timer_stop(context);
                        fail_count = 0;
                        return;
                    }
                    printf("Signal bad suspending stream for recovery (codec=%d, fail=%d)\n",
                           cur_codec, fail_count);
                    /* post-storm hygiene: a recovery that lands while the
                       signal is STILL bad can leave the buds' decoder MUTE
                       with our stream happily "started" (HW: back from a
                       weak-signal walk, 69 stalls, tx flowing, no sound).
                       Arm a final clean-air pass — see the pump tick. */
                    src_storm_ms = btstack_run_loop_get_time_ms();
                    /* ladder episode bookkeeping: a storm long after the last
                       ladder activity is a NEW episode (fresh pass count). */
                    if (src_storm_ms - src_ladder_last_ms > 120000) src_storm_passes = 0;
                    src_ladder_last_ms = src_storm_ms;
                    /* RELAPSE after a full ladder: a REAL storm landing after
                       3 refreshes in this episode means the refreshes are not
                       holding — the ACL-drop last resort fires HERE (and only
                       here; a clean recovery ends the ladder quietly). The
                       redial builds a guaranteed-fresh session. */
                    if (src_storm_passes >= 3 && cur_codec == 4) {
                        src_storm_passes = 0;
                        src_storm_pending = false;
                        uint16_t wch = a2dp_source_con_handle();
                        printf("storm relapse after full ladder — dropping ACL 0x%04x for a guaranteed-fresh session\n", wch);
                        src_dial_window_ms = src_storm_ms;   /* release handler redials */
                        src_dial_total = 0;
                        if (wch) gap_disconnect(wch);
                        return;
                    }
                    src_storm_pending = true;

                    // Reset AAC-ELD encoder + sequence so both sides start fresh
                    if (cur_codec == 4) {
                        aaceld_frame_sequence = 1;
                        context->rtp_timestamp = 0;
                        /* The FDK close/open storm is DEFERRED to the main
                           loop (a2dp_source_main_work): mallocing HERE (BT
                           context) deadlocks against a main-loop malloc in
                           flight — e.g. the relay decoder opening as a phone
                           connects (HW: watchdog "loop starved >2s" right
                           after phone connect + TX stall). The stream is
                           suspended below, so nothing encodes meanwhile;
                           fill is also gated on this flag. */
                        src_eld_reset_pending = true;
                        printf("AAC-ELD: seq/ts cleared, encoder reset deferred to main loop\n");
                    }

                    a2dp_demo_timer_stop(context);
                    int suspend_status = avdtp_source_suspend(context->avdtp_cid, context->local_seid);
                    fail_count = 0;

                    if (suspend_status != ERROR_CODE_SUCCESS) {
                        if (src_suspend_ms != 0) {
                            printf("Suspend failed (status %d) while suspended — pump stays parked\n", suspend_status);
                        } else {
                            // Suspend failed immediately — restart timer directly
                            printf("Suspend failed (status %d), restarting timer\n", suspend_status);
                            a2dp_demo_timer_start(context);
                            if (suspend_status == ERROR_CODE_COMMAND_DISALLOWED)
                                src_sig_disallowed("recovery suspend");
                        }
                    } else {
                        // Arm recovery timer in case suspend response never arrives
                        suspend_recovery_pending = true;
                        btstack_run_loop_remove_timer(&suspend_recovery_timer);
                        btstack_run_loop_set_timer_handler(&suspend_recovery_timer, suspend_recovery_handler);
                        btstack_run_loop_set_timer(&suspend_recovery_timer, SUSPEND_RECOVERY_TIMEOUT_MS);
                        btstack_run_loop_add_timer(&suspend_recovery_timer);
                    }
                    return;
                }
            }
            return;
        }
        send_wait_ticks = 0;
        fail_count = 0;  // reset on successful send
    }

    /* Dispatch on OUR codec selection (cur_codec, latched at completed
       setup) — NOT the endpoint's remote_configuration. BTstack ZEROES
       remote_configuration when the endpoint resets during reconnect churn,
       and it can stay zeroed for the WHOLE session: AirPods cache our old
       configuration and accept OPEN+START without a fresh SET_CONFIGURATION.
       Zeroed config reads as AVDTP_CODEC_SBC (== 0); the old codec_type
       dispatch then fell into the SBC arm, whose anti-crash guard (the
       bus-fault fix) silently broke out — a ZOMBIE pump: timer ticking,
       nothing encoded, nothing sent, "Stream started." with no media, the
       buds SUSPEND after ~1 s, our auto-restart re-STARTs = endless
       started/paused loop with no sound. (HW: SWD autopsy of the live
       freeze — cur_codec 4, remote_configuration ALL ZEROS, tx_pkts frozen,
       prime stuck 1, slots pinned 6/6.) cur_codec cannot masquerade: 0
       already returned above. */
    if (cur_codec == 0) return;
    watchdog_hw->scratch[7] = 0x3A;   /* entering codec dispatch / fill / send */

    switch (cur_codec){

        case 1:   /* SBC */
            codec_sbc_ops.fill(context);
            /* +1: the SBC payload header ships with the frames (see the
               reservation in fill_sbc_audio_buffer) */
            if ((context->codec_storage_count + 1 + btstack_sbc_encoder_sbc_buffer_length()) > context->max_media_payload_size){
                // schedule sending
                context->codec_ready_to_send = 1;
                a2dp_source_stream_endpoint_request_can_send_now(context->avdtp_cid, context->local_seid);
            }
            break;
#ifdef HAVE_AAC_FDK
        case 2:   /* AAC-LC */
            if (context->codec_storage_count == 0){
                codec_fdk_fill(context);
            }

            if (context->codec_storage_count > 0) {
                // schedule sending
                context->codec_ready_to_send = 1;
                a2dp_source_stream_endpoint_request_can_send_now(context->avdtp_cid, context->local_seid);
            }
            break;
#endif
        case 3:   /* LDAC */
            /* Live quality retune, BEFORE the backlog early-return: a
               pending change must land even while TX is behind. */
            codec_ldac_tick();
            if (context->codec_ready_to_send)
                return;
            codec_ldac_ops.fill(context);

            if (context->codec_storage_count > 1) {
                context->codec_ready_to_send = 1;
                a2dp_source_stream_endpoint_request_can_send_now(context->avdtp_cid, context->local_seid);
            }
            break;
        case 5:   /* LHDC V5: 5 ms frames; fill stages ONE whole packet
                     (wrapper-flushed multi-frame payload + 2-byte header) */
            /* before the backlog early-return: a pending bitrate change must
               land even while TX is behind (same rule as the LDAC tick) */
            codec_lhdc_tick();
            if (context->codec_ready_to_send)
                return;
            codec_lhdc_ops.fill(context);

            if (context->codec_storage_count > 2) {
                context->codec_ready_to_send = 1;
                a2dp_source_stream_endpoint_request_can_send_now(context->avdtp_cid, context->local_seid);
            }
            break;
#ifdef HAVE_AAC_FDK
        case 4:   /* Apple AAC-ELD: encode on Core 0 (FDK-AAC not safe for Core 1) */
            codec_fdk_fill(context);

            /* CONSTANT CADENCE (splice-damage experiment): silence keepalives
               used to ship 1-AU/10 ms while music ships 3-AU/33 ms — every
               splice flapped the on-air rhythm, and the buds' ADAPTIVE
               jitter engine (the AACP 0x0B machinery) is the last untested
               damage suspect. Batch silence exactly like music so the air
               cadence never changes across closes. */
            if (context->codec_num_frames >= 3 ||
                (context->codec_num_frames > 0 &&
                 (context->max_media_payload_size - context->codec_storage_count) <= src_eld_au_space())) {
                context->codec_ready_to_send = 1;
                a2dp_source_stream_endpoint_request_can_send_now(context->avdtp_cid, context->local_seid);
            }
            break;
#endif
        default:
            break;
    }
}


static void a2dp_demo_timer_start(a2dp_media_sending_context_t * context){

    // AAC-ELD: max 986 bytes per RTP payload (Apple negotiated limit)
    // SBC/LDAC: 656 bytes — long-tuned, left alone.
    // AAC-LC: use the ACTUALLY NEGOTIATED media MTU. One AAC-LC AU is 1024
    // samples in one payload, so the payload size IS the bitrate ceiling
    // (656 B => ~234 kbps at 48 k); a sink that negotiates a bigger MTU can
    // have the higher rate it advertises instead of being clamped to the
    // classic 672-byte-MTU assumption.
    if (cur_codec == 4) {
        context->max_media_payload_size = 986;
    } else if (cur_codec == 2) {
        int m = avdtp_max_media_payload_size(context->avdtp_cid, context->local_seid);
        context->max_media_payload_size = (m >= 0x290 && m <= 1024) ? (uint16_t) m : 0x290;
    } else {
        context->max_media_payload_size = 0x290;
    }
    /* Discarding a staged-but-unsent batch here has TWO obligations the
       count-zero alone missed (HW: post-kick first packets shipped with
       num_frames 3 but 1-2 actual AUs — valid on air, but frames_sent
       over-advanced the RTP clock by the stale count at the exact moment
       the buds re-lock):
       1) codec_num_frames must die with codec_storage_count;
       2) the discarded AUs consumed Apple seqs — rewind, like the TX-stall
          drop does, so resumes never punch an on-air seq gap. Guarded on
          seq != 1: the kick path just reset the counter, nothing to rewind. */
    if (cur_codec == 4 && context->codec_num_frames > 0 &&
        aaceld_frame_sequence != 1) {
        aaceld_frame_sequence = (uint16_t)
            ((aaceld_frame_sequence - context->codec_num_frames) & 0x0FFF);
    }
    context->codec_num_frames = 0;
    context->codec_storage_count = 0;
    context->codec_ready_to_send = 0;
    context->streaming = 1;
    src_rtp_ms_base = 0;    /* fresh ms timeline for this stream session */
    src_tick_last_ms = 0;   /* pause/resume must not read as a giant tick gap */
    src_full_since = 0;     /* nor a pre-suspend jam as a 200 ms dwell served */
    src_manflush_streak = 0;
    if (src_rec_state == 3) {
        /* kick choreography (suspend->rebuild->restart) can exceed HEALTH_MS
           while the pump is stopped: measured from the kick decision, the
           first post-restart tick could confirm vacuously (txmark pre-kick,
           freeacl trivially free after a restart). Measure health from the
           RESTART instead. */
        src_rec_t0 = btstack_run_loop_get_time_ms();
        src_rec_txmark = src_tx_pkts;
    }
    if (sc.local_stream_endpoint != NULL &&
        sc.local_stream_endpoint->l2cap_media_cid != 0) {
        /* re-assert the flushable marking every stream start: repairs any
           ordering race where a stale RELEASED cleared a newer stream's
           registration (single global slot in the SDK patch) */
        l2cap_set_flushable_local_cid(sc.local_stream_endpoint->l2cap_media_cid);
    }
    src_eld_last_send_ms = 0;  /* same rule for the RTP hole-snap reference:
                                  without this, the first send after a resume
                                  saw gap = the WHOLE pause and kicked the RTP
                                  clock forward by it — a giant jump right as
                                  the buds re-lock (the startup mute/noise arc;
                                  buds reset their decoder at START, so the
                                  fresh stream must continue ts smoothly).
                                  Mid-stream holes (Windows per-sound EP
                                  close) still stamp — no suspend involved. */
    btstack_run_loop_remove_timer(&context->audio_timer);
    btstack_run_loop_set_timer_handler(&context->audio_timer, avdtp_audio_timeout_handler);
    btstack_run_loop_set_timer_context(&context->audio_timer, context);
    btstack_run_loop_set_timer(&context->audio_timer, audio_timer_interval);
    btstack_run_loop_add_timer(&context->audio_timer);
}

static void a2dp_demo_timer_stop(a2dp_media_sending_context_t * context){
    core1_encoder_active = false;  // stop Core 1 encoder
    suspend_recovery_pending = false;
    btstack_run_loop_remove_timer(&suspend_recovery_timer);
    context->time_audio_data_sent = 0;
    context->acc_num_missed_samples = 0;
    context->samples_ready = 0;
    context->streaming = 1;
    context->codec_storage_count = 0;
    context->codec_ready_to_send = 0;
    btstack_run_loop_remove_timer(&context->audio_timer);
}

static void a2dp_demo_timer_pause(a2dp_media_sending_context_t * context){
    is_streaming = false;
    context->time_audio_data_sent = 0;
    context->acc_num_missed_samples = 0;
    context->samples_ready = 0;
    context->streaming = 1;
    context->codec_storage_count = 0;
    context->codec_ready_to_send = 0;
    btstack_run_loop_remove_timer(&context->audio_timer);
} 




static void dump_remote_sink_endpoints(void){
    PLOG("Remote Endpoints:\n");
    int i;
    for (i=0;i<num_remote_seps;i++) {
        PLOG("- %u. remote seid %u\n", i, remote_seps[i].sep.seid);
    }
}

static int find_remote_seid(uint8_t remote_seid){
    int i;
    for (i=0;i<num_remote_seps;i++){
        if (remote_seps[i].sep.seid == remote_seid){
            return i;
        }
    }
    return -1;
}



#ifdef HAVE_AAC_FDK

#endif



static void packet_handler_inner(uint8_t packet_type, uint16_t channel, uint8_t *packet, uint16_t size);
static void packet_handler(uint8_t packet_type, uint16_t channel, uint8_t *packet, uint16_t size){
    watchdog_hw->scratch[7] = 0x40;   /* entered (hang forensics) */
    packet_handler_inner(packet_type, channel, packet, size);
    watchdog_hw->scratch[7] = 0x4F;   /* exited cleanly */
}
static void packet_handler_inner(uint8_t packet_type, uint16_t channel, uint8_t *packet, uint16_t size){
    if (packet_type != HCI_EVENT_PACKET) return;
    if (hci_event_packet_get_type(packet) != HCI_EVENT_AVDTP_META) return;
    UNUSED(channel);
    UNUSED(size);

    uint8_t signal_identifier;
    uint8_t status;
    avdtp_sep_t sep;
    uint16_t avdtp_cid;
    uint8_t remote_seid;
    int local_remote_seid_index;
    uint32_t vendor_id;
    uint16_t codec_id;
    bd_addr_t address;


    //printf("Current packet event is 0x%02x\n", packet[2]);

    /* Dual-role routing: BTstack broadcasts connection-level signaling events
       to BOTH the sink and source handlers, so the PHONE's events land here
       too. Anything owned by the relay must never drive the headset state
       machine (ESP32 relay lesson: one leaked event wedges the stream).
       Every AVDTP subevent leads with avdtp_cid. */
    {
        uint16_t ev_cid = little_endian_read_16(packet, 3);
        switch (packet[2]){
            case AVDTP_SUBEVENT_SIGNALING_CONNECTION_ESTABLISHED:
            case AVDTP_SUBEVENT_SIGNALING_CONNECTION_RELEASED:
                break;                     /* attributed inside the cases below */
            default:
                if (bt_sink_relay_owns_cid(ev_cid)) return;   /* phone event */
                break;
        }
    }

    switch (packet[2]){
        case AVDTP_SUBEVENT_SIGNALING_CONNECTION_ESTABLISHED:
            avdtp_subevent_signaling_connection_established_get_bd_addr(packet, address);
            avdtp_cid = avdtp_subevent_signaling_connection_established_get_avdtp_cid(packet);
            status = avdtp_subevent_signaling_connection_established_get_status(packet);
            if (status != 0){
                printf("AVDTP source signaling connection failed: status %d\n", status);
                /* STATUS 11 = ACL_CONNECTION_ALREADY_EXISTS (0x0B).
                   A Pico-side reset (SWD flash, watchdog, panic, brownout)
                   restarts us without the buds ever seeing a disconnect, so
                   they still hold the ACL. auto-connect then bounces off
                   "already exists", AVDTP never establishes, and there is NO
                   SOUND until the user replugs — which is the only thing that
                   forces a real teardown. HW-observed 2026-07-29 right after
                   an SWD flash.
                   Remedy: tear the stale ACL down so the next dial starts
                   clean. gap_disconnect on the existing handle triggers the
                   normal disconnect path, which re-arms auto-connect. */
                if (status == 0x0B) {
                    /* OUR controller holds nothing — BTstack frees a failed
                       connection before the event even reaches us, so any
                       handle/address lookup here is provably NULL (and the
                       old session-handle branch was equally dead; its
                       fallback ERASED the pairing for a transient condition,
                       review-4ae498d-findings.md). The stale session lives on
                       the BUDS' side until their supervision timeout clears
                       it (HW 2026-07-29: gone within ~7 retries). The one
                       remedy that works is to keep dialing PAST it: refund
                       the dial budget so the 8-dial give-up cannot exhaust
                       against it. NEVER touch the pairing for this. */
                    src_dial_total = 0;
                    printf("[recon] status 11 (buds still hold the old ACL) — dial budget refunded, retrying\n");
                }
                /* no retry pending -> this dial is DONE failing: LED solid */
                if (src_dial_tries == 0) led_idle_unless_pairing();
                break;
            }
            /* macOS parity: packet types + role. The old one-shot behind
               can_send_now silently SKIPPED when the queue was busy — the
               link may have been running narrow packets all along (53
               controller flushes/min at DESK range vs macOS baseline 0).
               Now latched: the park timer retries until both go out. */
            src_ptype_pending = true;
            /* Inbound connection from a non-slot address = a phone for the
               sink relay — its handler adopts it; do NOT touch headset state. */
            if (avdtp_cid != media_tracker.avdtp_cid && bt_sink_relay_is_phone_addr(address)){
                printf("AVDTP: inbound %s -> sink relay (phone), cid 0x%02x\n",
                       bd_addr_to_str(address), avdtp_cid);
                break;
            }
            /* A SECOND HEADSET while one is already connected. Every paired
               headset can page US (we stay connectable), so an idle one
               powering up mid-stream lands here — and everything below would
               overwrite media_tracker.avdtp_cid, wipe num_remote_seps and the
               codec notes, and re-run discovery ON TOP of the live stream
               (HW log: two capability scans back to back, then "stream
               already established — keeping it" with the cid now pointing at
               the wrong link). The relay phone is filtered above; this is the
               headset case. One headset at a time is the product model, so
               keep the one that is playing and drop the newcomer. */
            if (a2dp_is_connected_flag && avdtp_cid != media_tracker.avdtp_cid &&
                memcmp(address, cur_active_device, sizeof(bd_addr_t)) != 0) {
                uint16_t intruder = avdtp_subevent_signaling_connection_established_get_con_handle(packet);
                printf("AVDTP: %s connected while ", bd_addr_to_str(address));
                printf("%s is active — dropping the newcomer (one headset at a time)\n",
                       bd_addr_to_str(cur_active_device));
                if (intruder) gap_disconnect(intruder);
                break;
            }

            // print info
            printf("Current Device found: %s \n",  bd_addr_to_str(address));
            memcpy(cur_active_device, address, sizeof(bd_addr_t));

            media_tracker.avdtp_cid = avdtp_subevent_signaling_connection_established_get_avdtp_cid(packet);
            printf("AVDTP source signaling connection established: avdtp_cid 0x%02x\n", avdtp_cid);

            allow_switch_slot = false;
            src_user_dc_hold = false;   /* a live link voids the 'd' hold */
            led_set_mode(LED_IDLE);   /* connected — blink again only when audio flows */
            // seid selected per argv
            num_remote_seps = 0;
            selected_remote_sep_index = 0;
            codec_ldac_note_capability(false);
            codec_aaceld_note_capability(false);
            codec_lhdc_note_capability(false, NULL, 0);
            finish_setup_aac = false;
            src_codec_configured = false;   /* prove it per session */
            status = avdtp_source_discover_stream_endpoints(media_tracker.avdtp_cid);
            a2dp_is_connected_flag = true;
            src_stream_established = false;   /* fresh connection: no media channel yet */
            src_con_handle = avdtp_subevent_signaling_connection_established_get_con_handle(packet);
            /* HW-tested best value: full +8 dBm cap on every link ('T'/'W'
               console keys remain for experiments) */
            bt_link_tx_power(src_con_handle, 8);
            src_session_streamed = false;    /* prove it, per session */
            hfp_battery_arm_dial(address);   /* battery SLC, deferred 2 s */
            {   /* nameless slot (v1-era record): learn the name now */
                char snm[SLOT_NAME_LEN];
                read_slot_name(read_uint8_last_flash(), snm);
                if (snm[0] == 0) gap_remote_name_request(address, 0x02, 0);
            }
            src_dial_tries = 0;   /* dial landed: stop periodic retries (the 20 s
                                     window stays armed — a drop during setup
                                     redials via src_dial_maybe_redial_after_drop) */
            btstack_run_loop_remove_timer(&src_dial_retry_timer);

            break;
        
        case AVDTP_SUBEVENT_STREAMING_CONNECTION_ESTABLISHED:
            status = avdtp_subevent_streaming_connection_established_get_status(packet);
            if (status != 0){
                printf("Streaming connection failed: status %d\n", status);
                break;
            }
            avdtp_cid = avdtp_subevent_streaming_connection_established_get_avdtp_cid(packet);
            media_tracker.local_seid = avdtp_subevent_streaming_connection_established_get_local_seid(packet);
            media_tracker.remote_seid = avdtp_subevent_streaming_connection_established_get_remote_seid(packet);
            a2dp_is_connected_flag = true;
            hfp_battery_note_stream_started();
            /* macOS-parity PB regime (local SDK l2cap patch — which DOES
               exist in this tree, see sdk-patches/): ONLY the registered
               media channel goes out PB=0b10 auto-flushable; signaling and
               AACP stay 0b00 non-flushable so a fading link never flushes
               reliable protocol traffic. REGISTRATION IS LOAD-BEARING:
               without it the patched l2cap sends EVERYTHING 0b00 and the
               200 ms auto-flush timer legitimately never applies — that
               mis-marking sank the 4ae498d "CYW43 never arms the timer"
               measurements (review-4ae498d-findings.md). The live proof of
               correct marking is usbpods_flushable_tx_count counting up in
               the 'R' report — it reads 0 forever when this call is missing. */
            if (sc.local_stream_endpoint != NULL) {
                uint16_t mcid = sc.local_stream_endpoint->l2cap_media_cid;
                l2cap_set_flushable_local_cid(mcid);
                uint16_t afto = hci_automatic_flush_timeout();
                PLOG("[flush] media cid 0x%04x registered AUTO-FLUSHABLE (PB=0b10; control stays 0b00), timeout %u slots = %u ms\n",
                       mcid, afto, (unsigned)(afto * 5 / 8));
            }
            src_stream_established = true;   /* MEDIA channel is up (this is the
                                                reconnect-race discriminator —
                                                a2dp_is_connected_flag raises at
                                                SIGNALING, i.e. before every scan) */

            printf("Streaming connection established, avdtp_cid 0x%02x\n", avdtp_cid);
            codec_aac_on_media_open();   /* MTU is real now — lift the AAC rate */
            codec_ldac_on_media_open();  /* ...and fit LDAC's packing to it */
            // NOTE: the AACP channel is deliberately NOT opened here. Racing
            // its L2CAP connect against the AVDTP START + AVRCP connect on the
            // fresh ACL broke AVRCP on AirPods (the long-standing -aacp-repo
            // bug). It opens after AVRCP establishes — see avrcp_packet_handler.

            avdtp_source_start_stream(media_tracker.avdtp_cid, media_tracker.local_seid);
            avrcp_connect((uint8_t *) cur_active_device, &media_tracker.avrcp_cid);
            break;

        case AVDTP_SUBEVENT_SIGNALING_SEP_FOUND:
            memset(&sep, 0, sizeof(avdtp_sep_t));
            sep.seid = avdtp_subevent_signaling_sep_found_get_remote_seid(packet);;
            sep.in_use = avdtp_subevent_signaling_sep_found_get_in_use(packet);
            sep.media_type = avdtp_subevent_signaling_sep_found_get_media_type(packet);
            sep.type = avdtp_subevent_signaling_sep_found_get_sep_type(packet);
            PLOG("Found sep: seid %u, in_use %d, media type %d, sep type %d (1-SNK)\n", sep.seid, sep.in_use, sep.media_type, sep.type);
            if (sep.type == AVDTP_SINK) {
                remote_seps[num_remote_seps].sep = sep;
                num_remote_seps++;
            }
            break;

        case AVDTP_SUBEVENT_SIGNALING_SEP_DICOVERY_DONE:
            // select remote if there's only a single remote
            if (num_remote_seps == 1){
                media_tracker.remote_seid = remote_seps[0].sep.seid;
                PLOG("Only one remote Stream Endpoint with SEID %u, select it for initiator commands\n", media_tracker.remote_seid);
            }


            cur_num_remote_seps = 0;
            // find 1st capabilities; should be sbc
            avdtp_source_get_all_capabilities(media_tracker.avdtp_cid, remote_seps[0].sep.seid);

            break;

        case AVDTP_SUBEVENT_SIGNALING_CAPABILITIES_DONE:
            cur_num_remote_seps ++;
            if (cur_num_remote_seps < num_remote_seps){
                PLOG("\n\n Getting next CAPABILITY \n\n");
                avdtp_source_get_all_capabilities(media_tracker.avdtp_cid, remote_seps[cur_num_remote_seps].sep.seid);
            }else{
                PLOG("finish scaning CAPABILITIES codecs\n");
                set_next_capablity_and_start_stream();
            }
            break;


        case AVDTP_SUBEVENT_SIGNALING_MEDIA_TRANSPORT_CAPABILITY:
            PLOG("CAPABILITY - MEDIA_TRANSPORT supported on remote.\n");
            break;
        case AVDTP_SUBEVENT_SIGNALING_REPORTING_CAPABILITY:
            PLOG("CAPABILITY - REPORTING supported on remote.\n");
            break;
        case AVDTP_SUBEVENT_SIGNALING_RECOVERY_CAPABILITY:
            PLOG("CAPABILITY - RECOVERY supported on remote: \n");
            PLOG("    - recovery_type                %d\n", avdtp_subevent_signaling_recovery_capability_get_recovery_type(packet));
            PLOG("    - maximum_recovery_window_size %d\n", avdtp_subevent_signaling_recovery_capability_get_maximum_recovery_window_size(packet));
            PLOG("    - maximum_number_media_packets %d\n", avdtp_subevent_signaling_recovery_capability_get_maximum_number_media_packets(packet));
            break;
        case AVDTP_SUBEVENT_SIGNALING_CONTENT_PROTECTION_CAPABILITY:
            PLOG("CAPABILITY - CONTENT_PROTECTION supported on remote: \n");
            PLOG("    - cp_type           %d\n", avdtp_subevent_signaling_content_protection_capability_get_cp_type(packet));
            PLOG("    - cp_type_value_len %d\n", avdtp_subevent_signaling_content_protection_capability_get_cp_type_value_len(packet));
            PLOG("    - cp_type_value     \'%s\'\n", avdtp_subevent_signaling_content_protection_capability_get_cp_type_value(packet));
            break;
        case AVDTP_SUBEVENT_SIGNALING_MULTIPLEXING_CAPABILITY:
            PLOG("CAPABILITY - MULTIPLEXING supported on remote: \n");
            PLOG("    - fragmentation                  %d\n", avdtp_subevent_signaling_multiplexing_capability_get_fragmentation(packet));
            PLOG("    - transport_identifiers_num      %d\n", avdtp_subevent_signaling_multiplexing_capability_get_transport_identifiers_num(packet));
            PLOG("    - transport_session_identifier_1 %d\n", avdtp_subevent_signaling_multiplexing_capability_get_transport_session_identifier_1(packet));
            PLOG("    - transport_session_identifier_2 %d\n", avdtp_subevent_signaling_multiplexing_capability_get_transport_session_identifier_2(packet));
            PLOG("    - transport_session_identifier_3 %d\n", avdtp_subevent_signaling_multiplexing_capability_get_transport_session_identifier_3(packet));
            PLOG("    - tcid_1                         %d\n", avdtp_subevent_signaling_multiplexing_capability_get_tcid_1(packet));
            PLOG("    - tcid_2                         %d\n", avdtp_subevent_signaling_multiplexing_capability_get_tcid_2(packet));
            PLOG("    - tcid_3                         %d\n", avdtp_subevent_signaling_multiplexing_capability_get_tcid_3(packet));
            break;
        case AVDTP_SUBEVENT_SIGNALING_DELAY_REPORTING_CAPABILITY:
            PLOG("CAPABILITY - DELAY_REPORTING supported on remote.\n");
            break;
        case AVDTP_SUBEVENT_SIGNALING_DELAY_REPORT:
            PLOG("DELAY_REPORT received: %d.%0d ms, local seid %d\n", 
                avdtp_subevent_signaling_delay_report_get_delay_100us(packet)/10, avdtp_subevent_signaling_delay_report_get_delay_100us(packet)%10,
                avdtp_subevent_signaling_delay_report_get_local_seid(packet));
            break;
        case AVDTP_SUBEVENT_SIGNALING_HEADER_COMPRESSION_CAPABILITY:
            PLOG("CAPABILITY - HEADER_COMPRESSION supported on remote: \n");
            PLOG("    - back_ch   %d\n", avdtp_subevent_signaling_header_compression_capability_get_back_ch(packet));
            PLOG("    - media     %d\n", avdtp_subevent_signaling_header_compression_capability_get_media(packet));
            PLOG("    - recovery  %d\n", avdtp_subevent_signaling_header_compression_capability_get_recovery(packet));
            break;

        case AVDTP_SUBEVENT_SIGNALING_MEDIA_CODEC_SBC_CAPABILITY:
            // cache
            remote_seid = avdtp_subevent_signaling_media_codec_sbc_capability_get_remote_seid(packet);
            local_remote_seid_index = find_remote_seid(remote_seid);
            btstack_assert(local_remote_seid_index >= 0);
            (void) memcpy(remote_seps[local_remote_seid_index].media_codec_event, packet, size);
            remote_seps[local_remote_seid_index].sep.capabilities.media_codec.media_codec_type = AVDTP_CODEC_SBC;
            remote_seps[local_remote_seid_index].have_media_codec_apabilities = true;
            codec_sbc_on_capability(packet);
            break;

        case AVDTP_SUBEVENT_SIGNALING_MEDIA_CODEC_MPEG_AUDIO_CAPABILITY:
            // cache
            remote_seid = avdtp_subevent_signaling_media_codec_sbc_capability_get_remote_seid(packet);
            local_remote_seid_index = find_remote_seid(remote_seid);
            btstack_assert(local_remote_seid_index >= 0);
            (void) memcpy(remote_seps[local_remote_seid_index].media_codec_event, packet, size);
            remote_seps[local_remote_seid_index].sep.capabilities.media_codec.media_codec_type = AVDTP_CODEC_MPEG_1_2_AUDIO;
            remote_seps[local_remote_seid_index].have_media_codec_apabilities = true;
            PLOG("CAPABILITY - MEDIA_CODEC: MPEG AUDIO, remote seid %u: \n", remote_seid);

            break;

        case AVDTP_SUBEVENT_SIGNALING_MEDIA_CODEC_MPEG_AAC_CAPABILITY: {
            // cache
            remote_seid = avdtp_subevent_signaling_media_codec_sbc_capability_get_remote_seid(packet);
            local_remote_seid_index = find_remote_seid(remote_seid);
            btstack_assert(local_remote_seid_index >= 0);
            (void) memcpy(remote_seps[local_remote_seid_index].media_codec_event, packet, size);
            remote_seps[local_remote_seid_index].sep.capabilities.media_codec.media_codec_type = AVDTP_CODEC_MPEG_2_4_AAC;
            remote_seps[local_remote_seid_index].have_media_codec_apabilities = true;
            codec_aac_on_capability(packet);
            break;
        }

        case AVDTP_SUBEVENT_SIGNALING_MEDIA_CODEC_ATRAC_CAPABILITY:
            // cache
            remote_seid = avdtp_subevent_signaling_media_codec_sbc_capability_get_remote_seid(packet);
            local_remote_seid_index = find_remote_seid(remote_seid);
            btstack_assert(local_remote_seid_index >= 0);
            (void) memcpy(remote_seps[local_remote_seid_index].media_codec_event, packet, size);
            remote_seps[local_remote_seid_index].sep.capabilities.media_codec.media_codec_type = AVDTP_CODEC_ATRAC_FAMILY;
            remote_seps[local_remote_seid_index].have_media_codec_apabilities = true;
            PLOG("CAPABILITY - MEDIA_CODEC: ATRAC, remote seid %u: \n", remote_seid);

            break;

        case AVDTP_SUBEVENT_SIGNALING_MEDIA_CODEC_OTHER_CAPABILITY:
            // cache
            remote_seid = avdtp_subevent_signaling_media_codec_sbc_capability_get_remote_seid(packet);
            local_remote_seid_index = find_remote_seid(remote_seid);
            btstack_assert(local_remote_seid_index >= 0);
            (void) memcpy(remote_seps[local_remote_seid_index].media_codec_event, packet, size);
            remote_seps[local_remote_seid_index].sep.capabilities.media_codec.media_codec_type  = AVDTP_CODEC_NON_A2DP;
            remote_seps[local_remote_seid_index].have_media_codec_apabilities = true;
            const uint8_t *media_info = avdtp_subevent_signaling_media_codec_other_capability_get_media_codec_information(packet);
            vendor_id = get_vendor_id(media_info);
            codec_id = get_codec_id(media_info);

            remote_seps[local_remote_seid_index].vendor_id = vendor_id;
            remote_seps[local_remote_seid_index].codec_id = codec_id;

            if (codec_ldac_matches(vendor_id, codec_id))
            {
                codec_ldac_note_capability(true);
                PLOG("CAPABILITY - LDAC, remote seid %u\n", remote_seid);
            }
            else if (codec_aaceld_matches(vendor_id, codec_id))
            {
                codec_aaceld_note_capability(true);   /* 44.1 mode resamples 441->480 */
                PLOG("CAPABILITY - Apple AAC-ELD, remote seid %u\n", remote_seid);
                uint16_t info_len = avdtp_subevent_signaling_media_codec_other_capability_get_media_codec_information_len(packet);
                PLOG("  raw bytes (%u): ", info_len);
                for (int k = 0; k < info_len; k++) {
                    PLOG("%02x ", media_info[k]);
                }
                PLOG("\n");
            }
            else if (codec_lhdc_matches(vendor_id, codec_id))
            {
                uint16_t info_len = avdtp_subevent_signaling_media_codec_other_capability_get_media_codec_information_len(packet);
                codec_lhdc_note_capability(true, info_len >= 11 ? media_info : NULL, remote_seid);
                PLOG("CAPABILITY - LHDC V5, remote seid %u (info %u bytes)\n",
                     remote_seid, info_len);
            }
            else {
                PLOG("CAPABILITY - MEDIA_CODEC: OTHER, remote seid %u: \n", remote_seid);
                PLOG("vendor_id 0x%04x: \n", vendor_id);
                PLOG("codec_id: 0x%04x: \n", codec_id);
            }

            break;

        case AVDTP_SUBEVENT_SIGNALING_MEDIA_CODEC_SBC_CONFIGURATION:
            if (avdtp_subevent_signaling_media_codec_sbc_configuration_get_avdtp_cid(packet)
                    == media_tracker.avdtp_cid)
                src_codec_configured = true;
            codec_sbc_on_configuration(packet);
            break;

        case AVDTP_SUBEVENT_SIGNALING_MEDIA_CODEC_MPEG_AAC_CONFIGURATION:
            if (avdtp_subevent_signaling_media_codec_mpeg_aac_configuration_get_avdtp_cid(packet)
                    == media_tracker.avdtp_cid){
                src_codec_configured = true;
                /* Remote-first config: the finish_setup_aac gate exists to
                   filter the RELAY's sink-SEP configs (different cid), not a
                   headset configuring our own link. Without this the handler
                   early-returned and the encoder was never initialised. */
                finish_setup_aac = true;
            }
            codec_aac_on_configuration(packet);
            break;

        case AVDTP_SUBEVENT_SIGNALING_MEDIA_CODEC_MPEG_AUDIO_CONFIGURATION:
        case AVDTP_SUBEVENT_SIGNALING_MEDIA_CODEC_ATRAC_CONFIGURATION:
            // TODO: handle other configuration event
            printf("Config not handled for %s\n", codec_name_for_type(remote_seps[selected_remote_sep_index].sep.capabilities.media_codec.media_codec_type));
            break;
        case AVDTP_SUBEVENT_SIGNALING_MEDIA_CODEC_OTHER_CONFIGURATION:
            if (avdtp_subevent_signaling_media_codec_other_configuration_get_avdtp_cid(packet)
                    == media_tracker.avdtp_cid)
                src_codec_configured = true;
            PLOG("Received other configuration\n");
            uint8_t *codec_info = (uint8_t *) a2dp_subevent_signaling_media_codec_other_configuration_get_media_codec_information(packet);

            vendor_id = get_vendor_id(codec_info);
            codec_id = get_codec_id(codec_info);

            // LDAC
            if (codec_ldac_matches(vendor_id, codec_id)) {
                codec_ldac_on_configuration(packet);
            }
            // LHDC V5
            else if (codec_lhdc_matches(vendor_id, codec_id)) {
                codec_lhdc_on_configuration(packet);
            }
            // Apple AAC-ELD
            else if (codec_aaceld_matches(vendor_id, codec_id)) {
                PLOG("A2DP Source: Received Apple AAC-ELD configuration!\n");
                PLOG("  raw config bytes: ");
                uint16_t config_info_len = a2dp_subevent_signaling_media_codec_other_configuration_get_media_codec_information_len(packet);
                for (int k = 0; k < config_info_len; k++) {
                    PLOG("%02x ", codec_info[k]);
                }
                printf("\n");

                // Init FDK-AAC encoder for AAC-ELD (AOT 39)
                AACENC_ERROR err;
                // Close previous encoder if reconnecting
                if (handleAAC != NULL) {
                    aacEncClose(&handleAAC);
                    handleAAC = NULL;
                }
                if ((err = aacEncOpen(&handleAAC, 0x03, 2)) != AACENC_OK) {
                    printf("Couldn't open AAC-ELD encoder: %d\n", err);
                    break;
                }
                if ((err = aacEncoder_SetParam(handleAAC, AACENC_AOT, 39)) != AACENC_OK) {
                    printf("Couldn't set AAC-ELD AOT: %d\n", err);
                    break;
                }
                src_full_bitrate = src_eld_bitrate();  /* ladder restore point */
                src_vbr_mode = 0;                /* ELD runs CBR */
                src_relay_rate_low = false;
                if ((err = aacEncoder_SetParam(handleAAC, AACENC_BITRATE, src_eld_bitrate())) != AACENC_OK) {
                    printf("Couldn't set AAC-ELD bitrate: %d\n", err);
                    break;
                }
                if ((err = aacEncoder_SetParam(handleAAC, AACENC_SAMPLERATE, 48000)) != AACENC_OK) {
                    printf("Couldn't set AAC-ELD sample rate: %d\n", err);
                    break;
                }
                if ((err = aacEncoder_SetParam(handleAAC, AACENC_CHANNELMODE, 2)) != AACENC_OK) {
                    printf("Couldn't set AAC-ELD channel mode: %d\n", err);
                    break;
                }
                // 480 samples = 10ms at 48kHz (AAC-ELD-Stereo48K-10ms mode 130)
                if ((err = aacEncoder_SetParam(handleAAC, AACENC_GRANULE_LENGTH, 480)) != AACENC_OK) {
                    printf("Couldn't set AAC-ELD frame size: %d\n", err);
                    break;
                }
                if ((err = aacEncoder_SetParam(handleAAC, AACENC_SBR_MODE, src_eld_sbr_now())) != AACENC_OK) {
                    printf("Couldn't set AAC-ELD SBR mode: %d\n", err);
                    break;
                }
                if (src_eld_sbr_now() &&
                    (err = aacEncoder_SetParam(handleAAC, AACENC_SBR_RATIO, 1)) != AACENC_OK) {
                    printf("Couldn't set AAC-ELD SBR ratio: %d\n", err);
                    break;
                }
                if ((err = aacEncoder_SetParam(handleAAC, AACENC_BITRATEMODE, src_eld_vbr_now())) != AACENC_OK) {
                    printf("Couldn't set AAC-ELD VBR mode: %d\n", err);
                    break;
                }
                if (src_eld_vbr_now() &&
                    (err = aacEncoder_SetParam(handleAAC, AACENC_PEAK_BITRATE, SRC_ELD_PEAK_BITRATE)) != AACENC_OK) {
                    printf("Couldn't set AAC-ELD peak bitrate: %d\n", err);
                    break;
                }
                if ((err = aacEncoder_SetParam(handleAAC, AACENC_AFTERBURNER, 0)) != AACENC_OK) {
                    printf("Couldn't disable AAC-ELD afterburner: %d\n", err);
                    break;
                }
                // Raw access units with Apple 4-byte per-frame header
                if ((err = aacEncoder_SetParam(handleAAC, AACENC_TRANSMUX, TT_MP4_RAW)) != AACENC_OK) {
                    printf("Couldn't set AAC-ELD transport: %d\n", err);
                    break;
                }
                if ((err = aacEncEncode(handleAAC, NULL, NULL, NULL, NULL)) != AACENC_OK) {
                    printf("Couldn't initialize AAC-ELD encoder: %d\n", err);
                    break;
                }
                if ((err = aacEncInfo(handleAAC, &aacinf)) != AACENC_OK) {
                    printf("Couldn't get AAC-ELD encoder info: %d\n", err);
                    break;
                }

                PLOG("AAC-ELD setup complete! frameLength=%d, inputChannels=%d, bitrate=%lu (encoder reports %lu, sbr=%lu ratio=%lu vbr=%lu)\n",
                       aacinf.frameLength, aacinf.inputChannels,
                       (unsigned long)src_eld_bitrate(),
                       (unsigned long)aacEncoder_GetParam(handleAAC, AACENC_BITRATE),
                       (unsigned long)aacEncoder_GetParam(handleAAC, AACENC_SBR_MODE),
                       (unsigned long)aacEncoder_GetParam(handleAAC, AACENC_SBR_RATIO),
                       (unsigned long)aacEncoder_GetParam(handleAAC, AACENC_BITRATEMODE));

                eld_resample = (settings()->usbset & USBSET_RATE44) != 0;
                if (eld_resample) {
                    /* 44.1 pipeline: pop 441-frame slots (10 ms @ 44.1 =
                       3 x 147 polyphase periods -> EXACTLY one 480-sample
                       48 k granule). Encoder + RTP clock stay 48 k. */
                    current_sample_rate = 44100;
                    acc_num_simples = 441;
                    resamp44_reset();
                    printf("AAC-ELD: 44.1 pipeline, resampling 441->480 per granule\n");
                } else {
                    current_sample_rate = 48000;
                    acc_num_simples = aacinf.frameLength;  // use actual frame size from encoder
                }
                rtp_samples_per_frame = aacinf.frameLength;   /* 480: 48 k RTP clock */
                audio_timer_interval = 10;

                PLOG("AAC-ELD acc_num_simples=%d\n", acc_num_simples);

                audio_slot_queue_configure_with_count(acc_num_simples, src_eld_slot_count());

                // Core 1 path exists, but FDK-AAC is currently encoded on Core 0.
                aaceld_frame_sequence = 1;
                core1_encoder_active = false;
                PLOG("AAC-ELD Core 0 encoder active\n");

                cur_codec = CODEC_ID_AACELD;

                avdtp_source_open_stream(media_tracker.avdtp_cid, media_tracker.local_seid, media_tracker.remote_seid);
            }
            else
            {
                printf("Config not handled for %s\n", codec_name_for_type(remote_seps[selected_remote_sep_index].sep.capabilities.media_codec.media_codec_type));
            }
            break;

        case AVDTP_SUBEVENT_STREAMING_CAN_SEND_MEDIA_PACKET_NOW:
            a2dp_demo_send_media_packet();
            break;  


        case AVDTP_SUBEVENT_SIGNALING_ACCEPT:
            // TODO check cid
            src_wedge_strikes = 0;   /* an accepted command proves signaling alive */
            signal_identifier = avdtp_subevent_signaling_accept_get_signal_identifier(packet);
            if (is_cmd_triggered_localy){
                is_cmd_triggered_localy = 0;
                printf("AVDTP Source command accepted\n");
            }
            
            switch (signal_identifier){
                case AVDTP_SI_OPEN:
                    break;
                case AVDTP_SI_SET_CONFIGURATION:
                    break;
                case  AVDTP_SI_START:
                    printf("Stream started.\n");
                    src_started_ms = btstack_run_loop_get_time_ms();
                    if (cur_codec == 4) src_prime_pending = true;   /* start prime */
                    if (src_pending_jitter){   /* game jitter AFTER the rate dance */
                        btstack_run_loop_remove_timer(&src_jitter_timer);
                        btstack_run_loop_set_timer_handler(&src_jitter_timer, src_jitter_handler);
                        btstack_run_loop_set_timer(&src_jitter_timer, 500);
                        btstack_run_loop_add_timer(&src_jitter_timer);
                    }
                    src_suspend_ms = 0;          /* streaming: disarm self-heal */
                    src_resume_tries = 0;
                    if (finish_scan_avdtp_codec){
                        a2dp_demo_timer_start(&media_tracker);
                        is_streaming = true;
                        src_session_streamed = true;  /* audio: not a wedged peer */
                        src_nostream_run = 0;
                        src_wedge_hold   = false;     /* it streamed: unlatch */
                        start_led_blink();
                    }
                    break;
                case AVDTP_SI_SUSPEND:
                    printf("Stream paused.\n");
                    // Cancel recovery timer — suspend response arrived
                    suspend_recovery_pending = false;
                    btstack_run_loop_remove_timer(&suspend_recovery_timer);
                    a2dp_demo_timer_pause(&media_tracker);
                    /* Arm the self-heal dwell: if the immediate restart below
                       is rejected/ignored (asleep AirPods), the 1 s timer
                       re-STARTs after 3 s, bounded to 3 tries. */
                    src_suspend_ms = btstack_run_loop_get_time_ms();
                    // Auto-restart: ask to resume so AirPods reset their decoder
                    printf("Auto-restarting stream...\n");
                    {   uint8_t rst = avdtp_source_start_stream(media_tracker.avdtp_cid, media_tracker.local_seid);
                        if (rst == ERROR_CODE_COMMAND_DISALLOWED) src_sig_disallowed("auto-restart");
                    }
                    break;
                case AVDTP_SI_ABORT:
                case AVDTP_SI_CLOSE:
                    printf("Stream released.\n");
                    a2dp_demo_timer_stop(&media_tracker);
                    break;
                default:
                    break;
            }
            break;

        case AVDTP_SUBEVENT_SIGNALING_REJECT:
            signal_identifier = avdtp_subevent_signaling_reject_get_signal_identifier(packet);
            printf("Rejected %s\n", avdtp_si2str(signal_identifier));
            /* SET_CONFIGURATION rejects arrive ASYNCHRONOUSLY, long after
               set_next_codec() returned "command sent OK" — so the selection
               loop had already finished and NOTHING tried the next codec. A
               sink that dislikes one codec was left with no stream at all
               (HW: ROSE OpenFeel rejects our AAC config; the dongle sat
               connected with codec 0 instead of falling back to SBC).
               BTstack punts on this too ("TODO: implement retry"). Park the
               offending codec for this session and run the ladder again. */
            if (signal_identifier == AVDTP_SI_SET_CONFIGURATION) {
                if (src_attempt_codec != CODEC_ID_NONE) {
                    src_codec_reject_mask |= (uint8_t)(1u << src_attempt_codec);
                    const codec_ops_t *rej = codec_by_id(src_attempt_codec);
                    printf("codec select: %s rejected by the sink — trying the next one\n",
                           rej ? rej->name : "?");
                }
                if (!src_stream_established && !is_streaming) {
                    int r = set_next_codec(0);
                    printf("codec select: retry result %d\n", r);
                }
                break;
            }
            // If suspend or start was rejected during recovery, restart timer
            if (signal_identifier == AVDTP_SI_SUSPEND || signal_identifier == AVDTP_SI_START) {
                suspend_recovery_pending = false;
                btstack_run_loop_remove_timer(&suspend_recovery_timer);
                /* START rejected while SUSPENDED (asleep AirPods): the stream
                   is still parked — pumping the timer into it just churns TX
                   stalls. Leave it to the self-heal dwell instead. */
                if (signal_identifier == AVDTP_SI_START && src_suspend_ms != 0) {
                    printf("START rejected while suspended — self-heal will retry\n");
                    break;
                }
                if (a2dp_is_connected_flag && src_suspend_ms == 0) {
                    printf("Restarting audio timer after reject\n");
                    a2dp_demo_timer_start(&media_tracker);
                }
            }
            break;
        case AVDTP_SUBEVENT_SIGNALING_GENERAL_REJECT:
            signal_identifier = avdtp_subevent_signaling_general_reject_get_signal_identifier(packet);
            printf("Rejected %s\n", avdtp_si2str(signal_identifier));
            if (signal_identifier == AVDTP_SI_SUSPEND || signal_identifier == AVDTP_SI_START) {
                suspend_recovery_pending = false;
                btstack_run_loop_remove_timer(&suspend_recovery_timer);
                if (a2dp_is_connected_flag && src_suspend_ms == 0) {
                    printf("Restarting audio timer after general reject\n");
                    a2dp_demo_timer_start(&media_tracker);
                }
            }
            break;
        case AVDTP_SUBEVENT_STREAMING_CONNECTION_RELEASED:
            l2cap_set_flushable_local_cid(0);   /* stale cid must never mark a
                                                   later channel flushable */
            a2dp_demo_timer_stop(&media_tracker);
            printf("Streaming connection released.\n");
            src_storm_pending = false;   /* storm history dies with the stream */
            src_storm_passes = 0;
            src_rec_state = 0;           /* recovery episode dies with it too —
                                            a stale state-3 t0/txmark must not
                                            judge the NEXT session's link */
            src_auto_kick_pending = false;
            src_rec_escal = 0;
            src_manflush_ms = 0;
            src_full_since = 0;
            led_idle_unless_pairing();
            is_streaming = false;
            src_stream_established = false;
            break;
        case AVDTP_SUBEVENT_SIGNALING_CONNECTION_RELEASED:
            /* cid check, not owns_cid: the sink handler may already have
               cleared its cid for this same (broadcast) event. Only the
               HEADSET's release may tear the source state down. */
            if (little_endian_read_16(packet, 3) != media_tracker.avdtp_cid) break;
            a2dp_demo_timer_stop(&media_tracker);
            finish_scan_avdtp_codec = false;
            if (a2dp_is_connected_flag && (read_settings_byte() & USBSET_PAUSEDC)) {
                usb_hid_consumer(0x00B1);   /* headset gone -> pause the host */
            }
            a2dp_is_connected_flag = false;
            src_suspend_ms = 0;         /* teardown: disarm the self-heal */
            src_resume_tries = 0;
            src_wedge_strikes = 0;      /* wedge history dies with the link */
            btstack_run_loop_remove_timer(&src_wedge_timer);
            src_dial_maybe_redial_after_drop();
            if (src_slot_redial_pending){
                /* console slot switch: the old link is down, cur_slot already
                   points at the picked slot — dial it now (one-shot) */
                src_slot_redial_pending = false;
                a2dp_source_reconnect();
            }
            cur_capability = 0;
            src_codec_reject_mask = 0;   /* a new session re-offers everything:
                                            a sink can change its mind (and
                                            these buds change SEP sets between
                                            modes) */
            src_attempt_codec = CODEC_ID_NONE;
            src_storm_pending = false;   /* storm history dies with the link */
            src_storm_passes = 0;
            led_idle_unless_pairing();
            codec_ldac_note_capability(false);
            codec_aaceld_note_capability(false);
            codec_lhdc_note_capability(false, NULL, 0);
            finish_setup_aac = false;
            src_codec_configured = false;
            media_tracker.avdtp_cid = 0;   /* stale cid must never match a later phone cid */
            /* Release the codec instances — the pump is stopped, and holding
               ~100 KB of encoder while idle starves the relay's AAC decoder
               (and vice versa: a leaked ELD encoder starved the next AAC
               open — the error-33 Bose bug). Reconnect re-opens per config. */
            cur_codec = 0;   /* encoders are GONE: no pump dispatch may run
                                one until the next codec setup completes */
            /* Hand off to the main loop: release() frees, and freeing from
               this IRQ can deadlock on malloc_mutex (see the flag's comment).
               cur_codec is already 0, so nothing dispatches an encoder in the
               few ms before the sweep runs. */
            src_codec_release_pending = true;
            /* local = we asked for this teardown ('d', slot switch, pairing);
               only a teardown we did NOT request can implicate our SLC. */
            hfp_battery_note_link_teardown(src_user_dc_hold || bt_pairing_active());
            hfp_battery_link_down();
            printf("Signaling connection released.\n");
            break;
        default:
            break;
    }
}

void increase_vol_by_key(){
    if (media_tracker.volume > 117){
         media_tracker.volume = 127;
    } else {
                media_tracker.volume += 10;
    }
    printf(" - volume up (via set absolute volume) %d%% (%d)\n",  media_tracker.volume * 100 / 127,  media_tracker.volume);
    avrcp_controller_set_absolute_volume(media_tracker.avrcp_cid, media_tracker.volume);
}


void decrease_vol_by_key(){

    if (media_tracker.volume < 10){
        media_tracker.volume = 0;
    } else {
        media_tracker.volume -= 10;
    }
    printf(" - volume down (via set absolute volume) %d%% (%d)\n",  media_tracker.volume * 100 / 127,  media_tracker.volume);
    avrcp_controller_set_absolute_volume(media_tracker.avrcp_cid, media_tracker.volume);
    
}

#ifdef HAVE_BTSTACK_STDIN

/* all-FF (erased) and all-00 both mean "no device here" */
/* Advanced/debug console keys — they live here because they read and write
   this module's internals. Returns false for anything it does not own, so
   console.c can handle the user-facing tier. */
void set_bt_volume(int16_t val){
    media_tracker.volume = (val*2 + 100) * 127 / 100;
    src_vol_cmds++;   /* profile switches storm this path — see 'R' report */
    avrcp_controller_set_absolute_volume(media_tracker.avrcp_cid, media_tracker.volume);
}

/* --- console bridge: the few internals the UI needs, named ---------------- */

bool a2dp_source_is_playing(void){
    return is_streaming && is_usb_streaming;
}

void a2dp_source_set_debug_mode(bool on){
    extern volatile bool aap_rx_log;
    src_proto_log = on;    /* AVDTP capability scan + raw codec-config dumps */
    aap_rx_log    = on;    /* AACP frames + per-connect settings echoes      */
}

void a2dp_source_console_disconnect(void){
    /* the full user-disconnect path (same as the web button). The old bare
       avdtp_source_disconnect() left the retry window armed, so the stale-
       session redialer — or connmode 3 — re-dialed right after the release
       and 'd' never stuck. */
    avdtp_disconnect_keep_pairing();
}

bool a2dp_source_console_key(char cmd){
    uint8_t status = ERROR_CODE_SUCCESS;
    is_cmd_triggered_localy = 1;
    static bool enter_remote_seid_index = false;


    if (enter_remote_seid_index){
        if ((cmd < '0') || (cmd > '9')) return true;
        uint8_t index = cmd - '0';
        if (index >= num_remote_seps){
            printf("Index too high, try again\n");
            return true;
        }
        enter_remote_seid_index = false;
        selected_remote_sep_index = index;
        media_tracker.remote_seid = remote_seps[selected_remote_sep_index].sep.seid;
        printf("Selected Remote Stream Endpoint with SEID %u\n",  media_tracker.remote_seid);
        return true;
    }

    switch (cmd){
        /* ---- setup toggles (same fields the web UI writes) ---------------- */
        case 'J':
            // AirPods jitter buffer via AACP 0x0B: 150 -> 60 -> 30 ms
            aap_cycle_jitter();
            break;
        case 'K':   /* kick: manual suspend->rebuild->restart (mute recovery) */
            printf("manual stream refresh (k)\n");
            a2dp_source_apply_eld_rate();
            break;
        case 'U':
            {   /* PCM tap: 500 ms summaries of what the USB host delivers */
                extern void usb_pcm_tap_toggle(void);
                usb_pcm_tap_toggle();
            }
            break;
        case 'R':
            {   /* boot-time reset report is lost to the CDC reconnect window */
                extern uint32_t last_fault_pc, last_fault_lr, last_fault_cfsr;
                extern uint8_t  last_reset_kind;
                if (last_reset_kind == 2) {
                    printf("[reset] HARD FAULT: PC=0x%08lx LR=0x%08lx CORE %lu CFSR=0x%07lx (addr2line -e build-universal/USBPods_Pico2W.elf)\n",
                           (unsigned long)last_fault_pc, (unsigned long)last_fault_lr,
                           (unsigned long)(last_fault_cfsr >> 28),
                           (unsigned long)(last_fault_cfsr & 0x0FFFFFFFu));
                } else if (last_reset_kind == 3) {
                    const char *pm = (const char *)(uintptr_t)last_fault_lr;
                    if (last_fault_lr < 0x10000000u || last_fault_lr >= 0x10400000u)
                        pm = "(fmt not in flash)";
                    printf("[reset] PANIC from 0x%08lx arg0=0x%lx: \"%s\"\n",
                           (unsigned long)last_fault_pc, (unsigned long)last_fault_cfsr, pm);
                } else if (last_reset_kind == 1) {
                    extern uint8_t last_hang_phase, last_hang_irq;
                    printf("[reset] WATCHDOG (loop starved >2s — phase %u [1 usb 2 settings "
                           "3 relay-decode 4 eld-rebuild 5 tail], irq 0x%02x [10 usb-task "
                           "20 bootsel 30 pump, xF=exited])\n",
                           last_hang_phase, last_hang_irq);
                } else {
                    printf("[reset] clean boot\n");
                }
            }
            bt_sink_relay_dump();
            settings_debug_dump();   /* [settings] seq/sector/page/rc — flash health */
            {   /* license: state + the 22-byte binding id (feed to sign.sh) */
                extern uint8_t license_state(void); extern uint8_t license_key_id(void);
                extern uint32_t license_features(void);
                extern void license_device_id(uint8_t out[22]);
                uint8_t lid[22]; license_device_id(lid);
                printf("[lic] state %u key %u features 0x%08lx id ",
                       license_state(), license_key_id(),
                       (unsigned long)license_features());
                for (int li = 0; li < 22; li++) printf("%02x", lid[li]);
                printf("\n");
            }
            printf("[src] ts_snaps %lu (RTP holes stamped; %lu live-splices seamless, tx_malformed %lu)\n",
           (unsigned long)src_ts_snaps, (unsigned long)src_live_splices,
           (unsigned long)src_tx_malformed);
    printf("[src] autoflush %lu, eflush %lu, cflush %lu, flushable_tx %lu (flushable_tx 0 while streaming = media CID not registered; cflush>0 = autoflush data contaminated by fallbacks)\n",
           (unsigned long)usbpods_flush_occurred_count,
           (unsigned long)usbpods_enhanced_flush_done_count,
           (unsigned long)src_flush_fallbacks,
           (unsigned long)usbpods_flushable_tx_count);
    {   uint16_t rch = a2dp_source_con_handle();
        if (rch != 0) {
            printf("[link] role %s — rssi/lq/fcc/flush-readback follow async\n",
                   gap_get_role(rch) == HCI_ROLE_MASTER ? "central" : "PERIPHERAL");
            src_link_probe_step = 4;
        }
    }
    printf("[src] pnudges %lu (preventive per-splice resets)\n",
           (unsigned long)src_pnudges);
    printf("[src] send_fails %lu (last rc %u) vol_cmds %lu nudges %lu auto_kicks %lu backoff %u parkstarts %lu%s\n",
           (unsigned long)src_send_fails, src_send_fail_rc,
           (unsigned long)src_vol_cmds, (unsigned long)src_auto_nudges,
           (unsigned long)src_auto_kicks, src_rec_escal,
           (unsigned long)src_park_starts,
           src_auto_kick_enable ? "" : " [AUTO-RECOVERY OFF]");
    if (src_tx_malformed) {
        printf("[mal] len %u nfr %u walk_off %u walk_n %u seqctr %u hex ",
               src_mal_len, src_mal_nfr, src_mal_walkoff, src_mal_walkn,
               src_mal_seqctr);
        uint16_t mn = (src_mal_len < sizeof(src_mal_snap)) ? src_mal_len
                                                           : sizeof(src_mal_snap);
        for (uint16_t mi = 0; mi < mn; mi++) printf("%02x", src_mal_snap[mi]);
        printf("\n");
    }
            printf("[src] tx_pkts %lu tx_stall_drops %lu usb_slot_drops %lu scrubs %lu real_waits %lu max_tick_gap %lums (codec %d, bitrate %s)\n",
                   (unsigned long)src_tx_pkts,
                   (unsigned long)src_tx_stall_drops,
                   (unsigned long)usb_slot_push_drops,
                   (unsigned long)usb_slot_scrubs,
                   (unsigned long)slot_real_waits,
                   (unsigned long)src_max_tick_gap_ms,
                   cur_codec, src_relay_rate_low ? "relay-low" : "full");
            src_max_tick_gap_ms = 0;   /* per-window peak */
            {   /* pool invariant: free+ready+filling must equal the pool size.
           MORE = a duplicated index (two writers on one slot: persistent
           garble); FEWER = a leaked slot. Both raced here historically. */
        int inv = queue_get_level(&ready_queue) + queue_get_level(&free_queue)
                + (filling_slot_valid ? 1 : 0);
        if (inv != active_slot_count)
            printf("[slots] INVARIANT BROKEN: %d of %u accounted\n",
                   inv, active_slot_count);
    }
    printf("[slots] ready %u free %u of %u, filling %d@%u, ready_full_drops %lu, usb_streaming %d, streaming %d\n",
                   queue_get_level(&ready_queue), queue_get_level(&free_queue),
                   active_slot_count, filling_slot_valid ? 1 : 0,
                   (unsigned)filling_offset,
                   (unsigned long)usb_slot_ready_full_drops,
                   is_usb_streaming ? 1 : 0, is_streaming ? 1 : 0);
            {   extern uint16_t usb_stop_delay;
                printf("[usbbuf] credit %lu samp, stop_delay %u, epclose_flushes %lu (%lu slots dropped), prime %d\n",
                       (unsigned long)media_tracker.samples_ready, usb_stop_delay,
                       (unsigned long)usb_ep_flushes,
                       (unsigned long)usb_ep_flushed_slots,
                       src_prime_pending ? 1 : 0);
            }
            {   extern void qmi_regs_dump(void);
                qmi_regs_dump();
            }
            printf("[usb] rx_gaps %lu max %lums (missed ISO frames = silent skips)\n",
                   (unsigned long)usb_rx_gap_events,
                   (unsigned long)usb_rx_max_gap_ms);
            usb_rx_gap_events = 0;
            usb_rx_max_gap_ms = 0;
            break;
        case 'B':   /* raw Bytes */
            {   /* continuous raw host-PCM dump (hex, decodable); b again stops */
                extern void usb_pcm_dump_toggle(void);
                usb_pcm_dump_toggle();
            }
            break;
        case 'G':   /* experimental AACP 0x09/0x03 value 0 (decoder reset?) */
            aap_send_09_03(0);
            break;
        case 'I':   /* experimental AACP 0x09/0x03 value 1 (decoder reset?) */
            aap_send_09_03(1);
            break;
        case 'X': { /* auto-kick enable toggle — hold off during experiments */
            src_auto_kick_enable = !src_auto_kick_enable;
            printf("[auto] splice-churn auto-recovery %s\n",
                   src_auto_kick_enable ? "ENABLED" : "disabled (experiment mode)");
            break;
        }
        case 'Q':   /* 0x4c status probe: replay macOS's AACP metrics poll */
            aap_probe_4c();
            break;
        case 'F': {  /* fake one EP splice (see usb_splice_sim in uac.c) —
                        THE decisive flush-vs-splice experiment per
                        macos-link-recovery-report.md section 7 */
            extern volatile uint8_t usb_splice_sim;
            usb_splice_sim = 50;
            printf("[sim] splice armed\n");
            break;
        }
        case 'A': {  /* raw AACP frame logging (buds' decode-distress hunt) */
            extern volatile bool aap_rx_log;
            aap_rx_log = !aap_rx_log;
            printf("[aap] raw frame log %s\n", aap_rx_log ? "ON" : "off");
            break;
        }
        case 'E':   /* ELD frame tap: one line per TX packet (stability work) */
            src_eld_tap = !src_eld_tap;
            printf("[eld] frame tap %s (#pkt, t wall-ms, seq first-AU, n AUs, "
                   "len bytes, ts RTP, gap ms since prev send, SNAP = RTP hole stamped)\n",
                   src_eld_tap ? "ON" : "off");
            break;
        case 'N':
            bt_sink_relay_toggle_aac();   /* SBC-relay test: hide the AAC sink SEP */
            break;
        case 'T': {   /* cycle HEADSET link max TX power (coex experiments) */
            static const int8_t tp[] = { 8, 4, 0, -4, -8 };
            static uint8_t ti;
            bt_link_tx_power(a2dp_source_con_handle(), tp[ti]);
            ti = (uint8_t)((ti + 1) % 5);
            break;
        }
        case 'W': {   /* cycle PHONE (relay) link max TX power */
            static const int8_t tpw[] = { 8, 4, 0, -4, -8 };
            static uint8_t twi;
            bt_link_tx_power(bt_sink_relay_con_handle(), tpw[twi]);
            twi = (uint8_t)((twi + 1) % 5);
            break;
        }
        case 'g':
            printf("set_next_capablity_and_start_stream with seid %d\n", media_tracker.remote_seid);
            set_next_capablity_and_start_stream();
            break;
        case 'a':
            printf("Get all capabilities of stream endpoint with seid %d\n", media_tracker.remote_seid);
            status = avdtp_source_get_all_capabilities(media_tracker.avdtp_cid, media_tracker.remote_seid);
            break;
        case 'f':
            printf("Get configuration of stream endpoint with seid %d\n", media_tracker.remote_seid);
            status = avdtp_source_get_configuration(media_tracker.avdtp_cid, media_tracker.remote_seid);
            break;
        case 'i':
            if (num_remote_seps == 0){
                printf("Remote Stream Endpoints not discovered yet, please discover stream endpoints first\n");
                break;
            }
            dump_remote_sink_endpoints();
            printf("Please enter index of remote stream endpoint:\n");
            enter_remote_seid_index = true;
            break;

        case 's':{
            //status = set_configuration();
            break;
        }
        case 'n':{
            if (num_remote_seps == 0){
                printf("Remote Stream Endpoints not discovered yet, please discover stream endpoints first\n");
                break;
            }
            if (remote_seps[selected_remote_sep_index].have_media_codec_apabilities == false){
                printf("Remote Stream Endpoints Media Codec Capabilities not received yet, please get (all) capabilities for stream endpoint with seid %u first\n",  media_tracker.remote_seid);
                break;
            }
            printf("Reconfigure stream endpoint with seid %d\n", media_tracker.remote_seid);
            avdtp_media_codec_type_t media_codec_type = remote_seps[selected_remote_sep_index].sep.capabilities.media_codec.media_codec_type;
            avdtp_capabilities_t new_configuration;
            new_configuration.media_codec.media_type = AVDTP_AUDIO;

            uint16_t new_sampling_frequency = 44100;
            switch (current_sample_rate){
                case 44100:
                    new_sampling_frequency = 48000;
                    break;
                default:
                    break;
            }

            switch (media_codec_type){
                case AVDTP_CODEC_SBC:
                    avdtp_config_sbc_set_sampling_frequency(media_codec_config_data, new_sampling_frequency);
                    break;
                case AVDTP_CODEC_MPEG_1_2_AUDIO:
                    avdtp_config_mpeg_audio_set_sampling_frequency(media_codec_config_data, new_sampling_frequency);
                    break;
                case AVDTP_CODEC_MPEG_2_4_AAC:
                    avdtp_config_mpeg_aac_set_sampling_frequency(media_codec_config_data, new_sampling_frequency);
                    break;
                case AVDTP_CODEC_ATRAC_FAMILY:
                    avdtp_config_atrac_set_sampling_frequency(media_codec_config_data, new_sampling_frequency);
                    break;
                default:
                    printf("Reconfigure not implemented for %s\n", codec_name_for_type(media_codec_type));
                    return true;   /* key consumed, nothing to reconfigure */
            }
            new_configuration.media_codec.media_type = AVDTP_AUDIO;
            new_configuration.media_codec.media_codec_type = media_codec_type;
            new_configuration.media_codec.media_codec_information_len = media_codec_config_len;
            new_configuration.media_codec.media_codec_information = media_codec_config_data;
            status = avdtp_source_reconfigure(media_tracker.avdtp_cid, media_tracker.local_seid, media_tracker.remote_seid, 1 << AVDTP_MEDIA_CODEC, new_configuration);
            break;
        }
        case 'o':
            printf("Establish stream between local %d and remote %d seid\n", media_tracker.local_seid, media_tracker.remote_seid);
            status = avdtp_source_open_stream(media_tracker.avdtp_cid, media_tracker.local_seid, media_tracker.remote_seid);
            break;
        case 'm':
            printf("Start stream between local %d and remote %d seid, \n", media_tracker.local_seid, media_tracker.remote_seid);
            status = avdtp_source_start_stream(media_tracker.avdtp_cid, media_tracker.local_seid);
            break;
        case 'x':
            printf("Abort stream between local %d and remote %d seid\n", media_tracker.local_seid, media_tracker.remote_seid);
            status = avdtp_source_abort_stream(media_tracker.avdtp_cid, media_tracker.local_seid);
            break;
        case 't':
            printf("Release stream between local %d and remote %d seid\n", media_tracker.local_seid, media_tracker.remote_seid);
            status = avdtp_source_stop_stream(media_tracker.avdtp_cid, media_tracker.local_seid);
            break;
        case 'P':
            printf("Susspend stream between local %d and remote %d seid\n", media_tracker.local_seid, media_tracker.remote_seid);
            status = avdtp_source_suspend(media_tracker.avdtp_cid, media_tracker.local_seid);
            break;


        case 'M':
            a2dp_demo_send_media_packet();
            break;



        /* ---- media transport: HID consumer to the HOST — the same path
           the buds' own tap gestures take (see the AAP handler). */
        case 'V':   /* Verbose: the 1 Hz [diag] link telemetry */
            src_diag_log = !src_diag_log;
            printf("[diag] 1 Hz telemetry %s\n", src_diag_log ? "ON" : "off");
            break;
        case 'C':   /* Capability scan + raw codec-config dumps */
            src_proto_log = !src_proto_log;
            printf("[avdtp] capability-scan log %s\n", src_proto_log ? "ON" : "off");
            break;
        default:
            return false;   /* not ours — console.c owns the user tier */

    }
    if (status != ERROR_CODE_SUCCESS){
        printf("command '%c' failed, status 0x%02x\n", cmd, status);
    }
    return true;
}
#endif


static int setup_sbc_configuration(){ return codec_sbc_ops.configure(); }


static int setup_aac_configuration(){ return codec_aac_ops.configure(); }



static int set_ldac_configuration(){ return codec_ldac_ops.configure(); }


static int setup_aaceld_configuration(){ return codec_aaceld_ops.configure(); }


/* Plain user disconnect: drop the link, keep the pairing. The web
   Disconnect button used to route to avdtp_disconnect_and_scan(), which
   WIPES the slot + link key and hunts for a new speaker — so the headset's
   next inbound connect no longer matched a slot and the sink relay adopted
   it as a PHONE ("I did not ask to scan at all"). */
void avdtp_disconnect_keep_pairing(){
    src_user_dc_hold = true;  /* user asked: nothing may redial on its own */
    src_dial_window_ms = 0;   /* USER disconnect: no stale-session redial */
    src_dial_tries = 0;
    btstack_run_loop_remove_timer(&src_dial_retry_timer);
    a2dp_demo_timer_stop(&media_tracker);
    a2dp_source_disconnect(media_tracker.avdtp_cid);
    avrcp_disconnect(media_tracker.avdtp_cid);
    aap_disconnect();   /* AAP rides its own L2CAP channel — the AVDTP release
                           does not take it down, and a lingering channel holds
                           the ACL so the old AirPods redial mid-next-session
                           and the UI keeps their menu (report 7 sees channel
                           open + the new headset's connected flag) */
    audio_slot_queue_init();
    led_idle_unless_pairing();
}

/* Slot switch WHILE CONNECTED (console '1'/'2'): user-style teardown of the
   current link (keep pairing, no stale-session redial), then ONE dial of the
   newly selected slot as soon as the release completes. The button/web paths
   still refuse while connected (allow_switch_slot); this is the console's
   richer flow. Disconnected callers dial directly instead. */
void a2dp_source_switch_slot(uint8_t want){
    a2dp_source_wedge_clear();   /* the user chose this slot: full budget */
    write_uint8_last_flash(want);
    get_link_keys();   /* reload the dial target: reconnect dials
                          get_device_addr(), which only get_link_keys()
                          refreshes from the slot mac — without this the
                          handover redialed the headset it just dropped */
    if (!a2dp_is_connected_flag){ a2dp_source_reconnect(); return; }
    src_slot_redial_pending = true;
    avdtp_disconnect_keep_pairing();
}

void avdtp_disconnect_and_scan(){
    src_dial_window_ms = 0;   /* USER disconnect: no stale-session redial */
    src_dial_tries = 0;
    btstack_run_loop_remove_timer(&src_dial_retry_timer);
    a2dp_demo_timer_stop(&media_tracker);
    a2dp_source_disconnect(media_tracker.avdtp_cid);
    avrcp_disconnect(media_tracker.avdtp_cid);
    aap_disconnect();   /* see avdtp_disconnect_keep_pairing */
    audio_slot_queue_init(); // reset all slots on disconnect
    gap_drop_link_key_for_bd_addr((uint8_t *) get_device_addr());
    uint8_t currect_slot = read_uint8_last_flash();

    printf("currect slot iss %d\n", currect_slot);

    uint8_t new_addr[6] = {0x0};

    if(currect_slot == 0x1){
        write_slot1_mac(new_addr);
    }

    if(currect_slot == 0x2){
        write_slot2_mac(new_addr);
    }
    //gap_delete_all_link_keys();
    gap_start_scanning();
}

/* Post-reboot stale-session retry: after an UNCLEAN dongle reboot the
   AirPods still hold the dead ACL and reject/ignore the first page(s) —
   "signaling connection failed: status 8" until they notice the old link
   died. Retry the dial with a 2 s spacing, 4 tries, instead of making the
   user mash Connect. Armed per dial, disarmed on success/teardown. */
static void src_dial_retry_handler(btstack_timer_source_t *ts) {
    (void)ts;
    if (a2dp_is_connected_flag) { src_dial_tries = 0; return; }
    if (src_dial_tries == 0 || src_dial_total >= SRC_DIAL_MAX) {
        led_set_mode(LED_IDLE);   /* headset not found — give up, LED solid */
        return;
    }
    printf("connect retry %u/%u\n", (unsigned)(src_dial_total + 1u), (unsigned)SRC_DIAL_MAX);
    src_dial_tries++;
    src_dial_total++;
    avdtp_source_connect((uint8_t *) get_device_addr(), &media_tracker.avdtp_cid);
    /* 4 s spacing: the controller's page attempt runs ~5 s; the old 2 s
       overlapped it and spammed crossed pages. */
    btstack_run_loop_set_timer(&src_dial_retry_timer, 4000);
    btstack_run_loop_add_timer(&src_dial_retry_timer);
}

static void src_dial_retry_arm(void) {
    src_dial_tries = 1;
    src_dial_total = 1;
    /* NOTE: the wedge counter is deliberately NOT reset here. This helper is
       also reached from uac.c's unattended connmode-3 refill every 10 s -
       the very path the wedge brake exists to stop - so resetting here would
       refund the brake forever. Genuine user intent clears it instead, via
       a2dp_source_wedge_clear() at the connect entry points. */
    src_dial_window_ms = btstack_run_loop_get_time_ms();
    btstack_run_loop_remove_timer(&src_dial_retry_timer);
    btstack_run_loop_set_timer_handler(&src_dial_retry_timer, src_dial_retry_handler);
    btstack_run_loop_set_timer(&src_dial_retry_timer, 4000);
    btstack_run_loop_add_timer(&src_dial_retry_timer);
}

/* Second act of the stale-session dance (HW log): the first ACCEPTED link
   after an unclean reboot gets terminated by the AirPods mid-discovery
   (reason 0x13) — they use it to flush their dead AVDTP state, and the
   NEXT dial sticks. Called from the release/teardown paths: redial once
   more if we're inside the 20 s recovery window. */
static void src_dial_maybe_redial_after_drop(void) {
    if (src_user_dc_hold) return;        /* user said stop — honor it */
    /* A pairing scan drops the link on purpose and deliberately does NOT set
       src_user_dc_hold (avdtp_disconnect_and_scan), so without this every 'p'
       charges the wedge counter and a fourth one latches the brake mid-scan.
       The two sibling brakes already check this (a2dp_source_remote_bye, and
       the HFP teardown-blame call site). */
    if (bt_pairing_active()) return;

    /* A session that reached AVDTP signaling and never got ONE command
       accepted is not a transient drop - the peer is answering L2CAP and
       nothing above it (half-open session on its side). Redialing that only
       re-chimes the headset forever, because with the ACL still up no
       disconnect event will ever reach a2dp_source_remote_bye(). Give up and
       drop the ACL: that is the one action that clears the peer's stale
       session, and the log line tells the user what happened. */
    /* Stale the run: only a BURST of failures is a wedge, so four unrelated
       non-streaming sessions spread over hours must not latch. Measured
       between FAILURES only - stamping on every teardown (successes too)
       would let a slow wedge cycle reset the run before each increment and
       the brake would never latch at all. */
    static uint32_t src_nostream_last_ms;
    if (!src_session_streamed){
        uint32_t nnow = btstack_run_loop_get_time_ms();
        if (src_nostream_last_ms && nnow - src_nostream_last_ms > 60000)
            src_nostream_run = 0;
        src_nostream_last_ms = nnow ? nnow : 1;
    }
    if (!src_session_streamed && ++src_nostream_run >= SRC_WEDGE_MAX){
        src_nostream_run   = 0;
        src_wedge_hold     = true;       /* own flag: see the note at its decl */
        src_dial_tries     = 0;
        src_dial_window_ms = 0;
        btstack_run_loop_remove_timer(&src_dial_retry_timer);
        led_idle_unless_pairing();   /* never stomp a live pairing flash */
        printf("headset connects but never starts audio (%u tries) — dropping the "
               "link and stopping (press c to retry, or power the headset off and "
               "on)\n", (unsigned)SRC_WEDGE_MAX);
        /* src_con_handle DIRECTLY, not a2dp_source_con_handle(): the accessor
           is gated on a2dp_is_connected_flag, which the released handler
           clears a few lines before calling us - so the accessor returns 0
           here and the disconnect would silently never happen. The handle
           itself is still valid: the whole point of this brake is a peer that
           kept the ACL up. */
        if (src_con_handle) gap_disconnect(src_con_handle);
        return;
    }

    if (src_dial_window_ms == 0) return;
    if (btstack_run_loop_get_time_ms() - src_dial_window_ms >= 20000) return;
    if (src_dial_total >= 8) return;
    printf("link dropped during setup — redialing in 1.5 s (stale-session cleanup)\n");
    src_dial_tries = 1;
    btstack_run_loop_remove_timer(&src_dial_retry_timer);
    btstack_run_loop_set_timer_handler(&src_dial_retry_timer, src_dial_retry_handler);
    btstack_run_loop_set_timer(&src_dial_retry_timer, 1500);
    btstack_run_loop_add_timer(&src_dial_retry_timer);
}

/* Orderly BT shutdown before a soft reboot: HCI power-off detaches every
   ACL properly, so the AirPods KNOW we left — an unclean watchdog reboot
   leaves them holding a stale session that rejects our next pages
   ("signaling connection failed: status 8" until they time it out). */
void bt_prepare_reboot(void){
    hci_power_control(HCI_POWER_OFF);
}

void a2dp_source_reconnect(){
    /* every dial entry point (button, web Connect, boot auto-connect) */
    src_user_dc_hold = false;            /* explicit dial: drop the 'd' hold */
    const uint8_t *a = (const uint8_t *) get_device_addr();
    bool ff = true, zero = true;
    for (int i = 0; i < 6; i++){ ff &= (a[i] == 0xFF); zero &= (a[i] == 0); }
    if (ff || zero){
        printf("connect: slot %u is empty — nothing to dial\n", read_uint8_last_flash());
        led_set_mode(LED_IDLE);
        return;
    }
    led_set_mode(read_uint8_last_flash() == 0x2 ? LED_CONN_SLOT2 : LED_CONN_SLOT1);
    avdtp_source_connect((uint8_t *) a, &media_tracker.avdtp_cid);
    src_dial_retry_arm();
    sleep_ms(200);
}


void avdtp_source_establish_stream(){
    avdtp_source_connect((uint8_t *) get_device_addr(), &media_tracker.avdtp_cid);
    src_dial_retry_arm();
}


int set_next_codec(uint8_t num){
    switch (num){
        case 0:
            /* Walk THE registry in priority order (codec_registry.c):
               AAC-ELD > LHDC > LDAC > AAC > SBC. Two gates per entry: the
               user's enable toggle (settings codec_dis — applies here, i.e.
               at the next connection) and the capability scan's available().
               This replaced a hardcoded ELD>LDAC>AAC chain that predated the
               registry — and silently never offered LHDC at all. SBC is the
               registry tail and intentionally unskippable. */
            for (int i = 0; codec_registry[i]; i++){
                const codec_ops_t *ops = codec_registry[i];
                if (ops->id == CODEC_ID_SBC) break;   /* baseline: below */
                if (!codec_enabled(ops->id)) {
                    printf("codec select: %s disabled by user — skipping\n", ops->name);
                    continue;
                }
                if (src_codec_reject_mask & (uint8_t)(1u << ops->id)) {
                    printf("codec select: %s already rejected by this sink — skipping\n", ops->name);
                    continue;
                }
                if (ops->available()) {
                    src_attempt_codec = ops->id;
                    return ops->configure();
                }
            }
            printf("codec select: falling back to SBC\n");
            src_attempt_codec = CODEC_ID_SBC;
            return setup_sbc_configuration();
        case 1: // sbc
            return setup_sbc_configuration();
        default:
            return 1;
    }
}


void set_next_capablity_and_start_stream(){

    /* Reconnect race (the OLD reboot bug): a reconnecting headset (AirPods
       after an ACL drop) CONFIGURES US while our capability scan is still
       running — the stream is already established when the scan completes.
       Every open attempt then returns COMMAND_DISALLOWED (12) and the loop
       below cycled 12,12,1,12... forever inside the BT context: print
       storm, starved main loop, watchdog reboot.
       Discriminator: src_stream_established (MEDIA channel) — NOT
       a2dp_is_connected_flag, which raises at SIGNALING established and
       broke every normal connect ("stream not start"). */
    if (src_stream_established || is_streaming || src_codec_configured){
        printf("codec select: stream already established (remote configured) — keeping it\n");
        /* The stream is configured and (about to be) started — the START
           accept only starts the PUMP when this flag is set. Without it the
           kept stream ran pumpless: no keep-alive audio -> AirPods idle-
           suspend -> our auto-restart -> suspend... an infinite pause/start
           ping-pong (HW log, dozens of cycles). */
        finish_scan_avdtp_codec = true;
        return;
    }

    avdtp_source_stop_stream(media_tracker.avdtp_cid, media_tracker.local_seid);

    int result = set_next_codec(cur_capability);

    /* Bounded: at most two full passes over the capability list. An offer
       set that rejects everything must NOT hang the radio context. */
    int attempts = 0;
    while (result != 0){
        if (result == 1){
            cur_capability = 0;
        }else{
            cur_capability++;
        }
        if (++attempts > 16){
            printf("codec select: no endpoint accepted after %d attempts — giving up\n", attempts);
            return;
        }
        result = set_next_codec(cur_capability);
        printf("result is %d\n", result);
    }

    cur_capability++;
    finish_scan_avdtp_codec = true;
}


void start_led_blink(){
    led_set_mode(LED_PLAYING);   /* one slow flash for every codec */
}

static bool _bt_sink_volume_changed = false;

// getter returns a pointer to that storage
bool * get_is_bt_sink_volume_changed_ptr(void){
    return &_bt_sink_volume_changed;
}

static void avrcp_packet_handler_inner(uint8_t packet_type, uint16_t channel, uint8_t *packet, uint16_t size);
static void avrcp_packet_handler(uint8_t packet_type, uint16_t channel, uint8_t *packet, uint16_t size){
    watchdog_hw->scratch[7] = 0x42;   /* entered (hang forensics) */
    avrcp_packet_handler_inner(packet_type, channel, packet, size);
    watchdog_hw->scratch[7] = 0x4F;   /* exited cleanly */
}
static void avrcp_packet_handler_inner(uint8_t packet_type, uint16_t channel, uint8_t *packet, uint16_t size){
    UNUSED(channel);
    UNUSED(size);
    bd_addr_t event_addr;
    uint16_t local_cid;
    uint8_t  status = ERROR_CODE_SUCCESS;

    if (packet_type != HCI_EVENT_PACKET) return;
    if (hci_event_packet_get_type(packet) != HCI_EVENT_AVRCP_META) return;
    
    switch (packet[2]){
        case AVRCP_SUBEVENT_CONNECTION_ESTABLISHED: 
            local_cid = avrcp_subevent_connection_established_get_avrcp_cid(packet);
            status = avrcp_subevent_connection_established_get_status(packet);
            if (status != ERROR_CODE_SUCCESS){
                printf("AVRCP: Connection failed, local cid 0x%02x, status 0x%02x\n", local_cid, status);
                return;
            }
            avrcp_subevent_connection_established_get_bd_addr(packet, event_addr);
            /* Phones open AVRCP toward the relay sink — never adopt their
               channel (ESP32 lesson: adopted phone AVRCP hijacks volume
               state and falsely releases gates). Just leave it unmanaged. */
            if (bt_sink_relay_is_phone_addr(event_addr)){
                printf("AVRCP: from phone %s — ignored (sink relay)\n", bd_addr_to_str(event_addr));
                return;
            }
            media_tracker.avrcp_cid = local_cid;

            printf("AVRCP: Channel to %s successfully opened, avrcp_cid 0x%02x\n", bd_addr_to_str(event_addr), media_tracker.avrcp_cid);
             
            avrcp_target_support_event(media_tracker.avrcp_cid, AVRCP_NOTIFICATION_EVENT_PLAYBACK_STATUS_CHANGED);
            avrcp_target_support_event(media_tracker.avrcp_cid, AVRCP_NOTIFICATION_EVENT_TRACK_CHANGED);
            avrcp_target_support_event(media_tracker.avrcp_cid, AVRCP_NOTIFICATION_EVENT_NOW_PLAYING_CONTENT_CHANGED);
            avrcp_target_set_now_playing_info(media_tracker.avrcp_cid, NULL, sizeof(tracks)/sizeof(avrcp_track_t));
            /* Seed the QUERY answer only (no notification): play_info starts
               zeroed and STOPPED == 0 — some headsets mute their speakers on
               "never played". Status-change notifications stay silent, see
               the note at set_usb_streaming. */
            play_info.status = AVRCP_PLAYBACK_STATUS_PLAYING;

            printf("Enable Volume Change notification\n");
            avrcp_controller_enable_notification(media_tracker.avrcp_cid, AVRCP_NOTIFICATION_EVENT_VOLUME_CHANGED);
            // Set initial volume to 50% on first connection
            if (media_tracker.volume == 0 || media_tracker.volume == 127) {
                media_tracker.volume = 64;  // ~50% (0-127 range)
                avrcp_controller_set_absolute_volume(media_tracker.avrcp_cid, media_tracker.volume);
            }
            printf("Enable Battery Status Change notification\n");
            avrcp_controller_enable_notification(media_tracker.avrcp_cid, AVRCP_NOTIFICATION_EVENT_BATT_STATUS_CHANGED);
            // AVRCP is up — NOW the AirPods control channel may open (stem
            // presses -> HID keys, ear-detect pause, head-tracking stop).
            // Sequencing it after AVRCP avoids the L2CAP connect race that
            // killed AVRCP when both channels raced on the fresh ACL.
            // Non-Apple sinks refuse the PSM — harmless.
            aap_connect(cur_active_device);
            return;
        
        case AVRCP_SUBEVENT_CONNECTION_RELEASED:
            if (avrcp_subevent_connection_released_get_avrcp_cid(packet) != media_tracker.avrcp_cid) return;
            printf("AVRCP Target: Disconnected, avrcp_cid 0x%02x\n", avrcp_subevent_connection_released_get_avrcp_cid(packet));
            media_tracker.avrcp_cid = 0;
            s_avrcp_batt = -1;
            return;
        default:
            break;
    }

    if (status != ERROR_CODE_SUCCESS){
        printf("Responding to event 0x%02x failed, status 0x%02x\n", packet[2], status);
    }
}


bool is_muted = false;

bool get_bt_mute(){
    return is_muted;
}


static void avrcp_target_packet_handler_inner(uint8_t packet_type, uint16_t channel, uint8_t *packet, uint16_t size);
static void avrcp_target_packet_handler(uint8_t packet_type, uint16_t channel, uint8_t *packet, uint16_t size){
    watchdog_hw->scratch[7] = 0x44;   /* entered (hang forensics) */
    avrcp_target_packet_handler_inner(packet_type, channel, packet, size);
    watchdog_hw->scratch[7] = 0x4F;   /* exited cleanly */
}
static void avrcp_target_packet_handler_inner(uint8_t packet_type, uint16_t channel, uint8_t *packet, uint16_t size){
    UNUSED(channel);
    UNUSED(size);
    uint8_t  status = ERROR_CODE_SUCCESS;

    if (packet_type != HCI_EVENT_PACKET) return;
    if (hci_event_packet_get_type(packet) != HCI_EVENT_AVRCP_META) return;
    /* Only the headset's AVRCP channel may drive play/pause/volume — the
       phone's unadopted relay channel also lands here (cid leads every
       AVRCP subevent). */
    if (media_tracker.avrcp_cid == 0 ||
        little_endian_read_16(packet, 3) != media_tracker.avrcp_cid) return;

    bool button_pressed;
    char const * button_state;
    avrcp_operation_id_t operation_id;

    switch (packet[2]){
        case AVRCP_SUBEVENT_PLAY_STATUS_QUERY:
            status = avrcp_target_play_status(media_tracker.avrcp_cid, play_info.song_length_ms, play_info.song_position_ms, play_info.status);            
            break;
        // case AVRCP_SUBEVENT_NOW_PLAYING_INFO_QUERY:
        //     status = avrcp_target_now_playing_info(avrcp_cid);
        //     break;
        case AVRCP_SUBEVENT_OPERATION:
            operation_id = avrcp_subevent_operation_get_operation_id(packet);
            button_pressed = avrcp_subevent_operation_get_button_pressed(packet) > 0;
            button_state = button_pressed ? "PRESS" : "RELEASE";

            printf("AVRCP Target: operation %s (%s)\n", avrcp_operation2str(operation_id), button_state);

            if (!button_pressed){
                break;
            }
            switch (operation_id) {
                /* PLAY and PAUSE are ONE DUMB TOGGLE — see the note at
                   set_usb_streaming for why nothing smarter survives contact
                   with real headsets. Every press forwards exactly one 0xCD
                   to the computer (discrete 0xB0/0xB1 are widely ignored;
                   macOS did nothing); which label the headset picked is its
                   private toggle state and none of our business. The old
                   volume-0 mute on PAUSE stays gone; volume is restored on
                   every press so buds left at 0 by an older build heal. */
                case AVRCP_OPERATION_ID_PLAY:
                case AVRCP_OPERATION_ID_PAUSE: {
                    /* debounce only: one host toggle per 250 ms — a human
                       cannot press faster; a burst-happy headset cannot flap
                       the computer's playback */
                    uint32_t now_ms = btstack_run_loop_get_time_ms();
                    if (src_avrcp_op_ms && now_ms - src_avrcp_op_ms < 250) break;
                    src_avrcp_op_ms = now_ms;

                    is_muted = false;
                    _bt_sink_volume_changed = true;
                    avrcp_controller_set_absolute_volume(media_tracker.avrcp_cid, media_tracker.volume);
                    usb_hid_consumer(0x00CD);   /* one press = one host toggle */
                    break;
                }
                case AVRCP_OPERATION_ID_STOP:
                    status = avdtp_source_stop_stream(media_tracker.avdtp_cid, media_tracker.local_seid);
                    usb_hid_consumer(0x00B7);   /* HID Stop */
                    break;
                case AVRCP_OPERATION_ID_FORWARD:
                    usb_hid_consumer(0x00B5);   /* HID Scan Next Track */
                    break;
                case AVRCP_OPERATION_ID_BACKWARD:
                    usb_hid_consumer(0x00B6);   /* HID Scan Previous Track */
                    break;
                default:
                    break;
            }
            break;
        default:
            break;
    }

    if (status != ERROR_CODE_SUCCESS){
        printf("Responding to event 0x%02x failed, status 0x%02x\n", packet[2], status);
    }
}

uint8_t get_bt_volume(){
    return media_tracker.volume;
}

static void avrcp_controller_packet_handler_inner(uint8_t packet_type, uint16_t channel, uint8_t *packet, uint16_t size);
static void avrcp_controller_packet_handler(uint8_t packet_type, uint16_t channel, uint8_t *packet, uint16_t size){
    watchdog_hw->scratch[7] = 0x46;   /* entered (hang forensics) */
    avrcp_controller_packet_handler_inner(packet_type, channel, packet, size);
    watchdog_hw->scratch[7] = 0x4F;   /* exited cleanly */
}
static void avrcp_controller_packet_handler_inner(uint8_t packet_type, uint16_t channel, uint8_t *packet, uint16_t size){
    UNUSED(channel);
    UNUSED(size);
    
    if (packet_type != HCI_EVENT_PACKET) return;
    if (hci_event_packet_get_type(packet) != HCI_EVENT_AVRCP_META) return;
    if (!media_tracker.avrcp_cid) return;
    /* Ignore the phone's (unadopted) AVRCP channel — headset volume only. */
    if (little_endian_read_16(packet, 3) != media_tracker.avrcp_cid) return;

    switch (packet[2]){
        case AVRCP_SUBEVENT_NOTIFICATION_VOLUME_CHANGED:
            media_tracker.volume = avrcp_subevent_notification_volume_changed_get_absolute_volume(packet);
            
            _bt_sink_volume_changed = true;
            printf("AVRCP Controller: Notification Absolute Volume %d %%\n", media_tracker.volume * 100 / 127);\

            break;
        case AVRCP_SUBEVENT_NOTIFICATION_EVENT_BATT_STATUS_CHANGED:
            s_avrcp_batt = (int8_t)avrcp_subevent_notification_event_batt_status_changed_get_battery_status(packet);
            // see avrcp_battery_status_t
            printf("AVRCP Controller: Notification Battery Status 0x%02x\n", avrcp_subevent_notification_event_batt_status_changed_get_battery_status(packet));
            break;
        case AVRCP_SUBEVENT_NOTIFICATION_STATE:
            printf("AVRCP Controller: Notification %s - %s\n", 
                avrcp_event2str(avrcp_subevent_notification_state_get_event_id(packet)), 
                avrcp_subevent_notification_state_get_enabled(packet) != 0 ? "enabled" : "disabled");
            break;
        default:
            break;
    }  
}

int btstack_main(int argc, const char * argv[]){

    l2cap_init();
    hfp_battery_init();   /* AG for headset battery % — SLC only, no SCO */
    bt_hci_init();

    // Initialize AVDTP Sink
    avdtp_source_init();
    avdtp_source_register_packet_handler(&packet_handler);

    // BT sink relay: phone -> Pico -> headset (sink SEP + SDP + decoder)
    bt_sink_relay_init();

    // Initialize AVRCP Service
    avrcp_init();
    avrcp_register_packet_handler(&avrcp_packet_handler);
    // Initialize AVRCP Target
    avrcp_target_init();
    avrcp_target_register_packet_handler(&avrcp_target_packet_handler);

    // Initialize AVRCP Controller
    avrcp_controller_init();
    avrcp_controller_register_packet_handler(&avrcp_controller_packet_handler);


    // Initialize SDP
    sdp_init();
    memset(sdp_avdtp_source_service_buffer, 0, sizeof(sdp_avdtp_source_service_buffer));
    a2dp_source_create_sdp_record(sdp_avdtp_source_service_buffer, 0x10002, AVDTP_SOURCE_FEATURE_MASK_PLAYER, NULL, NULL);
    sdp_register_service(sdp_avdtp_source_service_buffer);



    // Create AVRCP Target service record and register it with SDP. We receive Category 1 commands from the headphone, e.g. play/pause
    memset(sdp_avrcp_target_service_buffer, 0, sizeof(sdp_avrcp_target_service_buffer));
    uint16_t supported_features = AVRCP_FEATURE_MASK_CATEGORY_PLAYER_OR_RECORDER;
#ifdef AVRCP_BROWSING_ENABLED
    supported_features |= AVRCP_FEATURE_MASK_BROWSING;
#endif
    /* 0x10001: was 0x10002, colliding with the A2DP source record above —
       sdp_register_service returned SDP_HANDLE_ALREADY_REGISTERED (unchecked)
       and the TARGET record was never actually published. Volume worked
       anyway because AVRCP rides the already-open channel; only peers doing
       an SDP lookup for our TG capability ever saw the gap. */
    avrcp_target_create_sdp_record(sdp_avrcp_target_service_buffer, 0x10001, supported_features, NULL, NULL);
    sdp_register_service(sdp_avrcp_target_service_buffer);

    // Create AVRCP Controller service record and register it with SDP. We send Category 2 commands to the headphone, e.g. volume up/down
    memset(sdp_avrcp_controller_service_buffer, 0, sizeof(sdp_avrcp_controller_service_buffer));
    uint16_t controller_supported_features = AVRCP_FEATURE_MASK_CATEGORY_MONITOR_OR_AMPLIFIER;
    avrcp_controller_create_sdp_record(sdp_avrcp_controller_service_buffer, 0x10003, controller_supported_features, NULL, NULL);
    sdp_register_service(sdp_avrcp_controller_service_buffer);

    hfp_battery_register_sdp();   /* 0x10005 — record count now 5 of 6 */


    console_init();   /* key handler + persisted debug mode (console.c) */

    a2dp_source_selfheal_init();   /* 1 s suspended-stream watchdog */
    a2dp_source_parkwatch_init();  /* parked-session watchdog (2 s)  */

    /* macOS-style link watchdog (wire-captured: macos-link-recovery-report.md):
       200 ms HCI automatic flush + auto-flushable ACL marking. On a degraded
       link the CONTROLLER discards stale media instead of retransmitting it
       late — the buds see concealable GAPS, never the splice-shaped burst of
       stale AUs that desyncs their ELD decoder. BTstack plumbs everything
       (per-connection HCI write) off this one call; 200 -> 0x0140 slots,
       byte-identical to macOS. PB marking is PER-CID via the SDK l2cap
       patch: only the media channel registered at STREAM_ESTABLISHED goes
       out 0b10 auto-flushable, control stays 0b00 — TRUE macOS parity, no
       deviation (sdk-patches/README.md). */
    /* Re-enabled after exoneration: the status-102 refusals blamed on this
       were actually the stale-link-key bug (slot-clear never dropped the
       TLV key) — the disable test was confounded. Retrial with clean keys. */
    gap_enable_link_watchdog(200);
    /* macOS also writes Link Policy on every AirPods connection (wire
       capture): allow role switch + sniff so the buds can manage the link
       the way they do against Apple hosts instead of bailing on stress. */
    gap_set_default_link_policy_settings(LM_LINK_POLICY_ENABLE_ROLE_SWITCH |
                                         LM_LINK_POLICY_ENABLE_SNIFF_MODE);

    /* Volume system: apply persisted gains (percent+1 encoding, 0 = unset).
       The software USB gain only engages in swvol mode. */
    if (settings()->usbset & USBSET_SWVOL) {
        uint8_t p = settings()->usb_vol_p1;
        set_usb_sw_gain_pct(p ? (uint8_t)(p - 1) : 100);
    }
    if (settings()->relay_vol_p1) {
        bt_sink_relay_set_vol_pct((uint8_t)(settings()->relay_vol_p1 - 1));
    }

    // turn on!
    hci_power_control(HCI_POWER_ON);

    return 0;

}
