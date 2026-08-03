#define MAC_LEN               6

/* LED states — the web UI carries the detail now, the LED just answers
   "what is it doing right this second": */
typedef enum {
    LED_IDLE = 0,      /* solid ON — idle / not connected            */
    LED_CONN_SLOT1,    /* one blink, pause — connecting slot 1       */
    LED_CONN_SLOT2,    /* two blinks, pause — connecting slot 2      */
    LED_PAIRING,       /* fast flash — pairing scan running          */
    LED_PLAYING,       /* slow flash — audio streaming               */
} led_mode_t;

void led_set_mode(led_mode_t m);

uint8_t read_uint8_last_flash();

bool write_uint8_last_flash(uint8_t value);

void read_slot1_mac(uint8_t mac[MAC_LEN]);

bool write_slot1_mac(const uint8_t mac[MAC_LEN]);

void read_slot2_mac(uint8_t mac[MAC_LEN]);

bool write_slot2_mac(const uint8_t mac[MAC_LEN]);

#define SLOT_NAME_LEN 32
bool write_slot_name(uint8_t slot, const char *name);   /* slot 1/2 */
void read_slot_name(uint8_t slot, char out[SLOT_NAME_LEN]);

/* USB settings bits (1 = on; erased flash = all on = defaults). */
#define USBSET_BOOTCONN  0x01
#define USBSET_PAUSEDC   0x02
#define USBSET_UAC1      0x04
uint8_t read_settings_byte(void);
bool write_settings_byte(uint8_t v);