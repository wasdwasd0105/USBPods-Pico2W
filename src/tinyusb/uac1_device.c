/* UAC1 (USB Audio Class 1.0) app class driver — speaker-only, TinyUSB 0.18.
 *
 * Ported from the usbpods ESP32 implementation. TinyUSB's built-in audio
 * driver is UAC 2.0-only (audiod_open rejects bInterfaceProtocol != 0x20), so
 * the UAC1 personality's audio interfaces are claimed by THIS driver,
 * registered via usbd_app_driver_get_cb() (app drivers are tried before
 * built-ins). It speaks the UAC1 control set — SET_CUR / GET_CUR / GET_MIN /
 * GET_MAX / GET_RES on the feature unit + the endpoint sampling-frequency
 * control — and lands on the SAME state the UAC2 path uses (volume[] / mute[]
 * / need_change_bt_volume in uac.c, audio_slot_push_samples toward BTstack),
 * so the rest of the firmware can't tell which class version is live.
 *
 * TinyUSB 0.18 deltas vs the ESP32 (0.19) original:
 *  - usbd_class_driver_t has NO xfer_isr: completions arrive in tud_task
 *    context. tud_task is pumped from a 500 us repeating timer (main.c), so
 *    the re-arm always lands inside the current 1 ms full-speed frame.
 *  - usbd_edpt_xfer is not ISR-safe here (pico mutex), so the SOF watchdog
 *    defers its re-arm through usbd_defer_func instead of arming directly.
 *  - usbd_edpt_close is a no-op for ISO on the rp2040 port; alt 0 just stops
 *    re-arming.
 */

#include <string.h>
#include <stdio.h>

#include "tusb.h"
#include "device/usbd_pvt.h"

#include "usb_descriptors.h"
#include "uac1_descriptors.h"
#include "../btstack/btstack_avdtp_source.h"

/* Shared UAC state in uac.c — the UAC1 handlers write the same variables the
   UAC2 callbacks do, so audio_control_task's BT-volume sync is unchanged. */
extern int8_t   mute[];
extern int16_t  volume[];
extern bool     need_change_bt_volume;
extern bool     is_usb_audio_running;
extern uint16_t usb_stop_delay;
void usb_rx_gap_probe(void);   /* uac.c — missed-ISO detector */

/* UAC1 class-specific request codes (audio10.pdf 5.2). */
#define UAC1_SET_CUR  0x01
#define UAC1_GET_CUR  0x81
#define UAC1_GET_MIN  0x82
#define UAC1_GET_MAX  0x83
#define UAC1_GET_RES  0x84

#define UAC1_FU_MUTE          0x01
#define UAC1_FU_VOLUME        0x02
#define UAC1_EP_SAMPLING_FREQ 0x01

/* Same range/steps as the UAC2 build: -50..0 dB, 1 dB resolution. */
#define UAC1_VOL_MIN_256DB  (-50 * 256)
#define UAC1_VOL_MAX_256DB  0
#define UAC1_VOL_RES_256DB  256

typedef struct {
    uint8_t rhport;
    uint8_t alt_spk;
    CFG_TUSB_MEM_SECTION CFG_TUSB_MEM_ALIGN uint8_t spk_buf[UAC1_EP_OUT_SZ];
    CFG_TUSB_MEM_SECTION CFG_TUSB_MEM_ALIGN uint8_t ctrl_buf[8];
} uac1_t;

static uac1_t s_u1;
static tusb_desc_endpoint_t s_ep_out_desc;

/* True while the host has selected the UAC1 configuration (our open() ran).
   Cleared on bus reset. Lets shared code (uac.c) know which class is live. */
static bool s_uac1_active;
bool usb_uac1_active(void) { return s_uac1_active; }

static void uac1_init(void) { memset(&s_u1, 0, sizeof(s_u1)); }
static bool uac1_deinit(void) { return true; }

static void uac1_reset(uint8_t rhport)
{
    (void)rhport;
    s_u1.alt_spk = 0;
    s_uac1_active = false;
}

/* Claim the whole UAC1 audio function: the STD AC interface plus every
   following audio-class descriptor, up to the first non-audio interface. */
static uint16_t uac1_open(uint8_t rhport, tusb_desc_interface_t const *itf, uint16_t max_len)
{
    TU_VERIFY(TUSB_CLASS_AUDIO == itf->bInterfaceClass &&
              AUDIO_SUBCLASS_CONTROL == itf->bInterfaceSubClass &&
              0x00 == itf->bInterfaceProtocol, 0);   /* UAC1 only */

    uint8_t const *p   = (uint8_t const *)itf;
    uint8_t const *end = p + max_len;
    uint16_t drv_len = 0;

    while (p < end) {
        uint8_t const dtype = tu_desc_type(p);
        if (dtype == TUSB_DESC_INTERFACE && drv_len > 0) {
            tusb_desc_interface_t const *i2 = (tusb_desc_interface_t const *)p;
            if (i2->bInterfaceClass != TUSB_CLASS_AUDIO) break;   /* HID */
        } else if (dtype == TUSB_DESC_ENDPOINT) {
            tusb_desc_endpoint_t const *ep = (tusb_desc_endpoint_t const *)p;
            s_ep_out_desc = *ep;
            /* rp2040 dcd_edpt_open rejects ISO — must pre-alloc here. */
            TU_ASSERT(usbd_edpt_iso_alloc(rhport, ep->bEndpointAddress,
                                          tu_edpt_packet_size(ep)), 0);
        }
        drv_len += tu_desc_len(p);
        p = tu_desc_next(p);
    }

    s_u1.rhport = rhport;
    s_uac1_active = true;
    printf("UAC1 function claimed (%u B) — UAC1 configuration active\n", drv_len);
    return drv_len;
}

static void spk_arm(void)
{
    if (s_u1.alt_spk && !usbd_edpt_busy(s_u1.rhport, s_ep_out_desc.bEndpointAddress)) {
        usbd_edpt_xfer(s_u1.rhport, s_ep_out_desc.bEndpointAddress,
                       s_u1.spk_buf, UAC1_EP_OUT_SZ);
    }
}

/* SOF watchdog helper — runs in tud_task context via usbd_defer_func. */
static void spk_arm_deferred(void *param)
{
    (void)param;
    spk_arm();
}

static bool set_interface(uint8_t rhport, tusb_control_request_t const *req)
{
    uint8_t const itf = (uint8_t)tu_le16toh(req->wIndex);
    uint8_t const alt = (uint8_t)tu_le16toh(req->wValue);

    if (itf == UAC1_ITF_SPK) {
        if (alt == 1 && s_u1.alt_spk != 1) {
            TU_VERIFY(usbd_edpt_iso_activate(rhport, &s_ep_out_desc));
            s_u1.alt_spk = 1;
            {   extern void audio_slot_grace_cancel(void);
                audio_slot_grace_cancel();
            }
            spk_arm();
        } else if (alt == 0) {
            s_u1.alt_spk = 0;   /* stop re-arming; ISO close is a no-op here */
            {   /* definitive host stop — FULL parity with the UAC2 EP-close
                   handler: pause AND drop everything staged. Without the
                   flush this was the "pause without clearing" variant — on
                   a hard test (rapid per-sound closes) staged PCM + pacing
                   credit accumulated into AirPods desync -> no sound. */
                extern void set_usb_streaming(bool flag);
                extern uint16_t usb_stop_delay;
                extern void audio_slot_request_flush(bool live);
                extern void audio_slot_live_close_grace(void);
                extern void usb_pcm_tap_session_reset(void);
                audio_slot_request_flush(usb_stop_delay <= 60);
                usb_stop_delay = 101;
                set_usb_streaming(false);
                usb_pcm_tap_session_reset();
            }
        }
        printf("UAC1 speaker alt %u\n", alt);
        /* SOF watchdog only while streaming. */
        usbd_sof_enable(rhport, SOF_CONSUMER_AUDIO, s_u1.alt_spk == 1);
    } else if (itf != UAC1_ITF_AC) {
        return false;
    }
    return tud_control_status(rhport, req);
}

/* Feature-unit requests (recipient = AC interface, entity in wIndex high). */
static bool fu_request(uint8_t rhport, tusb_control_request_t const *req, uint8_t stage)
{
    uint8_t const entity = TU_U16_HIGH(req->wIndex);
    uint8_t const sel    = TU_U16_HIGH(req->wValue);
    uint8_t const ch     = TU_U16_LOW(req->wValue);

    if (!(entity == UAC1_ENT_SPK_FU && (ch <= 2 || ch == 0xFF))) {
        printf("FU REJECT entity=%u ch=%u\n", entity, ch);
        return false;
    }

    if (req->bRequest == UAC1_SET_CUR) {
        if (stage == CONTROL_STAGE_SETUP) {
            return tud_control_xfer(rhport, req, s_u1.ctrl_buf, req->wLength);
        }
        if (stage == CONTROL_STAGE_DATA) {
            /* Echo suppression: nudge-driven slider moves write back here —
               only genuine (user) writes may push to BT. */
            extern bool usb_vol_nudge_active(void);
            bool const genuine = !usb_vol_nudge_active();
            if (sel == UAC1_FU_MUTE && req->wLength >= 1) {
                mute[ch <= 2 ? ch : 0] = (int8_t)s_u1.ctrl_buf[0];
                if (genuine) need_change_bt_volume = true;
            } else if (sel == UAC1_FU_VOLUME && req->wLength >= 2) {
                int16_t v = (int16_t)tu_le16toh(*(uint16_t *)s_u1.ctrl_buf);
                /* Per-channel layout: L/R carry the volume; volume[0] mirrors
                   ch1 so the shared BT-push path (audio_control_task reads
                   volume[0]) stays unchanged. 0xFF = all channels. */
                if (ch == 0xFF) {
                    volume[0] = volume[1] = volume[2] = v;
                } else {
                    volume[ch] = v;
                    if (ch <= 1) volume[0] = v;
                }
                printf("FU SET vol ch=%u %d dB%s\n", ch, v / 256,
                       genuine ? "" : " (nudge echo)");
                if (genuine) need_change_bt_volume = true;
            } else {
                return false;
            }
        }
        return true;
    }

    if (stage != CONTROL_STAGE_SETUP) return true;
    int16_t v16 = 0;
    uint16_t len = 2;
    switch (req->bRequest) {
    case UAC1_GET_CUR:
        if (sel == UAC1_FU_MUTE)        { s_u1.ctrl_buf[0] = (uint8_t)mute[ch <= 2 ? ch : 0]; len = 1; }
        else if (sel == UAC1_FU_VOLUME) v16 = volume[ch <= 2 ? ch : 1];
        else return false;
        break;
    case UAC1_GET_MIN: TU_VERIFY(sel == UAC1_FU_VOLUME); v16 = UAC1_VOL_MIN_256DB; break;
    case UAC1_GET_MAX: TU_VERIFY(sel == UAC1_FU_VOLUME); v16 = UAC1_VOL_MAX_256DB; break;
    case UAC1_GET_RES: TU_VERIFY(sel == UAC1_FU_VOLUME); v16 = UAC1_VOL_RES_256DB; break;
    default: return false;
    }
    if (len == 2) {
        s_u1.ctrl_buf[0] = (uint8_t)(v16 & 0xFF);
        s_u1.ctrl_buf[1] = (uint8_t)((v16 >> 8) & 0xFF);
    }
    return tud_control_xfer(rhport, req, s_u1.ctrl_buf, len);
}

/* Endpoint sampling-frequency control (3-byte value). 48 kHz only. */
static bool ep_request(uint8_t rhport, tusb_control_request_t const *req, uint8_t stage)
{
    TU_VERIFY(TU_U16_HIGH(req->wValue) == UAC1_EP_SAMPLING_FREQ);

    if (req->bRequest == UAC1_SET_CUR) {
        if (stage == CONTROL_STAGE_SETUP) {
            return tud_control_xfer(rhport, req, s_u1.ctrl_buf, req->wLength);
        }
        return true;   /* single advertised rate — accept and ignore */
    }
    if (req->bRequest == UAC1_GET_CUR) {
        if (stage != CONTROL_STAGE_SETUP) return true;
        extern uint32_t sample_rates[];
        uint32_t r = sample_rates[0];
        s_u1.ctrl_buf[0] = r & 0xFF;
        s_u1.ctrl_buf[1] = (r >> 8) & 0xFF;
        s_u1.ctrl_buf[2] = (r >> 16) & 0xFF;
        return tud_control_xfer(rhport, req, s_u1.ctrl_buf, 3);
    }
    return false;
}

static bool uac1_control_xfer_cb(uint8_t rhport, uint8_t stage, tusb_control_request_t const *req)
{
    if (req->bmRequestType_bit.type == TUSB_REQ_TYPE_STANDARD &&
        req->bmRequestType_bit.recipient == TUSB_REQ_RCPT_INTERFACE) {
        if (stage != CONTROL_STAGE_SETUP) return true;
        if (req->bRequest == TUSB_REQ_SET_INTERFACE) {
            return set_interface(rhport, req);
        }
        if (req->bRequest == TUSB_REQ_GET_INTERFACE) {
            uint8_t const itf = (uint8_t)tu_le16toh(req->wIndex);
            s_u1.ctrl_buf[0] = (itf == UAC1_ITF_SPK) ? s_u1.alt_spk : 0;
            return tud_control_xfer(rhport, req, s_u1.ctrl_buf, 1);
        }
        return false;
    }

    if (req->bmRequestType_bit.type != TUSB_REQ_TYPE_CLASS) return false;
    if (req->bmRequestType_bit.recipient == TUSB_REQ_RCPT_INTERFACE) {
        return fu_request(rhport, req, stage);
    }
    if (req->bmRequestType_bit.recipient == TUSB_REQ_RCPT_ENDPOINT) {
        return ep_request(rhport, req, stage);
    }
    return false;
}

static bool uac1_xfer_cb(uint8_t rhport, uint8_t ep_addr, xfer_result_t result, uint32_t xferred)
{
    (void)rhport;
    (void)result;

    if (ep_addr == s_ep_out_desc.bEndpointAddress) {
        /* Deliver to the BT pipeline (same sinks as the UAC2 rx callback),
           then immediately re-arm. Guard 4-byte frame alignment. */
        if (xferred > 0 && (xferred & 0x3) == 0) {
            {   /* 'U' console key: PCM tap (shared with the UAC2 path) */
                extern volatile bool usb_pcm_tap;
                extern void usb_pcm_tap_feed_u1(const int16_t *pcm, uint16_t bytes);
                if (usb_pcm_tap) usb_pcm_tap_feed_u1((const int16_t *)s_u1.spk_buf, (uint16_t)xferred);
            }
            {   /* 'p' console key: raw PCM dump (shared with the UAC2 path) */
                extern void usb_pcm_dump_feed_u1(const void *pcm, uint16_t bytes);
                usb_pcm_dump_feed_u1(s_u1.spk_buf, (uint16_t)xferred);
            }
            usb_rx_gap_probe();
            audio_slot_push_samples((int16_t *)s_u1.spk_buf, (uint16_t)(xferred / 4));
            set_usb_streaming(true);
            is_usb_audio_running = true;
            usb_stop_delay = 0;
        }
        spk_arm();
    }
    return true;
}

/* Per-frame watchdog (ISR context): a failed/missed arm has no completion to
   chain from — without this the stream starves permanently. usbd_edpt_xfer is
   not ISR-safe in 0.18, so punt to tud_task via usbd_defer_func. */
static void uac1_sof(uint8_t rhport, uint32_t frame_count)
{
    (void)frame_count;
    if (s_u1.alt_spk &&
        !usbd_edpt_busy(rhport, s_ep_out_desc.bEndpointAddress)) {
        usbd_defer_func(spk_arm_deferred, NULL, true);
    }
}

static usbd_class_driver_t const s_uac1_driver = {
    .name            = "uac1",
    .init            = uac1_init,
    .deinit          = uac1_deinit,
    .reset           = uac1_reset,
    .open            = uac1_open,
    .control_xfer_cb = uac1_control_xfer_cb,
    .xfer_cb         = uac1_xfer_cb,
    .sof             = uac1_sof,
};

usbd_class_driver_t const *usbd_app_driver_get_cb(uint8_t *driver_count)
{
    *driver_count = 1;
    return &s_uac1_driver;
}
