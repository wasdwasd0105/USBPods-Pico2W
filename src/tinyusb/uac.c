/*
 * The MIT License (MIT)
 *
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

 #include <stdio.h>
 #include <string.h>
 
 #include "bsp/board_api.h"
 #include "tusb.h"

 #include "usb_descriptors.h"
 #include "uac.h"

 #include "../btstack/btstack_avdtp_source.h"
#include "../btstack/btstack_sink_relay.h"   /* relay forces swvol */
 #include "pico/flash.h"
 #include "pico/time.h"


 
 //--------------------------------------------------------------------+
 // MACRO CONSTANT TYPEDEF PROTOTYPES
 //--------------------------------------------------------------------+
 
 // List of supported sample rates
 #include "../settings.h"

 uint32_t sample_rates[] = {48000};        /* set from settings at boot */

 uint32_t current_sample_rate  = 48000;

 bool need_change_bt_volume = false;
 
 #define N_SAMPLE_RATES  TU_ARRAY_SIZE(sample_rates)
 
 /* Blink pattern
  * - 25 ms   : streaming data
  * - 250 ms  : device not mounted
  * - 1000 ms : device mounted
  * - 2500 ms : device is suspended
  */
 enum
 {
   BLINK_STREAMING = 25,
   BLINK_NOT_MOUNTED = 250,
   BLINK_MOUNTED = 1000,
   BLINK_SUSPENDED = 2500,
 };
 
 enum
 {
   VOLUME_CTRL_0_DB = 0,
   VOLUME_CTRL_10_DB = 2560,
   VOLUME_CTRL_20_DB = 5120,
   VOLUME_CTRL_30_DB = 7680,
   VOLUME_CTRL_40_DB = 10240,
   VOLUME_CTRL_50_DB = 12800,
   VOLUME_CTRL_60_DB = 15360,
   VOLUME_CTRL_70_DB = 17920,
   VOLUME_CTRL_80_DB = 20480,
   VOLUME_CTRL_90_DB = 23040,
   VOLUME_CTRL_100_DB = 25600,
   VOLUME_CTRL_SILENCE = 0x8000,
 };

 #define BT_VOL_MAX   127
 #define USB_ATT_MAX  25600
 
 static uint32_t blink_interval_ms = BLINK_NOT_MOUNTED;
 
 // Audio controls
 // Current states
 int8_t mute[CFG_TUD_AUDIO_FUNC_1_N_CHANNELS_RX + 1];       // +1 for master channel 0
 int16_t volume[CFG_TUD_AUDIO_FUNC_1_N_CHANNELS_RX + 1];    // +1 for master channel 0
 
 int16_t volume0_last = 0;
 int8_t mute0_last = 0; 

 // Buffer for microphone data
 //int32_t mic_buf[CFG_TUD_AUDIO_FUNC_1_EP_IN_SW_BUF_SZ / 4];

 // Buffer for speaker data
 int32_t spk_buf[CFG_TUD_AUDIO_FUNC_1_EP_OUT_SW_BUF_SZ / 4];
 // Speaker data size received in the last frame
 extern volatile bool usb_pcm_tap;   /* 'U' console key (defined below) */
 int spk_data_size;
 // Resolution per format
 const uint8_t resolutions_per_format[CFG_TUD_AUDIO_FUNC_1_N_FORMATS] = {CFG_TUD_AUDIO_FUNC_1_FORMAT_1_RESOLUTION_RX,
                                                                         CFG_TUD_AUDIO_FUNC_1_FORMAT_2_RESOLUTION_RX};
 // Current resolution, update on format change
 uint8_t current_resolution;
 
 //void led_blinking_task(void);
 void audio_task(void);
 void audio_control_task(void);

 //--------------------------------------------------------------------+
 // HID consumer-control (media keys)
 //--------------------------------------------------------------------+
 extern uint8_t const desc_hid_report[];   /* usb_descriptors.c */

 uint8_t const *tud_hid_descriptor_report_cb(uint8_t instance)
 {
   (void)instance;
   return desc_hid_report;
 }

 uint16_t tud_hid_get_report_cb(uint8_t instance, uint8_t report_id, hid_report_type_t report_type,
                                uint8_t *buffer, uint16_t reqlen)
 {
   (void)instance;
   if (report_type == HID_REPORT_TYPE_FEATURE) {
     return webhid_get_report(report_id, buffer, reqlen);
   }
   return 0;
 }

 void tud_hid_set_report_cb(uint8_t instance, uint8_t report_id, hid_report_type_t report_type,
                            uint8_t const *buffer, uint16_t bufsize)
 {
   (void)instance;
   if (report_type == HID_REPORT_TYPE_FEATURE) {
     webhid_set_report(report_id, buffer, bufsize);
   }
 }

 /* Media-key sender: press is latched from any context (the BT handlers run
    outside the USB task), the state machine below sends press then release on
    consecutive hid_media_task() ticks (50 ms main loop) — the release is
    guaranteed so keys can never stick. */
 static volatile uint16_t hid_key_pending;   /* 0 = none */
 static uint16_t hid_key_cur;
 static uint8_t  hid_key_phase;              /* 0 idle, 1 pressed */

 void usb_hid_consumer(uint16_t usage)
 {
   hid_key_pending = usage;
 }

 /* ---- BT -> host volume sync, UAC1 mode -----------------------------------
    UAC2 notifies the host through the AC interrupt endpoint; UAC1's status
    interrupt is ignored by the legacy hosts UAC1 targets. Substitute: nudge
    the host with HID Volume Up/Down until its slider reaches the BT volume —
    we always know where the host sits because every key press makes it write
    the feature unit (SET_CUR -> volume[0]). Convergence-bounded: stop inside
    a 4% deadband or after the budget runs out (host key steps vary: Windows
    2%, macOS ~6%). */
 static volatile int s_vol_target_pct = -1;   /* -1 = idle */
 static uint8_t      s_vol_budget;
 static uint8_t      s_nudge_cooldown;        /* echo window after the last key */

 /* While nudging (and ~1 s after), host FU writes are OUR key presses echoing
    back — pushing them to BT would close the loop (BT notify -> key -> FU
    write -> set_bt_volume -> BT notify ...). */
 bool usb_vol_nudge_active(void)
 {
   return s_vol_target_pct >= 0 || s_nudge_cooldown > 0;
 }

 static void vol_nudge_start(uint8_t bt_level)
 {
   s_vol_target_pct = (bt_level > 127 ? 127 : bt_level) * 100 / 127;
   s_vol_budget = 30;
 }

 static uint8_t host_fu_to_pct(void);    /* defined with the curve LUT below */

 static int host_vol_pct(void)
 {
   /* Cubic-curve inversion (see s_db_to_pct) — must match the mapping the
      rest of the volume system uses, or the UAC1 nudge loop's deadband
      would chase a phantom offset. */
   return host_fu_to_pct();
 }

 static void hid_media_task(void)
 {
   if (!tud_hid_ready()) {
     return;
   }
   if (hid_key_phase == 0 && hid_key_pending) {
     hid_key_cur = hid_key_pending;
     hid_key_pending = 0;
     tud_hid_report(1, &hid_key_cur, 2);   /* consumer collection = report 1 */
     hid_key_phase = 1;
   } else if (hid_key_phase == 1) {
     uint16_t release = 0;
     tud_hid_report(1, &release, 2);
     hid_key_phase = 0;
   } else if (s_vol_target_pct >= 0 && usb_uac1_active()) {
     int d = s_vol_target_pct - host_vol_pct();
     if ((d > -4 && d < 4) || s_vol_budget == 0) {
       s_vol_target_pct = -1;           /* converged (or gave up) */
     } else {
       s_vol_budget--;
       s_nudge_cooldown = 20;           /* ~1 s echo window past the last key */
       hid_key_pending = (d > 0) ? 0x00E9 : 0x00EA;   /* Vol+ / Vol- */
     }
   } else if (s_nudge_cooldown > 0) {
     s_nudge_cooldown--;
   }
 }
 
 /* ---- USB Connect Mode (settings connmode) --------------------------------
    1 Manual (default): nothing automatic here.
    2 Manual, USB audio follows headset: the audio function is only served
      while a headset is connected; flips re-enumerate with a soft replug.
      HID and CDC stay in the no-audio personality, so the WebHID page and
      the serial console ride through it.
    3 Automatic: the host starts playing to the speaker while no headset is
      connected -> dial the saved slot, one attempt per 10 s. */
 bool bt_pairing_active(void);   /* btstack_hci.c — its header clashes with tusb */
 bool a2dp_user_dc_hold(void);   /* btstack_avdtp_source.c — user pressed disconnect */
 void a2dp_user_dc_clear(void);
 void usb_desc_set_audio_exposed(bool on);   /* usb_descriptors.c */
 bool usb_desc_audio_exposed(void);
 extern uint16_t usb_stop_delay;             /* defined below (audio task) */

 /* Replug hold: tud_disconnect/tud_connect run in the MAIN loop, but
   tud_task runs in the 500 us TIMER IRQ — an IRQ tud_task landing in the
   middle of the replug races usbd's state (Waveshare, connmode 2: watchdog
   "phase 5" right after "[connmode] audio restored -> USB replug"). The
   flag parks the IRQ-side task across the replug. */
volatile bool usb_replug_hold;

static void usb_mode_task(void)   /* 50 ms cadence (tinyusb_control_task) */
 {
   uint8_t mode = settings()->connmode;
#ifdef USBPODS_LITE
   /* Lite is MANUAL only: "USB audio follows headset" (2) and "auto-dial when
      the host plays" (3) are Full features, configured from the web UI that
      Lite does not have. A stored 2/3 (set under Full, or left over from an
      earlier build) must not keep dialing here. */
   mode = 1;
#endif
   bool conn = get_a2dp_connected_flag();

   /* Mode 2: descriptor shape tracks the headset. Held ~0.5 s before acting
      so a brief BT blip (reconfigure, slot switch) doesn't churn replugs. */
   static uint8_t flip_hold;
   bool want = (mode == 2) ? conn : true;
   if (want == usb_desc_audio_exposed()) {
     flip_hold = 0;
   } else if (++flip_hold >= 10) {
     flip_hold = 0;
     usb_desc_set_audio_exposed(want);
     printf("[connmode] audio %s -> USB replug\n", want ? "restored" : "hidden");
     usb_replug_hold = true;         /* park the IRQ-side tud_task (race!) */
     tud_disconnect();               /* same soft replug the WebHID op uses */
     sleep_ms(150);
     tud_connect();
     usb_replug_hold = false;
   }

   /* Mode 3: usb_stop_delay < 100 = speaker data seen within ~50 ms. */
   static uint16_t dial_cooldown;
   if (dial_cooldown) dial_cooldown--;
   if (usb_stop_delay >= 100) a2dp_user_dc_clear();  /* playback stopped: the
                                             next play is fresh user intent */
   if (mode == 3 && !conn && dial_cooldown == 0 &&
       usb_stop_delay < 100 && !bt_pairing_active() && !a2dp_user_dc_hold() &&
       !a2dp_source_wedge_hold()) {   /* peer connects but never streams:
                                         redialing it just re-chimes forever */
     const uint8_t *mac = settings()->slot[settings()->cur_slot == 2 ? 1 : 0].mac;
     bool have = false, zero = true;
     for (int i = 0; i < 6; i++) { have |= mac[i] != 0xFF; zero &= mac[i] == 0; }
     if (have && !zero) {
       printf("[connmode] host is playing -> dialing headset\n");
       a2dp_source_reconnect();
     }
     dial_cooldown = 200;            /* one attempt per 10 s while it plays */
   }
 }

 /*------------- MAIN -------------*/
 void tinyusb_main(void)
 {


  flash_safe_execute_core_init();

  /* Pipeline rate is boot-time: it shapes descriptors (UAC1) and the clock
     answers (UAC2), and the BT codec offers use the same bit. */
  sample_rates[0] = (settings()->usbset & USBSET_RATE44) ? 44100 : 48000;
  current_sample_rate = sample_rates[0];

  //board_init();
 
   // init device stack on configured roothub port
   tusb_rhport_init_t dev_init = {
     .role = TUSB_ROLE_DEVICE,
     .speed = TUSB_SPEED_AUTO
   };
   tusb_init(BOARD_TUD_RHPORT, &dev_init);


 }


 void tinyusb_task(void){
    if (usb_replug_hold) return;
    tud_task(); // TinyUSB device task
    usb_serial_task();   // drain printf ring -> CDC (same context as tud_task)
    audio_task(); 
 }
 

void tinyusb_control_task(void){
  //tud_task(); // TinyUSB device task
  { extern void usb_pcm_tap_tick(void); usb_pcm_tap_tick(); }
  { extern void usb_pcm_dump_tick(void); usb_pcm_dump_tick(); }  /* 'p' raw dump */
  audio_control_task();
  hid_media_task();
  webhid_task();
  usb_mode_task();
}

 //--------------------------------------------------------------------+
 // Device callbacks
 //--------------------------------------------------------------------+
 
 // Invoked when device is mounted
 extern bool usb_desc_uac1_first(void);

 void tud_mount_cb(void)
 {
   printf("USB configured (%s first)\n", usb_desc_uac1_first() ? "UAC1" : "UAC2");
   //blink_interval_ms = BLINK_MOUNTED;
 }
 
 // Invoked when device is unmounted
 void tud_umount_cb(void)
 {
   //blink_interval_ms = BLINK_NOT_MOUNTED;
 }
 
 // Invoked when usb bus is suspended
 // remote_wakeup_en : if host allow us  to perform remote wakeup
 // Within 7ms, device must draw an average of current less than 2.5 mA from bus
 void tud_suspend_cb(bool remote_wakeup_en)
 {
   (void)remote_wakeup_en;
   printf("tud_suspend_cb\n");
 }
 
 // Invoked when usb bus is resumed
 void tud_resume_cb(void)
 {
  printf("tud_resume_cb\n");
 }
 
 // Helper for clock get requests
 static bool tud_audio_clock_get_request(uint8_t rhport, audio_control_request_t const *request)
 {
   TU_ASSERT(request->bEntityID == UAC2_ENTITY_CLOCK);
 
   if (request->bControlSelector == AUDIO_CS_CTRL_SAM_FREQ)
   {
     if (request->bRequest == AUDIO_CS_REQ_CUR)
     {
       TU_LOG1("Clock get current freq %" PRIu32 "\r\n", current_sample_rate);
 
       audio_control_cur_4_t curf = { (int32_t) tu_htole32(current_sample_rate) };
       return tud_audio_buffer_and_schedule_control_xfer(rhport, (tusb_control_request_t const *)request, &curf, sizeof(curf));
     }
     else if (request->bRequest == AUDIO_CS_REQ_RANGE)
     {
       audio_control_range_4_n_t(N_SAMPLE_RATES) rangef =
       {
         .wNumSubRanges = tu_htole16(N_SAMPLE_RATES)
       };
       TU_LOG1("Clock get %d freq ranges\r\n", N_SAMPLE_RATES);
       for(uint8_t i = 0; i < N_SAMPLE_RATES; i++)
       {
         rangef.subrange[i].bMin = (int32_t) sample_rates[i];
         rangef.subrange[i].bMax = (int32_t) sample_rates[i];
         rangef.subrange[i].bRes = 0;
         TU_LOG1("Range %d (%d, %d, %d)\r\n", i, (int)rangef.subrange[i].bMin, (int)rangef.subrange[i].bMax, (int)rangef.subrange[i].bRes);
       }
 
       return tud_audio_buffer_and_schedule_control_xfer(rhport, (tusb_control_request_t const *)request, &rangef, sizeof(rangef));
     }
   }
   else if (request->bControlSelector == AUDIO_CS_CTRL_CLK_VALID &&
            request->bRequest == AUDIO_CS_REQ_CUR)
   {
     audio_control_cur_1_t cur_valid = { .bCur = 1 };
     TU_LOG1("Clock get is valid %u\r\n", cur_valid.bCur);
     return tud_audio_buffer_and_schedule_control_xfer(rhport, (tusb_control_request_t const *)request, &cur_valid, sizeof(cur_valid));
   }
   TU_LOG1("Clock get request not supported, entity = %u, selector = %u, request = %u\r\n",
           request->bEntityID, request->bControlSelector, request->bRequest);
   return false;
 }
 
 // Helper for clock set requests
 static bool tud_audio_clock_set_request(uint8_t rhport, audio_control_request_t const *request, uint8_t const *buf)
 {
   (void)rhport;
 
   TU_ASSERT(request->bEntityID == UAC2_ENTITY_CLOCK);
   TU_VERIFY(request->bRequest == AUDIO_CS_REQ_CUR);
 
   if (request->bControlSelector == AUDIO_CS_CTRL_SAM_FREQ)
   {
     TU_VERIFY(request->wLength == sizeof(audio_control_cur_4_t));
 
     current_sample_rate = (uint32_t) ((audio_control_cur_4_t const *)buf)->bCur;
 
     TU_LOG1("Clock set current freq: %" PRIu32 "\r\n", current_sample_rate);
 
     return true;
   }
   else
   {
     TU_LOG1("Clock set request not supported, entity = %u, selector = %u, request = %u\r\n",
             request->bEntityID, request->bControlSelector, request->bRequest);
     return false;
   }
 }
 
 // Helper for feature unit get requests
 static bool tud_audio_feature_unit_get_request(uint8_t rhport, audio_control_request_t const *request)
 {
   TU_ASSERT(request->bEntityID == UAC2_ENTITY_SPK_FEATURE_UNIT);
 
   if (request->bControlSelector == AUDIO_FU_CTRL_MUTE && request->bRequest == AUDIO_CS_REQ_CUR)
   {
     audio_control_cur_1_t mute1 = { .bCur = mute[request->bChannelNumber] };
     TU_LOG1("Get channel %u mute %d\r\n", request->bChannelNumber, mute1.bCur);
     return tud_audio_buffer_and_schedule_control_xfer(rhport, (tusb_control_request_t const *)request, &mute1, sizeof(mute1));
   }
   else if (request->bControlSelector == AUDIO_FU_CTRL_VOLUME)
   {
     if (request->bRequest == AUDIO_CS_REQ_RANGE)
     {
       audio_control_range_2_n_t(1) range_vol = {
         .wNumSubRanges = tu_htole16(1),
         .subrange[0] = { .bMin = tu_htole16(-VOLUME_CTRL_50_DB), tu_htole16(VOLUME_CTRL_0_DB), tu_htole16(256) }
       };
       TU_LOG1("Get channel %u volume range (%d, %d, %u) dB\r\n", request->bChannelNumber,
               range_vol.subrange[0].bMin / 256, range_vol.subrange[0].bMax / 256, range_vol.subrange[0].bRes / 256);
       return tud_audio_buffer_and_schedule_control_xfer(rhport, (tusb_control_request_t const *)request, &range_vol, sizeof(range_vol));
     }
     else if (request->bRequest == AUDIO_CS_REQ_CUR)
     {
       audio_control_cur_2_t cur_vol = { .bCur = tu_htole16(volume[request->bChannelNumber]) };
       TU_LOG1("Get channel %u volume %d dB\r\n", request->bChannelNumber, cur_vol.bCur / 256);
       return tud_audio_buffer_and_schedule_control_xfer(rhport, (tusb_control_request_t const *)request, &cur_vol, sizeof(cur_vol));
     }
   }
   TU_LOG1("Feature unit get request not supported, entity = %u, selector = %u, request = %u\r\n",
           request->bEntityID, request->bControlSelector, request->bRequest);
 
   return false;
 }
 
 // Helper for feature unit set requests
 static bool tud_audio_feature_unit_set_request(uint8_t rhport, audio_control_request_t const *request, uint8_t const *buf)
 {
   (void)rhport;
 
   TU_ASSERT(request->bEntityID == UAC2_ENTITY_SPK_FEATURE_UNIT);
   TU_VERIFY(request->bRequest == AUDIO_CS_REQ_CUR);
 
   if (request->bControlSelector == AUDIO_FU_CTRL_MUTE)
   {
     TU_VERIFY(request->wLength == sizeof(audio_control_cur_1_t));
 
     mute[request->bChannelNumber] = ((audio_control_cur_1_t const *)buf)->bCur;

     if(request->bChannelNumber == 0){
      need_change_bt_volume = true;
     }
 
     TU_LOG1("Set channel %d Mute: %d\r\n", request->bChannelNumber, mute[request->bChannelNumber]);
 
     return true;
   }
   else if (request->bControlSelector == AUDIO_FU_CTRL_VOLUME)
   {
     TU_VERIFY(request->wLength == sizeof(audio_control_cur_2_t));
 
     volume[request->bChannelNumber] = ((audio_control_cur_2_t const *)buf)->bCur;
 
     TU_LOG1("Set channel %d volume: %d dB\r\n", request->bChannelNumber, volume[request->bChannelNumber]/256);

     if(request->bChannelNumber == 0){
      need_change_bt_volume = true;
     }
 
     return true;
   }
   else
   {
     TU_LOG1("Feature unit set request not supported, entity = %u, selector = %u, request = %u\r\n",
             request->bEntityID, request->bControlSelector, request->bRequest);
     return false;
   }
 }
 
 //--------------------------------------------------------------------+
 // Application Callback API Implementations
 //--------------------------------------------------------------------+
 
 // Invoked when audio class specific get request received for an entity
 bool tud_audio_get_req_entity_cb(uint8_t rhport, tusb_control_request_t const *p_request)
 {
   audio_control_request_t const *request = (audio_control_request_t const *)p_request;
 
   if (request->bEntityID == UAC2_ENTITY_CLOCK)
     return tud_audio_clock_get_request(rhport, request);
   if (request->bEntityID == UAC2_ENTITY_SPK_FEATURE_UNIT)
     return tud_audio_feature_unit_get_request(rhport, request);
   else
   {
     TU_LOG1("Get request not handled, entity = %d, selector = %d, request = %d\r\n",
             request->bEntityID, request->bControlSelector, request->bRequest);
   }
   return false;
 }
 
 // Invoked when audio class specific set request received for an entity
 bool tud_audio_set_req_entity_cb(uint8_t rhport, tusb_control_request_t const *p_request, uint8_t *buf)
 {
   audio_control_request_t const *request = (audio_control_request_t const *)p_request;
 
   if (request->bEntityID == UAC2_ENTITY_SPK_FEATURE_UNIT)
     return tud_audio_feature_unit_set_request(rhport, request, buf);
   if (request->bEntityID == UAC2_ENTITY_CLOCK)
     return tud_audio_clock_set_request(rhport, request, buf);
   TU_LOG1("Set request not handled, entity = %d, selector = %d, request = %d\r\n",
           request->bEntityID, request->bControlSelector, request->bRequest);
 
   return false;
 }
 
 void usb_pcm_tap_session_reset(void);   /* defined with the tap below */

 bool tud_audio_set_itf_close_EP_cb(uint8_t rhport, tusb_control_request_t const * p_request)
 {
   (void)rhport;
 
   uint8_t const itf = tu_u16_low(tu_le16toh(p_request->wIndex));
   uint8_t const alt = tu_u16_low(tu_le16toh(p_request->wValue));
 
  if (ITF_NUM_AUDIO_STREAMING_SPK == itf && alt == 0) {
      blink_interval_ms = BLINK_MOUNTED;
      /* Host stopped the stream: pause NOW and DROP everything staged.
         Windows closes the ISO EP per SOUND — staged-but-unplayed PCM
         would play before the NEXT sound, and each close/open cycle then
         adds a sliver of latency (the "AirPods drift out of sync on
         Windows" report; UAC1 doesn't stage). The arc-era version paused
         WITHOUT clearing — that kept the stale samples and just delayed
         them. */
      {   /* GRACE REVERTED (HW: worse — PipeWire drains PCM before the
             close, so grace starved TX past the 100 ms stall threshold ->
             stall-suspend cascade from a single switch). Immediate close
             actions; liveness read before stop_delay is clobbered. */
          extern void audio_slot_request_flush(bool live);
          printf("[close] stop_delay %u -> live %d\n",
                 usb_stop_delay, usb_stop_delay <= 60);
          audio_slot_request_flush(usb_stop_delay <= 60);
      }
      usb_stop_delay = 101;
      set_usb_streaming(false);
      usb_pcm_tap_session_reset();   /* tap + rx-gap stats are per-session */
  }
   if (usb_pcm_tap) printf("[tap] EP CLOSE itf %u alt %u\n", itf, alt);

   return true;
 }
 
 bool tud_audio_set_itf_cb(uint8_t rhport, tusb_control_request_t const * p_request)
 {
   (void)rhport;
   uint8_t const itf = tu_u16_low(tu_le16toh(p_request->wIndex));
   uint8_t const alt = tu_u16_low(tu_le16toh(p_request->wValue));
 
   TU_LOG2("Set interface %d alt %d\r\n", itf, alt);
   {   /* EP reopen: a pending live-close grace ends as a non-event */
       extern void audio_slot_grace_cancel(void);
       if (alt != 0) audio_slot_grace_cancel();
   }
   if (usb_pcm_tap) printf("[tap] SET_ITF itf %u alt %u (open OK — arm succeeded)\n", itf, alt);
   if (ITF_NUM_AUDIO_STREAMING_SPK == itf && alt != 0)
       blink_interval_ms = BLINK_STREAMING;
 
   // Clear buffer when streaming format is changed
   spk_data_size = 0;
   if(alt != 0)
   {
     current_resolution = resolutions_per_format[alt-1];
   }
 
   return true;
 }

 uint16_t usb_stop_delay = 0;
 bool is_usb_audio_running = false;

 /* USB RX gap probe: host ISO packets arrive every 1 ms while playing — a
    gap means frames were MISSED (silent skips, audible as bursts in the mix
    and invisible to every BT-side counter). Fed by both the UAC2 and UAC1
    receive paths; read + reset via the 'R' debug dump. */
 volatile uint32_t usb_rx_gap_events;
 volatile uint32_t usb_rx_max_gap_ms;
 static uint32_t   usb_rx_last_ms;

 /* ---- PCM tap ('U' console key): what is the HOST actually sending? ----
    Aggregates in the USB IRQ, prints a 2-line summary every 500 ms via the
    non-blocking console (raw per-packet printing would starve the pipeline).
    peak/avg expose silent-vs-loud, jumps expose discontinuities (the "dirty"
    sound), zero pkts expose Windows sending empty audio, cadence/min/max/gap
    expose delivery stalls. One raw 8-sample dump per interval. */
 volatile bool usb_pcm_tap;
 static struct {
   uint32_t t0, last_ms, pkts, bytes, zero_pkts, jumps, gap_max;
   uint16_t min_pkt, max_pkt;
   int16_t  peak_l, peak_r, last_l, last_r;
   uint64_t energy;
 } tap;

 static void usb_pcm_tap_feed(const int16_t *pcm, uint16_t bytes)
 {
   uint32_t now = to_ms_since_boot(get_absolute_time());
   if (tap.t0 == 0) { tap.t0 = now; tap.min_pkt = 0xFFFF; }
   if (tap.last_ms && now - tap.last_ms > tap.gap_max) tap.gap_max = now - tap.last_ms;
   tap.last_ms = now;
   tap.pkts++; tap.bytes += bytes;
   if (bytes < tap.min_pkt) tap.min_pkt = bytes;
   if (bytes > tap.max_pkt) tap.max_pkt = bytes;
   uint16_t n = bytes / 2;   /* int16 count, L R interleaved */
   bool all_zero = true;
   for (uint16_t i = 0; i + 1 < n; i += 2) {
     int16_t l = pcm[i], r = pcm[i+1];
     if (l || r) all_zero = false;
     int al = l < 0 ? -l : l, ar = r < 0 ? -r : r;
     if (al > tap.peak_l) tap.peak_l = al;
     if (ar > tap.peak_r) tap.peak_r = ar;
     tap.energy += (uint32_t)(al + ar);
     int dl = l - tap.last_l; if (dl < 0) dl = -dl;
     int dr = r - tap.last_r; if (dr < 0) dr = -dr;
     if (dl > 8000 || dr > 8000) tap.jumps++;
     tap.last_l = l; tap.last_r = r;
   }
   if (all_zero) tap.zero_pkts++;

   if (now - tap.t0 >= 500) {
     printf("[tap] %lums pkts %lu bytes %lu (pkt %u..%u) peak L %d R %d avg %lu zero %lu jumps %lu maxgap %lums\n",
            (unsigned long)(now - tap.t0), (unsigned long)tap.pkts,
            (unsigned long)tap.bytes, tap.min_pkt == 0xFFFF ? 0 : tap.min_pkt,
            tap.max_pkt, tap.peak_l, tap.peak_r,
            (unsigned long)(tap.energy / (tap.bytes ? tap.bytes : 1)),
            (unsigned long)tap.zero_pkts, (unsigned long)tap.jumps,
            (unsigned long)tap.gap_max);
     printf("[tap] pcm: %d %d | %d %d | %d %d | %d %d\n",
            pcm[0], pcm[1], n > 3 ? pcm[2] : 0, n > 3 ? pcm[3] : 0,
            n > 5 ? pcm[4] : 0, n > 5 ? pcm[5] : 0,
            n > 7 ? pcm[6] : 0, n > 7 ? pcm[7] : 0);
     uint32_t keep_last = tap.last_ms; int16_t kl = tap.last_l, kr = tap.last_r;
     memset(&tap, 0, sizeof tap);
     tap.t0 = now; tap.min_pkt = 0xFFFF;
     tap.last_ms = keep_last; tap.last_l = kl; tap.last_r = kr;
   }
 }

 void usb_pcm_tap_feed_u1(const int16_t *pcm, uint16_t bytes) { usb_pcm_tap_feed(pcm, bytes); }

 void usb_pcm_tap_tick(void)   /* 50 ms cadence via tinyusb_control_task */
 {
   if (!usb_pcm_tap) return;
   uint32_t now = to_ms_since_boot(get_absolute_time());
   if (tap.t0 == 0) { tap.t0 = now; tap.min_pkt = 0xFFFF; return; }
   if (now - tap.t0 >= 500 && tap.pkts == 0) {
     printf("[tap] %lums pkts 0 — host sent NOTHING\n", (unsigned long)(now - tap.t0));
     tap.t0 = now;
   }
 }

 /* EP close = end of a host stream SESSION: start aggregation fresh so the
   close->reopen dead time never shows up as a fake in-stream "maxgap"
   (HW log: an idle pause between Windows test tones printed as an
   11796 ms gap) and the jump detector never compares across sessions. */
void usb_pcm_tap_session_reset(void)
{
  memset(&tap, 0, sizeof tap);
  usb_rx_last_ms = 0;   /* rx-gap probe: reopen dead time is not a gap */
}

void usb_pcm_tap_toggle(void)
 {
   usb_pcm_tap = !usb_pcm_tap;
   memset(&tap, 0, sizeof tap);
   printf("[tap] PCM tap %s\n", usb_pcm_tap ? "ON (500ms summaries)" : "OFF");
 }

/* ---- Raw PCM dump ('B' console key) --------------------------------------
   The tap above SUMMARISES; this prints the actual BYTES the host sends, so
   a known signal (play a sine) can be decoded offline and compared against
   what the encoder is fed.

   Runs CONTINUOUSLY until toggled off, as repeating BLOCKS of N consecutive
   packets. It cannot be a raw firehose: 1 pkt/ms x ~400 hex chars is ~400
   KB/s against a 115200 UART (~11 KB/s) and a drop-on-full console ring, so
   an unpaced dump would emit mostly shredded lines. A block is captured at
   full rate (so the waveform inside it is CONTINUOUS and analysable), then
   drained one packet per 50 ms tick from main-loop context — ~400 chars per
   tick vs the UART's ~575 per 50 ms, so every line is guaranteed COMPLETE.
   The next block starts as soon as the previous one is out.

   Capture itself is a bounded memcpy in the USB IRQ; printing there would
   stall the 1 ms ISO cadence. */
#define PCMDUMP_PKTS 16                      /* 16 ms of audio @ 1 pkt/ms   */
#define PCMDUMP_MAX  200                     /* >= EP_OUT_SZ_MAX (196 @48k) */
#define PCMDUMP_WAIT_MS 500                  /* block close-out if host idle */
static volatile bool     pcmdump_run;        /* 'B' toggle: keep blocking   */
static volatile bool     pcmdump_arm;        /* true while a block fills    */
static volatile uint8_t  pcmdump_w;          /* packets captured (IRQ)      */
static volatile uint32_t pcmdump_t0;         /* ms at block's first packet  */
static uint32_t          pcmdump_arm_ms;     /* ms the block was armed      */
static uint8_t           pcmdump_r;          /* packets printed (main loop) */
static bool              pcmdump_hdr;        /* block header still to print */
static uint32_t          pcmdump_block;
static uint16_t          pcmdump_len[PCMDUMP_PKTS];
static uint8_t           pcmdump_buf[PCMDUMP_PKTS][PCMDUMP_MAX];

/* USB IRQ context — keep it to a bounded memcpy. */
static void usb_pcm_dump_feed(const void *pcm, uint16_t bytes)
{
  if (!pcmdump_arm) return;
  uint8_t w = pcmdump_w;
  if (w >= PCMDUMP_PKTS) { pcmdump_arm = false; return; }
  if (w == 0) pcmdump_t0 = to_ms_since_boot(get_absolute_time());
  uint16_t n = (bytes > PCMDUMP_MAX) ? PCMDUMP_MAX : bytes;
  memcpy(pcmdump_buf[w], pcm, n);
  pcmdump_len[w] = n;
  /* Publish the packet only AFTER its bytes are stored: the drain prints
     strictly below w, so it can never read a buffer being written. */
  pcmdump_w = (uint8_t)(w + 1);
  if (pcmdump_w >= PCMDUMP_PKTS) pcmdump_arm = false;
}

void usb_pcm_dump_feed_u1(const void *pcm, uint16_t bytes)   /* UAC1 path */
{
  usb_pcm_dump_feed(pcm, bytes);
}

static void pcmdump_start_block(void)
{
  pcmdump_w   = 0;
  pcmdump_r   = 0;
  pcmdump_hdr = true;                 /* printed once the block is full */
  pcmdump_block++;
  pcmdump_arm_ms = to_ms_since_boot(get_absolute_time());
  pcmdump_arm = true;                 /* LAST: opens the IRQ writer */
}

void usb_pcm_dump_toggle(void)   /* 'b' */
{
  if (pcmdump_run || pcmdump_arm) {
    /* Stop, but let whatever is already captured drain — every line is
       self-contained, so a partial block is still usable data. */
    pcmdump_run = false;
    pcmdump_arm = false;
    printf("[pcm] dump OFF\n");
    return;
  }
  /* gain: this dump is PRE-gain (taken in the rx callback, before
     audio_slot_push_samples scales) — 32768 = unity, anything lower means
     the encoder is fed quieter audio than the bytes printed below. */
  extern uint16_t get_usb_sw_gain_q15(void);
  printf("[pcm] dump ON — repeating blocks of %u consecutive packets "
         "(%u ms of audio each, ~%u ms apart). Lines: "
         "\"[pcm] <idx> <bytes> <hex>\", hex = raw EP bytes, "
         "little-endian int16 L,R interleaved, %lu Hz, %u-bit, "
         "swgain %u/32768. Press b again to stop.\n",
         PCMDUMP_PKTS, PCMDUMP_PKTS, (PCMDUMP_PKTS + 2) * 50,
         (unsigned long)current_sample_rate, current_resolution,
         get_usb_sw_gain_q15());
  pcmdump_block = 0;
  pcmdump_run   = true;
  pcmdump_start_block();
}

void usb_pcm_dump_tick(void)   /* 50 ms cadence via tinyusb_control_task */
{
  if (pcmdump_arm) {
    /* The host going quiet mid-block is the very failure being hunted —
       close the block out instead of hanging the dump, and report even an
       empty one (that IS the finding: Windows stopped delivering). */
    if (to_ms_since_boot(get_absolute_time()) - pcmdump_arm_ms < PCMDUMP_WAIT_MS)
      return;
    pcmdump_arm = false;
  }

  if (pcmdump_hdr) {                    /* own tick: keeps every tick small */
    pcmdump_hdr = false;
    printf("[pcm] blk %lu t %lu n %u%s\n", (unsigned long)pcmdump_block,
           (unsigned long)pcmdump_t0, pcmdump_w,
           pcmdump_w ? "" : " — HOST SENT NOTHING");
    return;
  }

  if (pcmdump_r < pcmdump_w) {
    static const char hexd[] = "0123456789abcdef";
    uint8_t  i = pcmdump_r;
    uint16_t n = pcmdump_len[i];
    char     line[PCMDUMP_MAX * 2 + 1];
    for (uint16_t b = 0; b < n; b++) {
      line[b * 2]     = hexd[pcmdump_buf[i][b] >> 4];
      line[b * 2 + 1] = hexd[pcmdump_buf[i][b] & 0x0F];
    }
    line[n * 2] = '\0';
    printf("[pcm] %u %u %s\n", i, n, line);
    pcmdump_r = (uint8_t)(i + 1);
    return;
  }

  if (pcmdump_run) pcmdump_start_block();   /* block drained: next one */
}

 void usb_rx_gap_probe(void)
 {
   uint32_t now = to_ms_since_boot(get_absolute_time());
   if (usb_rx_last_ms != 0) {
     uint32_t gap = now - usb_rx_last_ms;
     if (gap > 4) {
       usb_rx_gap_events++;
       if (gap > usb_rx_max_gap_ms) usb_rx_max_gap_ms = gap;
     }
   }
   usb_rx_last_ms = now;
 }

/* 'F' console key: splice SIMULATOR (re-added for the flush experiment).
   Reproduces a host EP close -> ~50 ms no PCM -> reopen in the same IRQ
   context as the real close handler. */
volatile uint8_t usb_splice_sim;

 bool tud_audio_rx_done_pre_read_cb(uint8_t rhport, uint16_t n_bytes_received, uint8_t func_id, uint8_t ep_out, uint8_t cur_alt_setting)
 {
   (void)rhport;
   (void)func_id;
   (void)ep_out;
   (void)cur_alt_setting;

   if (usb_splice_sim) {
     if (usb_splice_sim == 50) {
       extern void audio_slot_request_flush(bool live);
       audio_slot_request_flush(true);
       usb_stop_delay = 101;
       set_usb_streaming(false);
       usb_pcm_tap_session_reset();
       printf("[sim] EP splice injected (50 pkts dropped)\n");
     }
     usb_splice_sim--;
     tud_audio_read(spk_buf, n_bytes_received);   /* drain + discard */
     spk_data_size = 0;
     return true;
   }

   spk_data_size = tud_audio_read(spk_buf, n_bytes_received);

   if (spk_data_size)
   {
    if (usb_pcm_tap) usb_pcm_tap_feed((const int16_t *)spk_buf, (uint16_t)spk_data_size);
    usb_pcm_dump_feed(spk_buf, (uint16_t)spk_data_size);   /* 'p' raw dump */
    usb_rx_gap_probe();
    usb_stop_delay = 0;
    set_usb_streaming(true);
    if (current_resolution == 16)
    {
      int16_t *src = (int16_t *)spk_buf;
      uint16_t sample_count = spk_data_size / 4; // should be 44-45

      audio_slot_push_samples(src, sample_count);

      is_usb_audio_running = true;
      spk_data_size = 0;
    }
   } 

   return true;
 }
 
//  bool tud_audio_tx_done_pre_load_cb(uint8_t rhport, uint8_t itf, uint8_t ep_in, uint8_t cur_alt_setting)
//  {
//    (void)rhport;
//    (void)itf;
//    (void)ep_in;
//    (void)cur_alt_setting;
 
//    // This callback could be used to fill microphone data separately
//    return true;
//  }
 
 //--------------------------------------------------------------------+
 // AUDIO Task
 //--------------------------------------------------------------------+


 void audio_task(void)
 {
  if (is_usb_audio_running){
    usb_stop_delay = 0;
  }else if (usb_stop_delay < 1000){   /* clamp: a free-running ++ wraps the
                                         uint16 every ~33 s and fakes a
                                         "host playing" window for Mode 3 */
    usb_stop_delay++;
    if (usb_stop_delay > 100){
      set_usb_streaming(false);
      usb_rx_last_ms = 0;             /* host paused: not a "gap" on resume */
    }
  }
  is_usb_audio_running = false;
 }
 

/* macOS maps its volume slider CUBICALLY (position^3 ~= amplitude) into the
   FU dB range; mapping the dB back LINEARLY caused the "vol bar mismatch"
   (web bar ~65% for a half-way Mac slider) and a too-steep loudness taper —
   in BOTH swvol and classic modes. Invert with pct = 100 * 10^(dB/60). */
static const uint8_t s_db_to_pct[51] = {
    100,96,93,89,86,83,79,76,74,71,68,66,63,61,58,56,54,52,50,48,
     46,45,43,41,40,38,37,35,34,33,32,30,29,28,27,26,25,24,23,22,
     22,21,20,19,18,18,17,16,16,15,15 };

static uint8_t host_fu_to_pct(void) {      /* volume[0] dB -> slider percent  */
    int att_db = -(volume[0] / 256);
    if (att_db < 0) att_db = 0;
    if (att_db > 50) att_db = 50;
    return s_db_to_pct[att_db];
}

static int pct_to_att_db(uint8_t pct) {    /* inverse: nearest LUT attenuation */
    for (int d = 0; d <= 50; d++) {
        if (s_db_to_pct[d] <= pct) return d;
    }
    return 50;
}

void audio_control_task(void)
 {
   /* Software USB volume mode (ESP32 design): the host's volume becomes a
      SOFTWARE gain on USB PCM only — the headset's absolute volume and the
      relay's phone audio are decoupled, each with its own web bar.
      FORCED ON while a relay phone is connected: in classic mode the host
      volume drives the headset's absolute volume, which scales the phone's
      relay audio too — turning the computer down would duck the phone's
      music. The USER SETTING is never modified; when the phone disconnects
      the mode falls back to whatever the setting says. */
   bool sw_now = (settings()->usbset & USBSET_SWVOL) || bt_sink_relay_connected();
   static bool sw_prev;
   if (sw_now != sw_prev) {
       sw_prev = sw_now;
       if (sw_now) {
           /* entering: adopt the host slider position as the sw gain */
           uint8_t pct = host_fu_to_pct();
           if (mute[0]) pct = 0;
           set_usb_sw_gain_pct(pct);
       } else {
           /* leaving: classic mode scales in the HEADSET, so the software
              gain must return to unity — and the headset volume re-syncs
              from the host FU on the next pass. */
           set_usb_sw_gain_pct(100);
           need_change_bt_volume = true;
       }
   }
   if (sw_now) {
       *get_is_bt_sink_volume_changed_ptr() = false;   /* no BT->host push  */
       if (need_change_bt_volume) {                    /* host FU -> gain   */
           need_change_bt_volume = false;
           uint8_t pct = host_fu_to_pct();
           if (mute[0]) pct = 0;
           set_usb_sw_gain_pct(pct);
           settings()->usb_vol_p1 = (uint8_t)(pct + 1);
           settings_mark_dirty();
       }
       return;
   }

   if (*get_is_bt_sink_volume_changed_ptr())
   {

    uint8_t bt_level = get_bt_volume();

    mute[0] = get_bt_mute();

    // BT level -> host FU dB through the host's cubic curve (so the OS
    // slider lands at the matching POSITION, not the linear-dB spot)
    uint8_t hpct = (bt_level > BT_VOL_MAX) ? 100
                 : (uint8_t)(((uint32_t)bt_level * 100 + 63) / BT_VOL_MAX);
    volume[0] = (int16_t)(-pct_to_att_db(hpct) * 256);

     // 6.1 Interrupt Data Message
     const audio_interrupt_data_t data = {
       .bInfo = 0,                                       // Class-specific interrupt, originated from an interface
       .bAttribute = AUDIO_CS_REQ_CUR,                   // Caused by current settings
       .wValue_cn_or_mcn = 0,                            // CH0: master volume
       .wValue_cs = AUDIO_FU_CTRL_VOLUME,                // Volume change
       .wIndex_ep_or_int = 0,                            // From the interface itself
       .wIndex_entity_id = UAC2_ENTITY_SPK_FEATURE_UNIT, // From feature unit
     };
 
     if (usb_uac1_active()) {
       vol_nudge_start(bt_level);    /* UAC1: walk the host there with vol keys */
     } else if (usb_desc_audio_exposed()) {
       tud_audio_int_write(&data);   /* UAC2 interrupt EP: tell the host */
     }
     *get_is_bt_sink_volume_changed_ptr() = false;
   }

   if (need_change_bt_volume){

    //printf("vol is %d\n", volume[0]);
    
    if(mute[0] == 1 && volume0_last == volume[0]){
      if (mute0_last == 1){
        set_bt_volume(-50 + host_fu_to_pct() / 2);   /* cubic-corrected */
        mute0_last = 0;
        mute[0] = 0;
      }else{
        mute0_last = 1;
        set_bt_volume(-50);
      }
    }else{
      set_bt_volume(-50 + host_fu_to_pct() / 2);     /* cubic-corrected */
    }
    volume0_last = volume[0];
    need_change_bt_volume = false;

    const audio_interrupt_data_t data = {
      .bInfo = 0,                                       // Class-specific interrupt, originated from an interface
      .bAttribute = AUDIO_CS_REQ_CUR,                   // Caused by current settings
      .wValue_cn_or_mcn = 0,                            // CH0: master volume
      .wValue_cs = AUDIO_FU_CTRL_VOLUME,                // Volume change
      .wIndex_ep_or_int = 0,                            // From the interface itself
      .wIndex_entity_id = UAC2_ENTITY_SPK_FEATURE_UNIT, // From feature unit
    };

    if (!usb_uac1_active() && usb_desc_audio_exposed()) {
      tud_audio_int_write(&data);
    }
    //*get_is_bt_sink_volume_changed_ptr() = false;
   }

  }
 
