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
 #include "debug_cdc.h"
 #include "mic_request.h"
 #include "media_control.h"
 #include "audio_control_validation.h"

 #include "../btstack/btstack_avdtp_source.h"
 #include "../btstack/aacp_mic_dec.h"
 #include "../btstack/btstack_aacp.h"
 #include "../btstack/volume_scale.h"
 #include "../realtime_metrics.h"
 #include "pico/flash.h"
 #include "pico/time.h"
 #include "hardware/sync.h"



 //--------------------------------------------------------------------+
 // MACRO CONSTANT TYPEDEF PROTOTYPES
 //--------------------------------------------------------------------+

 // List of supported sample rates
 const uint32_t sample_rates[] = {48000};

 uint32_t current_sample_rate  = 48000;

 static _Atomic bool need_change_bt_volume;
 static _Atomic uint32_t volume_set_revision;
 static _Atomic uint32_t volume_set_generation;
 static _Atomic uint32_t volume_set_receipt;

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

 static uint32_t blink_interval_ms = BLINK_NOT_MOUNTED;

 // Audio controls
 // Current states
 static _Atomic int8_t mute[CFG_TUD_AUDIO_FUNC_1_N_CHANNELS_RX + 1];       // +1 for master channel 0
 static _Atomic int16_t volume[CFG_TUD_AUDIO_FUNC_1_N_CHANNELS_RX + 1];    // +1 for master channel 0

 static usb_mic_request_t mic_request;
 static bool usb_mounted;
 static bool usb_suspended;
 static _Atomic bool usb_host_available;
 static bool mic_interface_open;
 static bool speaker_active;
 static uint32_t speaker_last_packet_us;
 static _Atomic bool volume_notification_pending;

 // Main publishes routing intent; only the USB service changes descriptors.
 static _Atomic bool usb_bluetooth_requested;
 static _Atomic bool usb_audio_present = true;
 static bool usb_descriptor_audio = true;
 static bool usb_mode_reconnecting;
 static uint32_t usb_reconnect_at;
 #define USB_REENUMERATE_US 250000u

 #define USB_PLAYBACK_IDLE_US 50000u

 void usb_audio_set_bluetooth(bool requested) {
   // A USB mic SET_INTERFACE can arrive between main's policy read and this
   // publication. An already accepted USB recording request retains priority.
   uint32_t saved = save_and_disable_interrupts();
   atomic_store_explicit(&usb_bluetooth_requested,
       requested && !usb_mic_request_wanted(&mic_request), memory_order_release);
   restore_interrupts(saved);
 }

 bool usb_audio_available(void) {
   return !atomic_load_explicit(&usb_bluetooth_requested, memory_order_acquire) &&
          atomic_load_explicit(&usb_audio_present, memory_order_acquire);
 }

 static void usb_audio_mode_task(void) {
   bool audio = !atomic_load_explicit(&usb_bluetooth_requested, memory_order_acquire);
   if (!usb_mode_reconnecting && audio != usb_descriptor_audio) {
     usb_mode_reconnecting = true;
     atomic_store_explicit(&usb_audio_present, false, memory_order_release);
     tud_disconnect();
     // Pull-up removal alone does not clear TinyUSB's mounted state.
     tud_umount_cb();
     usb_reconnect_at = time_us_32() + USB_REENUMERATE_US;
   }
   if (usb_mode_reconnecting && (int32_t)(time_us_32() - usb_reconnect_at) >= 0) {
     // Coalesce changes during the detach interval, without blocking audio work.
     usb_descriptors_set_audio(audio);
     usb_descriptor_audio = audio;
     atomic_store_explicit(&usb_audio_present, audio, memory_order_release);
     usb_mode_reconnecting = false;
     tud_connect();
   }
 }

 static void usb_speaker_stop(void) {
   if (!speaker_active) return;
   speaker_active = false;
   set_usb_streaming(false);
 }

 static void usb_mic_update_request(void) {
   atomic_store_explicit(&usb_host_available, usb_mounted && !usb_suspended, memory_order_release);
   bool wanted = usb_mounted && !usb_suspended && usb_audio_available() && mic_interface_open;
   if (!wanted && !speaker_active) usb_media_cancel_volume();
   if (wanted != usb_mic_request_wanted(&mic_request)) aacp_mic_pcm_reset();
   usb_mic_request_set(&mic_request, wanted);
 }

 bool usb_mic_take_request(bool *wanted) {
   return wanted != NULL && usb_mic_request_take(&mic_request, wanted);
 }

 bool usb_mic_requested(void) {
   return usb_mic_request_wanted(&mic_request);
 }

 bool usb_host_active(void) {
   return usb_audio_available() && atomic_load_explicit(&usb_host_available, memory_order_acquire);
 }

 uint32_t usb_volume_set_revision(void) {
   return atomic_load_explicit(&volume_set_revision, memory_order_acquire);
 }

 uint32_t usb_volume_set_generation(void) {
   return atomic_load_explicit(&volume_set_generation, memory_order_acquire);
 }

 uint32_t usb_volume_set_receipt(void) {
   return atomic_load_explicit(&volume_set_receipt, memory_order_acquire);
 }

 // Buffer for speaker data
 // One receive callback carries at most one descriptor-sized USB packet.
 static union {
   int16_t pcm16[CFG_TUD_AUDIO_FUNC_1_EP_OUT_SZ_MAX / sizeof(int16_t)];
   float pcm32[CFG_TUD_AUDIO_FUNC_1_EP_OUT_SZ_MAX / sizeof(float)];
 } spk_buf;
 static bool speaker_interface_open;
 static uint8_t speaker_format;

 //void led_blinking_task(void);
 void audio_task(void);
 void audio_control_task(void);

 /*------------- MAIN -------------*/
 void tinyusb_main(void)
 {


  flash_safe_execute_core_init();

  //board_init();

   // init device stack on configured roothub port
   tusb_rhport_init_t dev_init = {
     .role = TUSB_ROLE_DEVICE,
     .speed = TUSB_SPEED_AUTO
   };
   tusb_init(BOARD_TUD_RHPORT, &dev_init);


 }


 uint32_t tinyusb_service_interval_us(void) {
   // A suspended open endpoint can resume without another SET_INTERFACE.
   // Keep its fast cadence; an idle console also retains diagnostic throughput.
   return speaker_interface_open || mic_interface_open || debug_cdc_terminal_connected() ?
       REALTIME_USB_SERVICE_BUDGET_US : REALTIME_USB_IDLE_INTERVAL_US;
 }

 void tinyusb_task(void){
    usb_audio_mode_task();
    tud_task(); // TinyUSB device task
    // A bus reset clears TinyUSB configuration without an unmount callback.
    if (usb_mounted && !tud_mounted()) tud_umount_cb();
    audio_task();
    usb_media_task();
    debug_cdc_task(); // diagnostics follow audio and media work
 }


void tinyusb_control_task(void){
  audio_control_task();
}

 //--------------------------------------------------------------------+
 // Device callbacks
 //--------------------------------------------------------------------+

 // Invoked when device is mounted
 void tud_mount_cb(void)
 {
   if (usb_mode_reconnecting) return;
   usb_speaker_stop();
   speaker_interface_open = false;
   speaker_format=0;
   dongle_playback_usb_format(0);
   usb_media_set_available(true);
   usb_mounted = true;
   usb_suspended = false;
   mic_interface_open = false;
   usb_mic_update_request();
 }

 // Invoked when device is unmounted
 void tud_umount_cb(void)
 {
   usb_media_set_available(false);
   usb_mounted = false;
   speaker_interface_open = false;
   speaker_format=0;
   dongle_playback_usb_format(0);
   mic_interface_open = false;
   usb_mic_update_request();
   usb_speaker_stop();
 }

 // Invoked when usb bus is suspended
 // remote_wakeup_en : if host allow us  to perform remote wakeup
 // Within 7ms, device must draw an average of current less than 2.5 mA from bus
 void tud_suspend_cb(bool remote_wakeup_en)
 {
   (void)remote_wakeup_en;
   usb_media_set_available(false);
   usb_suspended = true;
   usb_mic_update_request();
   usb_speaker_stop();
 }

 // Invoked when usb bus is resumed
 void tud_resume_cb(void)
 {
  usb_suspended = false;
  usb_media_set_available(usb_mounted);
  usb_mic_update_request();
 }

 // Helper for the mic clock (fixed 64 kHz — see USB_MIC_SAMPLE_RATE)
 static bool tud_audio_mic_clock_get_request(uint8_t rhport, audio_control_request_t const *request)
 {
   TU_ASSERT(request->bEntityID == UAC2_ENTITY_MIC_CLOCK);
   TU_VERIFY(request->bChannelNumber == 0);

   if (request->bControlSelector == AUDIO_CS_CTRL_SAM_FREQ)
   {
     if (request->bRequest == AUDIO_CS_REQ_CUR)
     {
       audio_control_cur_4_t curf = { (int32_t) tu_htole32(USB_MIC_SAMPLE_RATE) };
       return tud_audio_buffer_and_schedule_control_xfer(rhport, (tusb_control_request_t const *)request, &curf, sizeof(curf));
     }
     else if (request->bRequest == AUDIO_CS_REQ_RANGE)
     {
       audio_control_range_4_n_t(1) rangef =
       {
         .wNumSubRanges = tu_htole16(1),
         .subrange[0] = { .bMin = (int32_t) USB_MIC_SAMPLE_RATE,
                          .bMax = (int32_t) USB_MIC_SAMPLE_RATE, .bRes = 0 }
       };
       return tud_audio_buffer_and_schedule_control_xfer(rhport, (tusb_control_request_t const *)request, &rangef, sizeof(rangef));
     }
   }
   else if (request->bControlSelector == AUDIO_CS_CTRL_CLK_VALID &&
            request->bRequest == AUDIO_CS_REQ_CUR)
   {
     audio_control_cur_1_t cur_valid = { .bCur = 1 };
     return tud_audio_buffer_and_schedule_control_xfer(rhport, (tusb_control_request_t const *)request, &cur_valid, sizeof(cur_valid));
   }
   return false;
 }

 // Helper for clock get requests
 static bool tud_audio_clock_get_request(uint8_t rhport, audio_control_request_t const *request)
 {
   TU_ASSERT(request->bEntityID == UAC2_ENTITY_CLOCK);
   TU_VERIFY(request->bChannelNumber == 0);

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
   TU_VERIFY(request->bChannelNumber == 0);
   TU_VERIFY(request->bRequest == AUDIO_CS_REQ_CUR);

   if (request->bControlSelector == AUDIO_CS_CTRL_SAM_FREQ)
   {
     TU_VERIFY(request->wLength == sizeof(audio_control_cur_4_t));

     TU_VERIFY(usb_audio_fixed_rate(buf, request->wLength, sample_rates[0]));
     current_sample_rate = sample_rates[0];

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
   TU_VERIFY(request->bChannelNumber == 0);

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
         .subrange[0] = { .bMin = tu_htole16(-VOLUME_CTRL_50_DB), tu_htole16(VOLUME_CTRL_0_DB), tu_htole16(1) }
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
   TU_VERIFY(request->bChannelNumber == 0);
   TU_VERIFY(request->bRequest == AUDIO_CS_REQ_CUR);

   if (request->bControlSelector == AUDIO_FU_CTRL_MUTE)
   {
     TU_VERIFY(request->wLength == sizeof(audio_control_cur_1_t));

     TU_VERIFY(buf != NULL && buf[0] <= 1);
     mute[request->bChannelNumber] = (int8_t)buf[0];

     TU_LOG1("Set channel %d Mute: %d\r\n", request->bChannelNumber, mute[request->bChannelNumber]);

     return true;
   }
   else if (request->bControlSelector == AUDIO_FU_CTRL_VOLUME)
   {
     TU_VERIFY(request->wLength == sizeof(audio_control_cur_2_t));

     int16_t requested_volume;
     TU_VERIFY(usb_audio_volume_value(buf, request->wLength, &requested_volume));
     atomic_store_explicit(&volume[request->bChannelNumber], requested_volume, memory_order_release);
     atomic_store_explicit(&volume_set_generation, usb_media_generation(), memory_order_release);
     atomic_store_explicit(&volume_set_receipt, usb_media_volume_sent(), memory_order_release);
     atomic_fetch_add_explicit(&volume_set_revision, 1u, memory_order_release);

     TU_LOG1("Set channel %d volume: %d dB\r\n", request->bChannelNumber, volume[request->bChannelNumber]/256);

     // A same-value SET is still explicit intent after a peer/source change.
     atomic_store_explicit(&need_change_bt_volume, true, memory_order_release);

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
   if (!usb_audio_available()) return false;
   audio_control_request_t const *request = (audio_control_request_t const *)p_request;

   if (request->bEntityID == UAC2_ENTITY_CLOCK)
     return tud_audio_clock_get_request(rhport, request);
   if (request->bEntityID == UAC2_ENTITY_MIC_CLOCK)
     return tud_audio_mic_clock_get_request(rhport, request);
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
   if (!usb_audio_available()) return false;
   audio_control_request_t const *request = (audio_control_request_t const *)p_request;

   if (request->bEntityID == UAC2_ENTITY_SPK_FEATURE_UNIT)
     return tud_audio_feature_unit_set_request(rhport, request, buf);
   if (request->bEntityID == UAC2_ENTITY_CLOCK)
     return tud_audio_clock_set_request(rhport, request, buf);
   if (request->bEntityID == UAC2_ENTITY_MIC_CLOCK)
   {
     return request->bControlSelector == AUDIO_CS_CTRL_SAM_FREQ &&
            request->bRequest == AUDIO_CS_REQ_CUR && request->bChannelNumber == 0 &&
            usb_audio_fixed_rate(buf, request->wLength, USB_MIC_SAMPLE_RATE);
   }
   TU_LOG1("Set request not handled, entity = %d, selector = %d, request = %d\r\n",
           request->bEntityID, request->bControlSelector, request->bRequest);

   return false;
 }

 // USB callbacks publish intent; the main loop owns Bluetooth calls.
 bool tud_audio_set_itf_close_EP_cb(uint8_t rhport, tusb_control_request_t const *request)
 {
   if (!usb_audio_available()) return false;
   (void)rhport;
   uint16_t itf = tu_le16toh(request->wIndex);
   uint16_t alt = tu_le16toh(request->wValue);
   if (itf == ITF_NUM_AUDIO_STREAMING_SPK) {
     if(alt>(AIRPODS_USB_FLOAT?2:1))return false;
     if (speaker_format!=alt) {
       blink_interval_ms = BLINK_MOUNTED;
       speaker_interface_open = false;
       usb_speaker_stop();
       audio_slot_usb_reset();
       speaker_format=0;
       dongle_playback_usb_format(0);
     }
     return true;
   }
   if (itf != ITF_NUM_AUDIO_STREAMING_MIC || alt>1) return false;
   if (alt == 0) {
     mic_interface_open = false;
     usb_mic_update_request();
   }
   return true;
 }

 bool tud_audio_set_itf_cb(uint8_t rhport, tusb_control_request_t const *request)
 {
   if (!usb_audio_available()) return false;
   (void)rhport;
   uint16_t itf = tu_le16toh(request->wIndex);
   uint16_t alt = tu_le16toh(request->wValue);
   if (itf == ITF_NUM_AUDIO_STREAMING_MIC) {
     if(alt>1)return false;
     mic_interface_open = alt == 1;
     usb_mic_update_request();
     return true;
   }
   if (itf != ITF_NUM_AUDIO_STREAMING_SPK || alt>(AIRPODS_USB_FLOAT?2:1)) return false;
   if(speaker_format!=alt) {
     usb_speaker_stop();
     audio_slot_usb_reset();
     if(speaker_format)(void)tud_audio_clear_ep_out_ff();
   }
   speaker_format=(uint8_t)alt;
   dongle_playback_usb_format((uint8_t)alt);
   speaker_interface_open = alt != 0;
   blink_interval_ms = alt != 0 ? BLINK_STREAMING : BLINK_MOUNTED;
   if (alt == 0) usb_speaker_stop();
   return true;
 }

 // TinyUSB has copied this packet and rearmed OUT before calling post_read.
 bool tud_audio_rx_done_post_read_cb(uint8_t rhport, uint16_t n_bytes_received, uint8_t func_id, uint8_t ep_out, uint8_t cur_alt_setting)
 {
   (void)rhport;
   (void)func_id;
   (void)ep_out;

   uint16_t frame_bytes=cur_alt_setting==2?8u:4u;
   uint16_t maximum=cur_alt_setting==2?CFG_TUD_AUDIO_FUNC_1_FORMAT_2_EP_SZ_OUT:CFG_TUD_AUDIO_FUNC_1_FORMAT_1_EP_SZ_OUT;
   if (n_bytes_received > sizeof(spk_buf) || n_bytes_received > maximum || n_bytes_received % frame_bytes != 0) {
     dongle_playback_usb_packet(time_us_32(),0,cur_alt_setting,1);
     (void)tud_audio_clear_ep_out_ff();
     return false;
   }
   uint16_t spk_data_size = tud_audio_read(&spk_buf, n_bytes_received);
   if (spk_data_size != n_bytes_received) {
     dongle_playback_usb_packet(time_us_32(),0,cur_alt_setting,2);
     (void)tud_audio_clear_ep_out_ff();
     return false;
   }
   if ((cur_alt_setting!=1 && cur_alt_setting!=2) || cur_alt_setting!=speaker_format || !speaker_interface_open ||
       !usb_mounted || usb_suspended || !usb_audio_available()) return true;

   if (spk_data_size)
   {
    speaker_last_packet_us = time_us_32();
    if (!speaker_active) {
      speaker_active = true;
      set_usb_streaming(true);
    }
    uint16_t sample_count = spk_data_size/frame_bytes;
    dongle_playback_usb_packet(speaker_last_packet_us,sample_count,cur_alt_setting,0);
    // USB mute must preserve the AirPods volume for a later unmute.
    if (atomic_load_explicit(&mute[0], memory_order_acquire))
      memset(&spk_buf, 0, spk_data_size);
    if(cur_alt_setting==2)audio_slot_push_float(spk_buf.pcm32,sample_count);
    else audio_slot_push_samples(spk_buf.pcm16,sample_count);
   }

   return true;
 }

 // Feed the mic EP IN: called by the audio driver before each IN transfer is
 // loaded. Pull 1ms of samples (64 @ 64 kHz) from the decoder's PCM ring and
 // zero-fill any shortfall so the host always gets a full frame.
 bool tud_audio_tx_done_pre_load_cb(uint8_t rhport, uint8_t itf, uint8_t ep_in, uint8_t cur_alt_setting)
 {
   if (!usb_audio_available()) return false;
   (void)rhport;
   (void)itf;
   (void)ep_in;

   int16_t buf[USB_MIC_SAMPLE_RATE / 1000];   // 64 samples / ms, mono
   uint32_t got = 0;
   if (cur_alt_setting == 1 && usb_mic_requested())
     got = aacp_mic_usb_read(buf, TU_ARRAY_SIZE(buf));
   memset(buf + got, 0, sizeof(buf) - got * sizeof(*buf));

   tud_audio_write(buf, sizeof(buf));
   return true;
 }

 //--------------------------------------------------------------------+
 // AUDIO Task
 //--------------------------------------------------------------------+


 void audio_task(void)
 {
   if (speaker_active && (uint32_t)(time_us_32() - speaker_last_packet_us) >= USB_PLAYBACK_IDLE_US)
     usb_speaker_stop();
   // Only the USB service context writes TinyUSB endpoint FIFOs.
   if (!atomic_load_explicit(&volume_notification_pending, memory_order_acquire) ||
       !atomic_exchange_explicit(&volume_notification_pending, false, memory_order_acq_rel)) return;
   if (!usb_host_active()) return;
   const audio_interrupt_data_t data = {
     .bInfo = 0, .bAttribute = AUDIO_CS_REQ_CUR,
     .wValue_cn_or_mcn = 0, .wValue_cs = AUDIO_FU_CTRL_VOLUME,
     .wIndex_ep_or_int = 0, .wIndex_entity_id = UAC2_ENTITY_SPK_FEATURE_UNIT,
   };
   if (!tud_audio_int_write(&data))
     atomic_store_explicit(&volume_notification_pending, true, memory_order_release);
 }


void audio_control_task(void)
 {
   if (!usb_audio_available()) {
     atomic_store_explicit(&need_change_bt_volume, false, memory_order_release);
     atomic_store_explicit(&volume_notification_pending, false, memory_order_release);
     return;
   }
   if (*get_is_bt_sink_volume_changed_ptr() && bt_volume_usb_ready() &&
       !atomic_load_explicit(&need_change_bt_volume, memory_order_acquire))
   {

    uint8_t bt_level;
    bool confirmed = get_bt_volume(&bt_level);

    // A USB Set Volume may interrupt the Bluetooth readback above.
    uint32_t saved = save_and_disable_interrupts();
    if (!atomic_load_explicit(&need_change_bt_volume, memory_order_acquire) && bt_volume_usb_ready()) {
      if (confirmed) {
        volume[0] = bt_volume_to_usb(bt_level);
        atomic_store_explicit(&volume_notification_pending, true, memory_order_release);
      }
      *get_is_bt_sink_volume_changed_ptr() = false;
    }
    restore_interrupts(saved);
   }

   uint32_t saved = save_and_disable_interrupts();
   bool changed=atomic_exchange_explicit(&need_change_bt_volume, false, memory_order_acq_rel);
   uint32_t revision=usb_volume_set_revision(), generation=usb_volume_set_generation();
   uint32_t receipt=atomic_load_explicit(&volume_set_receipt, memory_order_acquire);
   int16_t requested=atomic_load_explicit(&volume[0], memory_order_acquire);
   restore_interrupts(saved);
   if (changed && generation==usb_media_generation() && usb_host_active()) {
    // Mute is applied to PCM; AVRCP zero would erase the level on readback.
    set_bt_volume_from_usb(requested,revision,generation,receipt);
    atomic_store_explicit(&volume_notification_pending, true, memory_order_release);
   }

  }

