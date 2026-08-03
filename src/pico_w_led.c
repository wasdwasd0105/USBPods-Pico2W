#include <stdio.h>
#include "pico/stdlib.h"
#include "pico/cyw43_arch.h"
#include "pico_w_led.h"

#include "pico/flash.h"
#include "hardware/flash.h"


/* ONE worker + per-mode pattern tables. The old system ran three
   independent workers (generic blinker, double-, triple-blink) that could
   overlap and fight over the LED; a mode switch here always cancels the
   worker first, so exactly one pattern is ever live. */

typedef struct { uint16_t ms; bool on; } led_step_t;

static const led_step_t steps_slot1[] = { {200,true}, {1800,false} };
static const led_step_t steps_slot2[] = { {200,true}, {200,false}, {200,true}, {1800,false} };
static const led_step_t steps_pair[]  = { {100,true}, {100,false} };
static const led_step_t steps_play[]  = { {800,true}, {800,false} };

static const struct { const led_step_t *s; uint8_t n; } led_pat[] = {
    [LED_IDLE]       = { NULL, 0 },          /* solid ON, no worker */
    [LED_CONN_SLOT1] = { steps_slot1, 2 },
    [LED_CONN_SLOT2] = { steps_slot2, 4 },
    [LED_PAIRING]    = { steps_pair,  2 },
    [LED_PLAYING]    = { steps_play,  2 },
};

static led_mode_t led_mode = LED_IDLE;
static uint8_t    led_step;

static void led_do_work(async_context_t *ctx, async_at_time_worker_t *w) {
    const led_step_t *s = led_pat[led_mode].s;
    if (!s) return;                          /* IDLE raced in: stop silently */
    cyw43_arch_gpio_put(CYW43_WL_GPIO_LED_PIN, s[led_step].on);
    uint16_t d = s[led_step].ms;
    led_step = (uint8_t)((led_step + 1) % led_pat[led_mode].n);
    async_context_add_at_time_worker_in_ms(ctx, w, d);
}
static async_at_time_worker_t led_worker = { .do_work = led_do_work };

void led_set_mode(led_mode_t m) {
    if (m == led_mode) return;   /* dial retries re-set their mode: keep phase */
    led_mode = m;
    async_context_remove_at_time_worker(cyw43_arch_async_context(), &led_worker);
    led_step = 0;
    if (!led_pat[m].s) {
        cyw43_arch_gpio_put(CYW43_WL_GPIO_LED_PIN, true);
        return;
    }
    async_context_add_at_time_worker_in_ms(cyw43_arch_async_context(), &led_worker, 1);
}




// ─── PERSISTENT STORAGE SHIMS ────────────────────────────────────────────────
// The legacy stores lived INSIDE the BTstack TLV flash bank (last sector) and
// erased it on every write. All persistence now goes through the journaled
// settings store (src/settings.c); these shims keep the historic call sites
// unchanged. Mutators only touch the RAM cache — flash commits happen on the
// main loop (settings_tick), so calling these from BT/USB contexts is safe.
#include "settings.h"

uint8_t read_uint8_last_flash(void) {
    return settings()->cur_slot ? settings()->cur_slot : 1;
}

bool write_uint8_last_flash(uint8_t value) {
    settings()->cur_slot = (value == 2) ? 2 : 1;
    settings_mark_dirty();
    return true;
}

void read_slot1_mac(uint8_t mac[MAC_LEN]) { memcpy(mac, settings()->slot[0].mac, MAC_LEN); }
void read_slot2_mac(uint8_t mac[MAC_LEN]) { memcpy(mac, settings()->slot[1].mac, MAC_LEN); }

bool write_slot1_mac(const uint8_t mac[MAC_LEN]) {
    memcpy(settings()->slot[0].mac, mac, MAC_LEN);
    settings_mark_dirty();
    return true;
}

bool write_slot2_mac(const uint8_t mac[MAC_LEN]) {
    memcpy(settings()->slot[1].mac, mac, MAC_LEN);
    settings_mark_dirty();
    return true;
}

bool write_slot_name(uint8_t slot, const char *name) {
    char *dst = settings()->slot[slot == 2 ? 1 : 0].name;
    memset(dst, 0, SLOT_NAME_LEN);
    if (name) strncpy(dst, name, SLOT_NAME_LEN - 1);
    settings_mark_dirty();
    return true;
}

void read_slot_name(uint8_t slot, char out[SLOT_NAME_LEN]) {
    memcpy(out, settings()->slot[slot == 2 ? 1 : 0].name, SLOT_NAME_LEN);
    out[SLOT_NAME_LEN - 1] = 0;
}

uint8_t read_settings_byte(void) { return settings()->usbset; }

bool write_settings_byte(uint8_t v) {
    /* NO mask — every defined USBSET_* bit must persist. A narrow mask here
       silently dropped SWVOL (broke its persistence across reboots),
       RELAY_SBC ("chose SBC but phone still negotiated AAC"), and GAME
       ("game mode will not save to storage"). */
    settings()->usbset = v;
    settings_mark_dirty();
    return true;
}
