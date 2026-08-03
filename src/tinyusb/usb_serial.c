/* USB CDC-ACM serial console — replaces the external UART converter.
 *
 * A custom pico stdio driver, NOT pico_stdio_usb (that component insists on
 * owning tusb_init + its own descriptors; this project runs a custom
 * composite device). printf can fire from ANY context here — the main loop,
 * the 500 us tud_task timer IRQ, BTstack's threadsafe-background callbacks —
 * so out_chars only pushes into a lock-free-ish ring (short critical
 * section); usb_serial_task() drains it into the CDC FIFO from tud_task
 * context, where TinyUSB's mutexes are already exercised by the rest of the
 * stack. Dropped-on-full by design: logging must never stall audio.
 *
 * Also implements the picotool convention: opening the port at 1200 baud
 * reboots into BOOTSEL, so reflashing needs no button press.
 */

#include <string.h>

#include "pico/stdio.h"
#include "pico/stdio/driver.h"
#include "pico/bootrom.h"
#include "hardware/sync.h"
#include "hardware/uart.h"
#include "../settings.h"                      /* USBSET_UART console gate */
#include "hardware/gpio.h"
#include "tusb.h"

#define TXR_SZ 4096   /* power of two; boot chatter bursts are ~2 KB */
static uint8_t  s_txr[TXR_SZ];
static volatile uint32_t s_txr_w, s_txr_r;
static volatile uint32_t s_tx_dropped;

/* UART mirror — output only, NEVER blocking. The SDK's stdio_uart driver
   busy-waits ~87 us per char, so one log line stalled the shared BT/audio
   core for 10-25 ms (audible bursts). Instead printf lands in this ring and
   trickles out only while the UART TX FIFO has room. Drop-on-full: a debug
   port loses old chatter, the headset keeps its packets. */
#define UTXR_SZ 2048
static uint8_t  s_utxr[UTXR_SZ];
static volatile uint32_t s_utxr_w, s_utxr_r;

static void cdc_out_chars(const char *buf, int len)
{
    uint32_t save = save_and_disable_interrupts();
    for (int i = 0; i < len; i++) {
        uint32_t next = (s_txr_w + 1) & (TXR_SZ - 1);
        if (next == s_txr_r) {         /* full: drop the rest, count it */
            s_tx_dropped += (uint32_t)(len - i);
            break;
        }
        s_txr[s_txr_w] = (uint8_t)buf[i];
        s_txr_w = next;
    }
    for (int i = 0; i < len; i++) {    /* mirror to the UART ring */
        uint32_t next = (s_utxr_w + 1) & (UTXR_SZ - 1);
        if (next == s_utxr_r) break;   /* full: drop silently */
        s_utxr[s_utxr_w] = (uint8_t)buf[i];
        s_utxr_w = next;
    }
    restore_interrupts(save);
}

static int cdc_in_chars(char *buf, int len)
{
    /* UART RX first: the hardware UART (debug-probe bridge) is a full
       console — same debug keys as the CDC terminal — when the web
       "UART serial console" switch is on. Non-blocking: only drains what
       already sits in the FIFO. */
    if (settings()->usbset & USBSET_UART) {
        int n = 0;
        while (n < len && uart_is_readable(uart_default)) {
            buf[n++] = (char)uart_get_hw(uart_default)->dr;
        }
        if (n) return n;
    }

    if (!tud_cdc_connected()) {
        return PICO_ERROR_NO_DATA;
    }
    uint32_t u = tud_cdc_read(buf, (uint32_t)len);
    return u ? (int)u : PICO_ERROR_NO_DATA;
}

/* Input is EVENT-DRIVEN on pico stdio: consumers (btstack_stdin) register a
   chars-available callback and only call getchar once a driver announces
   input. The UART driver announces from its RX IRQ; without this hook, CDC
   keystrokes sat unread in the FIFO forever ("typed 'c', no response"). */
static void (*s_chars_avail_cb)(void *);
static void *s_chars_avail_param;

static void cdc_set_chars_available_callback(void (*fn)(void *), void *param)
{
    s_chars_avail_cb = fn;
    s_chars_avail_param = param;
}

static stdio_driver_t s_cdc_stdio = {
    .out_chars = cdc_out_chars,
    .in_chars  = cdc_in_chars,
    .set_chars_available_callback = cdc_set_chars_available_callback,
#if PICO_STDIO_ENABLE_CRLF_SUPPORT
    .crlf_enabled = PICO_STDIO_DEFAULT_CRLF,
#endif
};

void usb_serial_init(void)
{
    stdio_set_driver_enabled(&s_cdc_stdio, true);

    /* UART for the mirror + console input (SDK stdio_uart stays disabled). */
    uart_init(uart_default, 115200);
    gpio_set_function(PICO_DEFAULT_UART_TX_PIN, GPIO_FUNC_UART);
    gpio_set_function(PICO_DEFAULT_UART_RX_PIN, GPIO_FUNC_UART);
}

/* Drain the ring into the CDC FIFO — called right after tud_task() so it runs
   in the same context as the rest of the USB stack. */
void usb_serial_task(void)
{
    /* UART mirror pump FIRST — it must run even with no CDC terminal
       attached (that's exactly when the UART is the console). Fills the
       32-byte TX FIFO and leaves — never waits; at this 500 us cadence the
       FIFO gates the pace to the line rate. */
    if (settings()->usbset & USBSET_UART) {
        while (s_utxr_r != s_utxr_w && uart_is_writable(uart_default)) {
            uart_get_hw(uart_default)->dr = s_utxr[s_utxr_r];
            s_utxr_r = (s_utxr_r + 1) & (UTXR_SZ - 1);
        }
        /* UART keystrokes must be announced BEFORE the CDC-connected early
           return — the UART console matters most when no CDC terminal is
           attached. (btstack's callback just pokes the run loop — IRQ-safe.) */
        if (s_chars_avail_cb && uart_is_readable(uart_default)) {
            s_chars_avail_cb(s_chars_avail_param);
        }
    } else {
        s_utxr_r = s_utxr_w;             /* console off: keep the ring fresh */
        (void)uart_get_hw(uart_default)->dr;   /* and the RX FIFO drained    */
    }

    if (!tud_cdc_connected()) {
        /* No terminal attached: keep the ring fresh instead of stalling on a
           full buffer forever (DTR-less boots would pin old logs). */
        uint32_t save = save_and_disable_interrupts();
        s_txr_r = s_txr_w;
        restore_interrupts(save);
        return;
    }

    bool wrote = false;
    while (s_txr_r != s_txr_w && tud_cdc_write_available() > 0) {
        uint32_t r = s_txr_r;
        uint32_t chunk = (s_txr_w >= r) ? (s_txr_w - r) : (TXR_SZ - r);
        uint32_t room = tud_cdc_write_available();
        if (chunk > room) chunk = room;
        uint32_t n = tud_cdc_write(&s_txr[r], chunk);
        s_txr_r = (r + n) & (TXR_SZ - 1);
        wrote = wrote || (n > 0);
        if (n < chunk) break;
    }
    if (wrote) {
        tud_cdc_write_flush();
    }

    /* Announce pending input (btstack's callback is IRQ-safe by contract —
       it just pokes the run loop to poll). */
    if (s_chars_avail_cb && tud_cdc_available() > 0) {
        s_chars_avail_cb(s_chars_avail_param);
    }
}

/* picotool-style reset: host opens the port at 1200 baud -> BOOTSEL. */
void tud_cdc_line_coding_cb(uint8_t itf, cdc_line_coding_t const *coding)
{
    (void)itf;
    if (coding->bit_rate == 1200) {
        reset_usb_boot(0, 0);
    }
}
