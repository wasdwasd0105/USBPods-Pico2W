/* UAC1 (USB Audio Class 1.0) personality — descriptors. Speaker-only + HID.
 *
 * Enabled by USB_UAC1_MODE=1 (tusb_config.h): served INSTEAD of the UAC2 set
 * for maximum legacy-host compatibility (game consoles, KVMs, older OSes).
 * TinyUSB's built-in audio driver is UAC 2.0-only, so these interfaces
 * (bInterfaceProtocol 0x00) are claimed by the app class driver in
 * uac1_device.c. Ported from the usbpods ESP32 implementation, mic removed.
 */
#pragma once

/* Interface layout: AC=0, speaker AS=1, HID=2. */
enum {
    UAC1_ITF_AC       = 0,
    UAC1_ITF_SPK      = 1,
    UAC1_ITF_HID      = 2,
    UAC1_ITF_CDC      = 3,
    UAC1_ITF_CDC_DATA = 4,
#ifndef USBPODS_LITE
    UAC1_ITF_VENDOR   = 5,
    UAC1_ITF_TOTAL    = 6,
#else
    UAC1_ITF_TOTAL    = 5,
#endif
};

/* Entity IDs (reuse the UAC2 build's numbering). */
#define UAC1_ENT_SPK_IT  0x01   /* USB streaming input terminal */
#define UAC1_ENT_SPK_FU  0x02   /* feature unit (mute + volume) */
#define UAC1_ENT_SPK_OT  0x03   /* headphones output terminal   */

/* Full-speed 48 kHz/16-bit/stereo: 48 samples x 4 B per 1 ms frame. */
#define UAC1_EP_OUT_SZ   192
#define UAC1_RATE_48_3B  0x80, 0xBB, 0x00   /* 48000, 3-byte LE */
#define UAC1_RATE_44_3B  0x44, 0xAC, 0x00   /* 44100, 3-byte LE */

/* Lengths: CS-AC = hdr(9) + IT(12) + FU(10) + OT(9) = 40;
   AS = alt0(9) + alt1(9) + general(7) + format(11) + EP(9) + CS-EP(7) = 52;
   whole function incl. IAD(8) + std AC(9) = 109. */
#define UAC1_AC_CS_LEN   (9 + 12 + 10 + 9)
#define UAC1_AS_LEN      (9 + 9 + 7 + 11 + 9 + 7)
#define UAC1_FUNC_LEN    (8 + 9 + UAC1_AC_CS_LEN + UAC1_AS_LEN)

#define UAC1_FUNC_DESCRIPTOR(_stridx, _epout, _rate3b...) \
    /* IAD: groups AC + AS so usbd binds both interfaces to the app driver */ \
    8, TUSB_DESC_INTERFACE_ASSOCIATION, UAC1_ITF_AC, 2, TUSB_CLASS_AUDIO, 0x00, 0x00, 0x00, \
    /* Standard AC interface (UAC1: protocol 0x00, no class EPs) */ \
    9, TUSB_DESC_INTERFACE, UAC1_ITF_AC, 0, 0, TUSB_CLASS_AUDIO, AUDIO_SUBCLASS_CONTROL, 0x00, (_stridx), \
    /* CS AC header: bcdADC 1.00, 1 streaming interface in the collection */ \
    9, TUSB_DESC_CS_INTERFACE, 0x01, U16_TO_U8S_LE(0x0100), U16_TO_U8S_LE(UAC1_AC_CS_LEN), 1, UAC1_ITF_SPK, \
    /* IT: USB streaming, stereo */ \
    12, TUSB_DESC_CS_INTERFACE, 0x02, UAC1_ENT_SPK_IT, U16_TO_U8S_LE(0x0101), 0x00, 2, U16_TO_U8S_LE(0x0003), 0x00, 0x00, \
    /* FU: classic Win7-era stereo shape — master MUTE + per-channel VOLUME
       (bmaControls: master 0x01, ch1 0x02, ch2 0x02). Master-only volume
       made Win7's usbaudio skip the volume node entirely (zero FU traffic
       at bind): the slider fell back to host software attenuation. */ \
    10, TUSB_DESC_CS_INTERFACE, 0x06, UAC1_ENT_SPK_FU, UAC1_ENT_SPK_IT, 1, 0x01, 0x02, 0x02, 0x00, \
    /* OT: headphones */ \
    9, TUSB_DESC_CS_INTERFACE, 0x03, UAC1_ENT_SPK_OT, U16_TO_U8S_LE(0x0302), 0x00, UAC1_ENT_SPK_FU, 0x00, \
    /* AS alt 0 (zero bandwidth) / alt 1 (streaming) */ \
    9, TUSB_DESC_INTERFACE, UAC1_ITF_SPK, 0, 0, TUSB_CLASS_AUDIO, AUDIO_SUBCLASS_STREAMING, 0x00, (_stridx), \
    9, TUSB_DESC_INTERFACE, UAC1_ITF_SPK, 1, 1, TUSB_CLASS_AUDIO, AUDIO_SUBCLASS_STREAMING, 0x00, (_stridx), \
    /* CS AS general: link to IT, 1 frame delay, PCM */ \
    7, TUSB_DESC_CS_INTERFACE, 0x01, UAC1_ENT_SPK_IT, 1, U16_TO_U8S_LE(0x0001), \
    /* Type I format: stereo, 2 B/sample, 16-bit, 1 rate */ \
    11, TUSB_DESC_CS_INTERFACE, 0x02, 1, 2, 2, 16, 1, _rate3b, \
    /* ISO OUT data EP: adaptive, 192 B, 1 ms (9-byte UAC1 EP descriptor) */ \
    9, TUSB_DESC_ENDPOINT, (_epout), 0x09, U16_TO_U8S_LE(UAC1_EP_OUT_SZ), 1, 0, 0, \
    /* CS ISO EP: sampling-freq control supported, lock delay 1 ms */ \
    7, TUSB_DESC_CS_ENDPOINT, 0x01, 0x01, 1, U16_TO_U8S_LE(1)

/* App class driver entry points (uac1_device.c). */
void usb_hid_consumer(uint16_t usage);
