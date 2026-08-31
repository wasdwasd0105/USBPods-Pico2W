/* HFP-AG, battery-only: a service-level connection to the headset over the
 * EXISTING classic ACL, for the one thing AVRCP cannot carry — a battery
 * PERCENTAGE. Two dialects, both parsed natively by BTstack's AG:
 *   HF Indicator #2 (AT+BIEV=2,0-100)  -> the standardized 1.7 path, primary
 *   Apple AT+XAPL / AT+IPHONEACCEV 0-9 -> widely spoken by non-Apple TWS too
 * No SCO, no calls, no voice recognition — see the SCO notes in the .c file.
 * Design + line-verified BTstack study: private/docs/hfp-battery-design.md. */
#ifndef USBPODS_HFP_BATTERY_H
#define USBPODS_HFP_BATTERY_H

#include <stdint.h>
#include <stdbool.h>
#include "bluetooth.h"

void   hfp_battery_init(void);          /* btstack_main, after l2cap_init      */
void   hfp_battery_register_sdp(void);  /* btstack_main, with the other records */
void   hfp_battery_sdp_hide(bool hide); /* phone-pairing window: hide the AG record */
void   hfp_battery_arm_dial(bd_addr_t addr); /* AVDTP signaling established    */
void   hfp_battery_link_down(void);     /* AVDTP signaling released            */
void   hfp_battery_note_link_teardown(bool local); /* blame our SLC only on a remote teardown */
void   hfp_battery_note_stream_started(void); /* defers the dial while fresh   */
int8_t hfp_battery_get_percent(void);   /* 0-100, or -1 = none                 */

#endif
