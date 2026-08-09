#ifndef _PICO_BTSTACK_BTSTACK_CONFIG_H
#define _PICO_BTSTACK_BTSTACK_CONFIG_H

// BTstack features that can be enabled
#ifdef ENABLE_BLE
#define ENABLE_LE_PERIPHERAL
#define ENABLE_LE_CENTRAL
#define ENABLE_L2CAP_LE_CREDIT_BASED_FLOW_CONTROL_MODE
#endif
#define ENABLE_LOG_INFO
#define ENABLE_LOG_ERROR
#define ENABLE_PRINTF_HEXDUMP
#define ENABLE_SCO_OVER_HCI

// BTstack configuration. buffers, sizes, ...
#define HCI_OUTGOING_PRE_BUFFER_SIZE 4
#define HCI_ACL_PAYLOAD_SIZE (1691 + 4)
#define HCI_ACL_CHUNK_SIZE_ALIGNMENT 4
/* Sink relay: headset (source role) + phone (sink role) = 2 AVDTP links.
   Endpoints: up to 4 lazily-created source SEPs (SBC/AAC/LDAC/ELD) +
   2 sink SEPs (SBC + AAC-LC). */
#define MAX_NR_AVDTP_CONNECTIONS 2
#define MAX_NR_AVDTP_STREAM_ENDPOINTS 6
#define MAX_NR_AVRCP_CONNECTIONS 2
#define MAX_NR_BNEP_CHANNELS 1
#define MAX_NR_BNEP_SERVICES 1
#define MAX_NR_BTSTACK_LINK_KEY_DB_MEMORY_ENTRIES  2
#define MAX_NR_GATT_CLIENTS 1
#define MAX_NR_HCI_CONNECTIONS 2
#define MAX_NR_HID_HOST_CONNECTIONS 1
#define MAX_NR_HIDS_CLIENTS 1
#define MAX_NR_HFP_CONNECTIONS 1
/* headset: AVDTP sig+media, AVCTP, AAP = 4; phone: AVDTP sig+media, AVCTP = 3;
   +1 transient SDP-client channel during outgoing connects. */
#define MAX_NR_L2CAP_CHANNELS  9   /* +1: the HFP-AG RFCOMM multiplexer holds a
                                      persistent L2CAP channel; 8 left no room for
                                      the SDP-query transient in the dual-stream case */
#define MAX_NR_L2CAP_SERVICES  4   /* RFCOMM (HFP-AG) + AVDTP + AVCTP + SDP.
                                      3 was exactly full BEFORE the HFP work; adding
                                      rfcomm_init first in btstack_main silently evicted
                                      SDP -- l2cap_register_service fails quietly, and a
                                      pairing phone then can't browse PSM 1: ACL connects,
                                      no SSP ever starts, phone drops with 0x13,
                                      "pairing unsuccessful" (HW 2026-08-10, both iOS and
                                      Android). Count services when adding protocols. */
#define MAX_NR_RFCOMM_CHANNELS 1
#define MAX_NR_RFCOMM_MULTIPLEXERS 1
#define MAX_NR_RFCOMM_SERVICES 1
#define MAX_NR_SERVICE_RECORD_ITEMS 6   /* A2DP src + snk, AVRCP tgt + ctl */
#define MAX_NR_SM_LOOKUP_ENTRIES 3
#define MAX_NR_WHITELIST_ENTRIES 16
#define MAX_NR_LE_DEVICE_DB_ENTRIES 16

// Limit number of ACL/SCO Buffer to use by stack to avoid cyw43 shared bus overrun.
// 3 -> 6 for the sink relay: two A2DP links share these TX slots; at 3, a brief
// retransmit episode on the headset link wedges ALL sending (the 100ms
// codec_ready_to_send stall -> staged-audio drop). ESP32 relay made the same
// move (TX 4 -> 6, round 16). No WiFi on this build, so bus headroom is ample.
#define MAX_NR_CONTROLLER_ACL_BUFFERS 6
#define MAX_NR_CONTROLLER_SCO_PACKETS 3

// Enable and configure HCI Controller to Host Flow Control to avoid cyw43 shared bus overrun
#define ENABLE_HCI_CONTROLLER_TO_HOST_FLOW_CONTROL
#define HCI_HOST_ACL_PACKET_LEN 1024
/* 3 -> 8: with the sink relay there are TWO A2DP links feeding the host; at
   3 credits the controller stalls delivery whenever one run-loop pass is
   slow (encode burst), its RX buffers back up, air ACKs stop and the phone's
   retransmissions surface as repeated dropouts — the ESP32 hit the identical
   failure and fixed it with controller RX buffers 6->14 (its round 13).
   Credits are flow-control accounting, not allocated buffers: free. */
#define HCI_HOST_ACL_PACKET_NUM 8
#define HCI_HOST_SCO_PACKET_LEN 120
#define HCI_HOST_SCO_PACKET_NUM 3

// Link Key DB and LE Device DB using TLV on top of Flash Sector interface
#define NVM_NUM_DEVICE_DB_ENTRIES 16
#define NVM_NUM_LINK_KEYS 16

// We don't give btstack a malloc, so use a fixed-size ATT DB.
#define MAX_ATT_DB_SIZE 512

// BTstack HAL configuration
#define HAVE_EMBEDDED_TIME_MS

// map btstack_assert onto Pico SDK assert()
#define HAVE_ASSERT

// Some USB dongles take longer to respond to HCI reset (e.g. BCM20702A).
#define HCI_RESET_RESEND_TIMEOUT_MS 1000

#define ENABLE_SOFTWARE_AES128
#define ENABLE_MICRO_ECC_FOR_LE_SECURE_CONNECTIONS

#define HAVE_BTSTACK_STDIN

// To get the audio demos working even with HCI dump at 115200, this truncates long ACL packets
//#define HCI_DUMP_STDOUT_MAX_SIZE_ACL 100

#ifdef ENABLE_CLASSIC
#define ENABLE_L2CAP_ENHANCED_RETRANSMISSION_MODE
#endif

#endif // _PICO_BTSTACK_BTSTACK_CONFIG_H
