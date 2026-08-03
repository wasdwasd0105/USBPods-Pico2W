void tinyusb_main(void);
void tinyusb_task(void);
void tinyusb_control_task(void);
#include <stdint.h>
/* Queue a HID consumer-control key toward the USB host (press+release are
   sequenced by the control task; 0x00CD play/pause, 0x00B5 next, 0x00B6 prev). */
void usb_hid_consumer(uint16_t usage);
#include <stdbool.h>
bool usb_uac1_active(void);   /* host selected the UAC1 configuration */
void usb_serial_init(void);   /* register the CDC stdio driver (usb_serial.c) */
void usb_serial_task(void);
bool usb_vol_nudge_active(void);   /* echo window: host FU writes are our own nudges */
void usb_rx_gap_probe(void);       /* call per USB audio RX packet (UAC1+UAC2) */
extern volatile uint32_t usb_rx_gap_events;   /* missed-ISO-frame detector */
extern volatile uint32_t usb_rx_max_gap_ms;
/* WebHID config channel (webhid_cmd.c): vendor feature reports 2/3/4. */
uint16_t webhid_get_report(uint8_t report_id, uint8_t *buf, uint16_t reqlen);
void     webhid_set_report(uint8_t report_id, const uint8_t *buf, uint16_t len);
void     webhid_task(void);
