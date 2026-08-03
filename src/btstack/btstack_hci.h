//
// Created by  on 8/10/23.
//

#include <stdint.h>
#include "btstack.h"


#ifndef PICOW_USB_BT_AUDIO_BTSTACK_HCI_H
#define PICOW_USB_BT_AUDIO_BTSTACK_HCI_H

static void hci_packet_handler(uint8_t packet_type, uint16_t channel, uint8_t *packet, uint16_t size);
void bt_hci_init(void);

const char * get_device_addr_string();
bd_addr_t * get_device_addr();
bd_addr_t * get_device_addr_from_list(uint8_t i);
void get_link_keys();


#endif //PICOW_USB_BT_AUDIO_BTSTACK_HCI_H
bool bt_pairing_active(void);   /* inquiry running for a new pairing */
void    bt_scan_start_list(void);   /* web scan: collect, don't auto-pair */
uint8_t bt_scan_count(void);
bool    bt_scan_get(uint8_t i, uint8_t mac[6], int8_t *rssi, char name[24]);
bool    bt_pair_to(uint8_t i);      /* pair to scan hit i (current slot) */
