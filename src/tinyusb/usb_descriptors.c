/*
 * The MIT License (MIT)
 *
 * Copyright (c) 2020 Ha Thach (tinyusb.org)
 * Copyright (c) 2020 Jerzy Kasenberg
 *
 * Permission is hereby granted, free of charge, to any person obtaining a copy
 * of this software and associated documentation files (the "Software"), to deal
 * in the Software without restriction, including without limitation the rights
 * to use, copy, modify, merge, publish, distribute, sublicense, and/or sell
 * copies of the Software, and to permit persons to whom the Software is
 * furnished to do so, subject to the following conditions:
 *
 * The above copyright notice and this permission notice shall be included in
 * all copies or substantial portions of the Software.
 *
 * THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
 * IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
 * FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE
 * AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
 * LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
 * OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN
 * THE SOFTWARE.
 *
 */

#include "bsp/board_api.h"
#include "tusb.h"
#include "usb_descriptors.h"
#include "uac1_descriptors.h"
#include "../settings.h"   /* bt_mac cache for the product-name suffix */

extern uint32_t sample_rates[];   /* uac.c — pipeline rate set at boot */

/* TEMP (2026-07-10): 1 = expose ONLY the UAC1 configuration (no UAC2 config,
   no auto-fallback) — isolates Win7 debugging. Restore to 0 afterward. */
#define USB_TEMP_UAC1_ONLY 0   /* runtime USB-settings bit picks the order now */

/* A combination of interfaces must have a unique product id, since PC will save device driver after the first plug.
 * Same VID/PID with different interface e.g MSC (first), then CDC (later) will possibly cause system error on PC.
 *
 * Auto ProductID layout's Bitmap:
 *   [MSB]     AUDIO | MIDI | HID | MSC | CDC          [LSB]
 */
#define _PID_MAP(itf, n)  ( (CFG_TUD_##itf) << (n) )
/* Base 0x5000 (was 0x4000): the "USBPods" rename needs a FRESH Windows
   identity — endpoint friendly names freeze at first enumeration per
   VID/PID/serial, so the 0x4000-era devices keep showing "3- TinyUSB BT"
   no matter what strings we send. New PID = clean slate everywhere.
   (Chrome WebHID filter is vendorId+usagePage, so the page still finds
   the device — just re-pick it once in the permission prompt.) */
/* Lite (open-source) gets its OWN identity: base 0x6000, product string
   "USBPods Lite" — Windows must never mix cached descriptor shapes between
   the variants, and the web UI tells them apart by PID/name BEFORE opening
   the device (the vendor HID collection it would probe does not exist). */
#ifdef USBPODS_LITE
#define USB_PID_BASE          0x6000
#define USBPODS_PRODUCT_BASE  "USBPods Lite"
#else
#define USB_PID_BASE          0x5000
#define USBPODS_PRODUCT_BASE  "USBPods"
#endif
#define USB_PID           (USB_PID_BASE | _PID_MAP(CDC, 0) | _PID_MAP(MSC, 1) | _PID_MAP(HID, 2) | \
    _PID_MAP(MIDI, 3) | _PID_MAP(AUDIO, 4) | _PID_MAP(VENDOR, 5) )

//--------------------------------------------------------------------+
// Device Descriptors
//--------------------------------------------------------------------+
tusb_desc_device_t const desc_device =
{
    .bLength            = sizeof(tusb_desc_device_t),
    .bDescriptorType    = TUSB_DESC_DEVICE,
    .bcdUSB             = 0x0200,

    // Use Interface Association Descriptor (IAD) for Audio
    // As required by USB Specs IAD's subclass must be common class (2) and protocol must be IAD (1)
    .bDeviceClass       = TUSB_CLASS_MISC,
    .bDeviceSubClass    = MISC_SUBCLASS_COMMON,
    .bDeviceProtocol    = MISC_PROTOCOL_IAD,
    .bMaxPacketSize0    = CFG_TUD_ENDPOINT0_SIZE,

    .idVendor           = 0xCafe,
    .idProduct          = USB_PID,      /* adjusted per config ORDER in the cb */
    .bcdDevice          = 0x0403,   /* 0x03xx = dual-config era; bumps force
                                       hosts to re-read cached strings:
                                       +1 USBPods rename, +2 MAC suffix */

    .iManufacturer      = 0x01,
    .iProduct           = 0x02,
    .iSerialNumber      = 0x03,

    /* ONE configuration. The dual-config design (UAC2 + UAC1, host picks)
       is GONE: no tested host ever used config 2, and Windows would open
       the dual-config UAC2 stream then never send ISO data. The USB
       settings bit picks the single personality served each boot. */
    .bNumConfigurations = 0x01
};

/* Which personality (UAC2 or UAC1) this boot serves — from the USB settings
   bit (web switch "Use legacy UAC 1"); flipping it reboots + re-enumerates. */
static bool s_uac1_first = (USB_UAC1_FIRST != 0) || USB_TEMP_UAC1_ONLY;

void usb_desc_set_uac1_first(bool on) { s_uac1_first = on; }
bool usb_desc_uac1_first(void)        { return s_uac1_first; }

/* USB Connect Mode 2 ("USB audio follows headset"): while no headset is
   connected the device presents a HID+CDC-only personality, so the computer
   has no dead audio output to route to — the WebHID page and serial console
   stay alive. Flipped at runtime by usb_mode_task() with a soft replug. */
static bool s_audio_exposed = true;

void usb_desc_set_audio_exposed(bool on) { s_audio_exposed = on; }
bool usb_desc_audio_exposed(void)        { return s_audio_exposed; }

// Invoked when received GET DEVICE DESCRIPTOR
// Application return pointer to descriptor
uint8_t const * tud_descriptor_device_cb(void)
{
  /* Own PID + revision per PERSONALITY: Windows caches the enumerated
     configuration by VID/PID/REV, so every distinct descriptor shape needs
     its own identity. bcdDevice base bumped 0x03xx -> 0x04xx: the single-
     config reshape must not hit the cached dual-config descriptors. */
  static tusb_desc_device_t d;
  d = desc_device;
  if (s_uac1_first) {
    d.idProduct = USB_PID + 0x100;
    d.bcdDevice += 2;
  }
  if (sample_rates[0] == 44100) {
    d.bcdDevice += 0x10;    /* 44.1 personalities get their own cache slots */
  }
  if (!s_audio_exposed) {
    /* Same PID — Chrome's WebHID permission is keyed on VID/PID/serial and
       must survive the follows-headset replug. */
    d.bcdDevice += 0x40;
  }
  return (uint8_t const *)&d;
}

//--------------------------------------------------------------------+
// Configuration Descriptor
//--------------------------------------------------------------------+
/* A vendor-class interface with ZERO endpoints — the WebUSB config channel.
   All traffic is EP0 control (tud_vendor_control_xfer_cb), so no endpoints are
   consumed. Present in every configuration so WebUSB works whichever one is
   active. */
#ifndef USBPODS_LITE
#define TUD_VENDOR_NOEP_DESC_LEN  (9)
#define TUD_VENDOR_NOEP_DESCRIPTOR(_itfnum, _stridx) \
  9, TUSB_DESC_INTERFACE, _itfnum, 0, 0, TUSB_CLASS_VENDOR_SPECIFIC, 0x00, 0x00, _stridx
#else
#define TUD_VENDOR_NOEP_DESC_LEN  (0)   /* keeps the TOTAL_LEN sums honest */
#endif

#define CONFIG_UAC2_TOTAL_LEN 	(TUD_CONFIG_DESC_LEN + TUD_AUDIO_HEADSET_STEREO_DESC_LEN + TUD_HID_DESC_LEN + TUD_CDC_DESC_LEN + TUD_VENDOR_NOEP_DESC_LEN)
#define CONFIG_UAC1_TOTAL_LEN 	(TUD_CONFIG_DESC_LEN + UAC1_FUNC_LEN + TUD_HID_DESC_LEN + TUD_CDC_DESC_LEN + TUD_VENDOR_NOEP_DESC_LEN)

/* Arrays are built with placeholder config number 1; the configuration cb
   patches bConfigurationValue = index+1 at serve time (order is runtime). */
#define CFGNUM_UAC1  1
#define CFGNUM_UAC2  1

#if CFG_TUSB_MCU == OPT_MCU_LPC175X_6X || CFG_TUSB_MCU == OPT_MCU_LPC177X_8X || CFG_TUSB_MCU == OPT_MCU_LPC40XX
  // LPC 17xx and 40xx endpoint type (bulk/interrupt/iso) are fixed by its number
  // 0 control, 1 In, 2 Bulk, 3 Iso, 4 In etc ...
  #define EPNUM_AUDIO_IN    0x03
  #define EPNUM_AUDIO_OUT   0x03
  #define EPNUM_AUDIO_INT   0x01

#elif CFG_TUSB_MCU == OPT_MCU_CXD56
  // CXD56 USB driver has fixed endpoint type (bulk/interrupt/iso) and direction (IN/OUT) by its number
  // 0 control (IN/OUT), 1 Bulk (IN), 2 Bulk (OUT), 3 In (IN), 4 Bulk (IN), 5 Bulk (OUT), 6 In (IN)
  // #define EPNUM_AUDIO_IN    0x01
  // #define EPNUM_AUDIO_OUT   0x02
  // #define EPNUM_AUDIO_INT   0x03

#elif CFG_TUSB_MCU == OPT_MCU_NRF5X
  // ISO endpoints for NRF5x are fixed to 0x08 (0x88)
  #define EPNUM_AUDIO_IN    0x08
  #define EPNUM_AUDIO_OUT   0x08
  #define EPNUM_AUDIO_INT   0x01

#elif defined(TUD_ENDPOINT_ONE_DIRECTION_ONLY)
  // MCUs that don't support a same endpoint number with different direction IN and OUT defined in tusb_mcu.h
  //    e.g EP1 OUT & EP1 IN cannot exist together
  #define EPNUM_AUDIO_IN    0x01
  #define EPNUM_AUDIO_OUT   0x02
  #define EPNUM_AUDIO_INT   0x03

#else
  #define EPNUM_AUDIO_IN    0x01
  #define EPNUM_AUDIO_OUT   0x01
  #define EPNUM_AUDIO_INT   0x02
#endif

#define EPNUM_HID           0x83   /* interrupt IN for consumer-control keys */
#define EPNUM_CDC_NOTIF     0x85   /* CDC notification (interrupt IN)         */
#define EPNUM_CDC_OUT       0x04   /* CDC data OUT (bulk)                     */
#define EPNUM_CDC_IN        0x84   /* CDC data IN (bulk)                      */

/* HID: two top-level collections on one interface.
   1) Consumer control (report ID 1): media keys, as before.
   2) Vendor page 0xFF00 (feature reports 2/3/4): the WebHID config channel —
      Chrome pages can exchange feature reports through the OS HID driver, no
      drivers, no WiFi (the DS5Dongle pattern). A vendor page is never on the
      WebHID blocklist, and Windows splits each collection into its own HID
      handle so the media keys stay untouched. */
uint8_t const desc_hid_report[] = {
    TUD_HID_REPORT_DESC_CONSUMER(HID_REPORT_ID(1)),

#ifndef USBPODS_LITE   /* the whole vendor config channel is a Full feature */
    HID_USAGE_PAGE_N(0xFF00, 2),
    HID_USAGE(0x01),
    HID_COLLECTION(HID_COLLECTION_APPLICATION),
        HID_LOGICAL_MIN(0),
        HID_LOGICAL_MAX_N(255, 2),
        HID_REPORT_SIZE(8),

        HID_REPORT_ID(2)                       /* command (SET_FEATURE, 8 B)  */
        HID_USAGE(0x02),
        HID_REPORT_COUNT(8),
        HID_FEATURE(HID_DATA | HID_VARIABLE | HID_ABSOLUTE),

        HID_REPORT_ID(3)                       /* status (GET_FEATURE, 40 B)  */
        HID_USAGE(0x03),
        HID_REPORT_COUNT(40),
        HID_FEATURE(HID_DATA | HID_VARIABLE | HID_ABSOLUTE),

        HID_REPORT_ID(4)                       /* version (GET_FEATURE, 16 B) */
        HID_USAGE(0x04),
        HID_REPORT_COUNT(16),
        HID_FEATURE(HID_DATA | HID_VARIABLE | HID_ABSOLUTE),

        HID_REPORT_ID(5)                       /* saved devices (GET, 80 B)   */
        HID_USAGE(0x05),
        HID_REPORT_COUNT(80),
        HID_FEATURE(HID_DATA | HID_VARIABLE | HID_ABSOLUTE),

        HID_REPORT_ID(6)                       /* scan results (GET, 126 B)   */
        HID_USAGE(0x06),
        HID_REPORT_COUNT(126),
        HID_FEATURE(HID_DATA | HID_VARIABLE | HID_ABSOLUTE),

        HID_REPORT_ID(7)                       /* AirPods (GET, 16 B)         */
        HID_USAGE(0x07),
        HID_REPORT_COUNT(16),
        HID_FEATURE(HID_DATA | HID_VARIABLE | HID_ABSOLUTE),

        HID_REPORT_ID(8)                       /* saved-device LIST (GET, 241 B) */
        HID_USAGE(0x08),
        HID_REPORT_COUNT_N(241, 2),
        HID_FEATURE(HID_DATA | HID_VARIABLE | HID_ABSOLUTE),

        HID_REPORT_ID(9)                       /* relay PHONE list (GET, 121 B)  */
        HID_USAGE(0x09),
        HID_REPORT_COUNT(121),
        HID_FEATURE(HID_DATA | HID_VARIABLE | HID_ABSOLUTE),

        HID_REPORT_ID(10)                      /* device identity (GET, 24 B):
                                                  bt_mac[6] + flash uid[8] +
                                                  RP2350 die chip-info[8] + pad
                                                  — the license-binding ID */
        HID_USAGE(0x0A),
        HID_REPORT_COUNT(24),
        HID_FEATURE(HID_DATA | HID_VARIABLE | HID_ABSOLUTE),

        HID_REPORT_ID(11)                      /* license (GET/SET 188 B):
                                                  GET  [state][key_id]
                                                       [features u32 LE]
                                                       [blob 118][email 64]
                                                       [unbind_confirmed 1]
                                                  SET  [blob 118][email 64]
                                                       = install            */
        HID_USAGE(0x0B),
        HID_REPORT_COUNT_N(189, 2),
        HID_FEATURE(HID_DATA | HID_VARIABLE | HID_ABSOLUTE),
    HID_COLLECTION_END,
#endif /* !USBPODS_LITE */
};

/* Configuration A: UAC2 + HID (the existing personality). */
uint8_t const desc_configuration_uac2[] =
{
    // Config number, interface count, string index, total length, attribute, power in mA
    TUD_CONFIG_DESCRIPTOR(CFGNUM_UAC2, ITF_NUM_TOTAL, 0, CONFIG_UAC2_TOTAL_LEN, 0x00, 100),

    // Interface number, string index, EP Out & EP In address, EP size
    TUD_AUDIO_HEADSET_STEREO_DESCRIPTOR(2, EPNUM_AUDIO_OUT, 0x00, EPNUM_AUDIO_INT | 0x80),

    // HID media keys
    TUD_HID_DESCRIPTOR(ITF_NUM_HID, 0, HID_ITF_PROTOCOL_NONE, sizeof(desc_hid_report), EPNUM_HID, 16, 10),

    // CDC serial console (macro carries its own IAD for the 2 interfaces)
    TUD_CDC_DESCRIPTOR(ITF_NUM_CDC, 6, EPNUM_CDC_NOTIF, 8, EPNUM_CDC_OUT, EPNUM_CDC_IN, 64),

#ifndef USBPODS_LITE
    // WebUSB config channel (0 endpoints)
    TUD_VENDOR_NOEP_DESCRIPTOR(ITF_NUM_VENDOR, 0),
#endif
};
_Static_assert(sizeof(desc_configuration_uac2) == CONFIG_UAC2_TOTAL_LEN,
               "UAC2+HID config descriptor length arithmetic broken");

/* Configuration B: UAC1 + HID (legacy hosts; claimed by uac1_device.c). */
uint8_t const desc_configuration_uac1[] =
{
    TUD_CONFIG_DESCRIPTOR(CFGNUM_UAC1, UAC1_ITF_TOTAL, 0, CONFIG_UAC1_TOTAL_LEN, 0x00, 100),
    UAC1_FUNC_DESCRIPTOR(/*stridx*/ 2, EPNUM_AUDIO_OUT, UAC1_RATE_48_3B),
    TUD_HID_DESCRIPTOR(UAC1_ITF_HID, 0, HID_ITF_PROTOCOL_NONE, sizeof(desc_hid_report), EPNUM_HID, 16, 10),
    TUD_CDC_DESCRIPTOR(UAC1_ITF_CDC, 6, EPNUM_CDC_NOTIF, 8, EPNUM_CDC_OUT, EPNUM_CDC_IN, 64),
#ifndef USBPODS_LITE
    TUD_VENDOR_NOEP_DESCRIPTOR(UAC1_ITF_VENDOR, 0),
#endif
};
_Static_assert(sizeof(desc_configuration_uac1) == CONFIG_UAC1_TOTAL_LEN,
               "UAC1+HID config descriptor length arithmetic broken");

uint8_t const desc_configuration_uac1_44[] =
{
    TUD_CONFIG_DESCRIPTOR(CFGNUM_UAC1, UAC1_ITF_TOTAL, 0, CONFIG_UAC1_TOTAL_LEN, 0x00, 100),
    UAC1_FUNC_DESCRIPTOR(/*stridx*/ 2, EPNUM_AUDIO_OUT, UAC1_RATE_44_3B),
    TUD_HID_DESCRIPTOR(UAC1_ITF_HID, 0, HID_ITF_PROTOCOL_NONE, sizeof(desc_hid_report), EPNUM_HID, 16, 10),
    TUD_CDC_DESCRIPTOR(UAC1_ITF_CDC, 6, EPNUM_CDC_NOTIF, 8, EPNUM_CDC_OUT, EPNUM_CDC_IN, 64),
#ifndef USBPODS_LITE
    TUD_VENDOR_NOEP_DESCRIPTOR(UAC1_ITF_VENDOR, 0),
#endif
};
_Static_assert(sizeof(desc_configuration_uac1_44) == CONFIG_UAC1_TOTAL_LEN,
               "UAC1 44.1 config descriptor length arithmetic broken");

/* Configuration C: HID + CDC only — Connect Mode 2 while no headset is
   connected. Interfaces renumber to 0/1/2; the HID and CDC drivers bind by
   instance, not interface number, so the firmware side is unchanged. */
#define CONFIG_NOAUDIO_TOTAL_LEN  (TUD_CONFIG_DESC_LEN + TUD_HID_DESC_LEN + TUD_CDC_DESC_LEN + TUD_VENDOR_NOEP_DESC_LEN)
#ifdef USBPODS_LITE
#define NOAUDIO_ITF_COUNT 3
#else
#define NOAUDIO_ITF_COUNT 4
#endif

uint8_t const desc_configuration_noaudio[] =
{
    TUD_CONFIG_DESCRIPTOR(1, NOAUDIO_ITF_COUNT, 0, CONFIG_NOAUDIO_TOTAL_LEN, 0x00, 100),
    TUD_HID_DESCRIPTOR(0, 0, HID_ITF_PROTOCOL_NONE, sizeof(desc_hid_report), EPNUM_HID, 16, 10),
    TUD_CDC_DESCRIPTOR(1, 6, EPNUM_CDC_NOTIF, 8, EPNUM_CDC_OUT, EPNUM_CDC_IN, 64),
#ifndef USBPODS_LITE
    TUD_VENDOR_NOEP_DESCRIPTOR(3, 0),
#endif
};
_Static_assert(sizeof(desc_configuration_noaudio) == CONFIG_NOAUDIO_TOTAL_LEN,
               "no-audio config descriptor length arithmetic broken");

// Invoked when received GET CONFIGURATION DESCRIPTOR
// Application return pointer to descriptor
// Descriptor contents must exist long enough for transfer to complete
uint8_t const * tud_descriptor_configuration_cb(uint8_t index)
{
  (void)index;   /* single configuration; arrays carry bConfigurationValue 1 */
  if (!s_audio_exposed) return desc_configuration_noaudio;   /* Connect Mode 2 */
  if (s_uac1_first)
    return (sample_rates[0] == 44100) ? desc_configuration_uac1_44
                                      : desc_configuration_uac1;
  return desc_configuration_uac2;
}

//--------------------------------------------------------------------+
// BOS + WebUSB (vendor control channel — Android Chrome uses WebUSB to reach
// the SAME commands desktop reaches over WebHID)
//--------------------------------------------------------------------+
#ifndef USBPODS_LITE   /* Lite: no vendor interface, no BOS, no WebUSB */
#define VENDOR_REQ_WEBUSB     0x01
#define VENDOR_REQ_HID_GET    0x10   /* device->host: wValue = report id */
#define VENDOR_REQ_HID_SET    0x11   /* host->device: wValue = report id */

#define BOS_TOTAL_LEN  (TUD_BOS_DESC_LEN + TUD_BOS_WEBUSB_DESC_LEN)

uint8_t const desc_bos[] =
{
  TUD_BOS_DESCRIPTOR(BOS_TOTAL_LEN, 1),
  // WebUSB platform capability. iLandingPage = 0 (no auto-open popup).
  TUD_BOS_WEBUSB_DESCRIPTOR(VENDOR_REQ_WEBUSB, 0),
};
uint8_t const * tud_descriptor_bos_cb(void) { return desc_bos; }

/* The existing report-based command protocol (webhid_get_report /
   webhid_set_report, uac.h) reused verbatim — just carried over EP0 vendor
   control transfers instead of HID feature reports. Same report ids, same
   payloads, same main-loop dispatch. */
extern uint16_t webhid_get_report(uint8_t report_id, uint8_t *buf, uint16_t reqlen);
extern void     webhid_set_report(uint8_t report_id, const uint8_t *buf, uint16_t len);

static uint8_t  s_vusb_buf[256];      /* GET fill / SET receive (max report ~241 B) */
static uint8_t  s_vusb_set_id;

bool tud_vendor_control_xfer_cb(uint8_t rhport, uint8_t stage, tusb_control_request_t const *req)
{
  if (stage == CONTROL_STAGE_SETUP) {
    switch (req->bRequest) {
      case VENDOR_REQ_HID_GET: {
        uint16_t n = webhid_get_report((uint8_t)req->wValue, s_vusb_buf, req->wLength);
        if (n == 0) return false;
        return tud_control_xfer(rhport, req, s_vusb_buf, n);
      }
      case VENDOR_REQ_HID_SET: {
        if (req->wLength == 0 || req->wLength > sizeof(s_vusb_buf))
          return tud_control_status(rhport, req);        /* nothing to do */
        s_vusb_set_id = (uint8_t)req->wValue;
        return tud_control_xfer(rhport, req, s_vusb_buf, req->wLength);   /* receive data */
      }
      default:
        return false;   /* unknown vendor request -> stall */
    }
  }
  if (stage == CONTROL_STAGE_DATA && req->bRequest == VENDOR_REQ_HID_SET) {
    webhid_set_report(s_vusb_set_id, s_vusb_buf, req->wLength);   /* queues to main loop */
  }
  return true;   /* DATA / ACK stages of handled requests */
}
#endif /* !USBPODS_LITE */

//--------------------------------------------------------------------+
// String Descriptors
//--------------------------------------------------------------------+

// String Descriptor Index
enum {
  STRID_LANGID = 0,
  STRID_MANUFACTURER,
  STRID_PRODUCT,
  STRID_SERIAL,
};

// array of pointer to string descriptors
char const *string_desc_arr[] =
{
  (const char[]) { 0x09, 0x04 },  // 0: is supported language is English (0x0409)
  "USBPods",                      // 1: Manufacturer
  USBPODS_PRODUCT_BASE,           // 2: Product ("... UAC1" per personality, see cb)
  NULL,                           // 3: Serials will use unique ID if possible
  "USBPods",                      // 4: Audio Interface
  "USBPods",                      // 5: Audio Interface
  "debug console",                // 6: CDC serial
};


static uint16_t _desc_str[32 + 1];

// Invoked when received GET STRING DESCRIPTOR request
// Application return pointer to descriptor, whose contents must exist long enough for transfer to complete
uint16_t const *tud_descriptor_string_cb(uint8_t index, uint16_t langid) {
  (void) langid;
  size_t chr_count;

  switch ( index ) {
    case STRID_LANGID:
      memcpy(&_desc_str[1], string_desc_arr[0], 2);
      chr_count = 1;
      break;

    case STRID_SERIAL:
      chr_count = board_usb_get_serial(_desc_str + 1, 32);
      break;

    default:
      // Note: the 0xEE index string is a Microsoft OS 1.0 Descriptors.
      // https://docs.microsoft.com/en-us/windows-hardware/drivers/usbcon/microsoft-defined-usb-descriptors

      if ( !(index < sizeof(string_desc_arr) / sizeof(string_desc_arr[0])) ) return NULL;

      const char *str = string_desc_arr[index];
      if (index == STRID_PRODUCT) {
          /* per-dongle name: last 4 hex of the BT MAC (cached in settings at
             HCI-up — all-zero until the radio has come up once ever). The
             UAC1 personality announces itself too. */
          static char prod[32];
          const uint8_t *m = settings()->bt_mac;
          if (m[0] | m[1] | m[2] | m[3] | m[4] | m[5])
              snprintf(prod, sizeof prod, USBPODS_PRODUCT_BASE " %02X%02X%s",
                       m[4], m[5], s_uac1_first ? " UAC1" : "");
          else
              snprintf(prod, sizeof prod, USBPODS_PRODUCT_BASE "%s", s_uac1_first ? " UAC1" : "");
          str = prod;
      }

      // Cap at max char
      chr_count = strlen(str);
      size_t const max_count = sizeof(_desc_str) / sizeof(_desc_str[0]) - 1; // -1 for string type
      if ( chr_count > max_count ) chr_count = max_count;

      // Convert ASCII string into UTF-16
      for ( size_t i = 0; i < chr_count; i++ ) {
        _desc_str[1 + i] = str[i];
      }
      break;
  }

  // first byte is length (including header), second byte is string type
  _desc_str[0] = (uint16_t) ((TUSB_DESC_STRING << 8) | (2 * chr_count + 2));

  return _desc_str;
}
