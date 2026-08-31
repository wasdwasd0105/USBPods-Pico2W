#include "pico/cyw43_arch.h"
#include "pico/stdio_uart.h"     /* early-boot synchronous UART console */
#include "pico/cyw43_driver.h"   /* cyw43_set_pins_wl (universal radio probe) */
#include "pico/stdlib.h"
#include "btstack_run_loop.h"

#include "btstack/btstack_avdtp_source.h"
#include "btstack/btstack_hci.h"
#include "btstack/btstack_sink_relay.h"

#include <stdio.h>
#include "pico/stdlib.h"

#include "pico/multicore.h"

#include "btstack_event.h"

#include "hardware/flash.h"
#include "hardware/gpio.h"
#include "hardware/sync.h"
#include "hardware/structs/ioqspi.h"
#include "hardware/structs/sio.h"
#include "hardware/clocks.h"
#include "hardware/vreg.h"
#include "hardware/timer.h"
#include "hardware/structs/ioqspi.h"
#include "hardware/structs/sio.h"
#include "hardware/watchdog.h"
#include "pico/bootrom.h"   /* rom_func_lookup_inline (safe yank) */
#include "hardware/structs/qmi.h"   /* per-board QSPI divider (Waveshare) */
#include "hardware/structs/xip_ctrl.h" /* cache hit counters (probe) */

#include "tinyusb/uac.h"
#include "pico_w_led.h"
#include "settings.h"
#include "version.h"
#include "pico/flash.h"


/* Boot2 re-entry WITHOUT a cache flush — see SAFE-YANK below. On RP2350
   the callable flash-setup code lives at BOOTRAM_BASE (the ROM copies it
   there at boot) — NOT at XIP_BASE like the RP2040 (calling the flash
   image head executed metadata as code: UNDEFINSTR bootloop). Same source
   the SDK's own flash_init_boot2_copyout uses. */
static uint32_t boot2_ram_copy[64];
static void safe_yank_init(void) {
    memcpy(boot2_ram_copy, (const void *)BOOTRAM_BASE, 256);
}
static void __no_inline_not_in_flash_func(xip_mode_restore)(void) {
    rom_connect_internal_flash_fn ci =
        (rom_connect_internal_flash_fn)rom_func_lookup_inline(ROM_FUNC_CONNECT_INTERNAL_FLASH);
    rom_flash_exit_xip_fn ex =
        (rom_flash_exit_xip_fn)rom_func_lookup_inline(ROM_FUNC_FLASH_EXIT_XIP);
    ci();
    ex();
    ((void (*)(void))((uintptr_t)boot2_ram_copy + 1))();   /* thumb bit */
}

void qmi_regs_dump(void) {
    printf("[qmi] m0 timing=0x%08lx rcmd=0x%08lx rfmt=0x%08lx\n",
           (unsigned long)qmi_hw->m[0].timing,
           (unsigned long)qmi_hw->m[0].rcmd,
           (unsigned long)qmi_hw->m[0].rfmt);
    uint32_t hit = xip_ctrl_hw->ctr_hit, acc = xip_ctrl_hw->ctr_acc;
    printf("[xip] ctrl=0x%08lx hit=%lu acc=%lu (%lu%% hit since last dump)\n",
           (unsigned long)xip_ctrl_hw->ctrl, (unsigned long)hit, (unsigned long)acc,
           (unsigned long)(acc ? (uint64_t)hit * 100 / acc : 0));
    xip_ctrl_hw->ctr_hit = 0;
    xip_ctrl_hw->ctr_acc = 0;
}

static bool __no_inline_not_in_flash_func(get_bootsel_button)() {
    const uint CS_PIN_INDEX = 1;

    // Must disable interrupts, as interrupt handlers may be in flash, and we
    // are about to temporarily disable flash access!
    uint32_t flags = save_and_disable_interrupts();

    // Set chip select to Hi-Z
    hw_write_masked(&ioqspi_hw->io[CS_PIN_INDEX].ctrl,
                    GPIO_OVERRIDE_LOW << IO_QSPI_GPIO_QSPI_SS_CTRL_OEOVER_LSB,
                    IO_QSPI_GPIO_QSPI_SS_CTRL_OEOVER_BITS);

    // Note we can't call into any sleep functions in flash right now
    for (volatile int i = 0; i < 1000; ++i);

    // The HI GPIO registers in SIO can observe and control the 6 QSPI pins.
    // Note the button pulls the pin *low* when pressed.
#if PICO_RP2040
    #define CS_BIT (1u << 1)
#else
    #define CS_BIT SIO_GPIO_HI_IN_QSPI_CSN_BITS
#endif
    bool button_state = !(sio_hw->gpio_hi_in & CS_BIT);

    // Need to restore the state of chip select, else we are going to have a
    // bad time when we return to code in flash!
    hw_write_masked(&ioqspi_hw->io[CS_PIN_INDEX].ctrl,
                    GPIO_OVERRIDE_NORMAL << IO_QSPI_GPIO_QSPI_SS_CTRL_OEOVER_LSB,
                    IO_QSPI_GPIO_QSPI_SS_CTRL_OEOVER_BITS);

    /* SAFE-YANK: the CS glitch can knock the flash out of continuous-read
       mode — the next XIP fetch is then GARBAGE and the CPU parks in a junk
       loop (hang forensics: spin "inside aacEncEncode", IRQs alive).
       Re-establish the mode WITHOUT touching the XIP cache: flash content
       is unchanged, so the cached lines stay valid. (First attempt used
       flash_do_cmd — its cache FLUSH every 20 ms evicted the FDK decoder's
       whole table set 50x/s: relay decode collapsed to ~15% throughput on
       BOTH boards. Never flush here.) */
    xip_mode_restore();

    restore_interrupts(flags);

    return button_state;
}

static uint8_t btn_mode(void){
    uint8_t m = settings()->btn_mode;
    return (m >= 1 && m <= 4) ? m : 1;   /* 0/garbage = default vol mode */
}

void on_single_press(void){
    printf("key pressed short (single tap)!\n");
    if (!get_a2dp_connected_flag()) {
        a2dp_source_wedge_clear();   /* button press = user intent; on a
                                        headless dongle this is the ONLY way
                                        back from the wedge brake */
        a2dp_source_reconnect();
        return;
    }
    switch (btn_mode()) {
        case 1: increase_vol_by_key(); break;
        case 2: {   /* the 'k' console key as a button: suspend -> fresh
                       seq/ts/encoder -> restart (AirPods mute recovery) */
            extern void a2dp_source_apply_eld_rate(void);
            printf("button: stream reset\n");
            a2dp_source_apply_eld_rate();
            break;
        }
        case 3: usb_hid_consumer(0x00CD); break;   /* play/pause to the host */
        default: break;                            /* 4: none */
    }
}

void on_double_press(void){
    if(!get_allow_switch_slot()){
        decrease_vol_by_key();
        return;
    }
    uint8_t currect_slot = read_uint8_last_flash();
    if (currect_slot == 0x1){
        printf("switch to slot 2!\n");
        led_set_mode(LED_CONN_SLOT2);
        write_uint8_last_flash(0x2);
    } else{
        printf("switch to slot 1!\n");
        led_set_mode(LED_CONN_SLOT1);
        write_uint8_last_flash(0x1);
    }
    get_link_keys();
    printf("key pressed double!\n");
}

void on_long_press(void){
    printf("key pressed long!\n");
    if (get_a2dp_connected_flag()) {
        /* NEVER disconnect/pair from a long press while connected — an
           accidental hold used to wipe the slot and start scanning.
           Mode 1 uses it as volume-down; other modes ignore it. */
        if (btn_mode() == 1) decrease_vol_by_key();
        return;
    }
    avdtp_disconnect_and_scan();
}

// ---------- parameters ----------
#define DEBOUNCE_US          5000          // 5 ms
#define SHORT_PRESS_MIN_US  100000         // 100 ms
#define LONG_PRESS_MIN_US  1000000         // 1  s
#define DOUBLE_WINDOW_US    500000         // 500 ms

// ---------- state ----------
static bool     debounced_state  = false;   // last stable level
static uint64_t last_edge_us     = 0;       // last time level changed
static uint64_t press_start_us   = 0;       // when current press began
static uint64_t last_release_us  = 0;       // when previous press ended
static uint8_t  tap_counter      = 0;

// call every 1 ms
void check_bootsel_state(void)
{
    bool raw = get_bootsel_button();            // true == pressed
    uint64_t now = time_us_64();

    // --- debounce ---
    if (raw != debounced_state) {
        if (now - last_edge_us >= DEBOUNCE_US) {
            debounced_state = raw;
            last_edge_us = now;

            if (debounced_state) {              // -------- press --------
                press_start_us = now;
            } else {                            // -------- release ------
                uint64_t press_len = now - press_start_us;

                // classify type of this press
                if (press_len >= LONG_PRESS_MIN_US) {
                    on_long_press();
                    tap_counter = 0;            // long press stands alone
                } else if (press_len >= SHORT_PRESS_MIN_US) {
                    tap_counter++;
                    if (tap_counter == 1) {
                        last_release_us = now;  // start double-tap window
                    } else if (tap_counter == 2 &&
                               (now - last_release_us) <= DOUBLE_WINDOW_US) {
                        on_double_press();
                        tap_counter = 0;
                    }
                }
            }
        }
    }

    // timeout: single tap
    if (tap_counter == 1 && (now - last_release_us) > DOUBLE_WINDOW_US) {
        on_single_press();
        tap_counter = 0;
    }
}


/* scratch[7]: WHICH IRQ-side handler was live when the watchdog fired
   (0xN0 = entered, 0xNF = exited cleanly). Phase-5 hangs are IRQ-side —
   this narrows them to a handler. */
bool usb_timer_callback(repeating_timer_t *rt){
    /* NO scratch[7] here: this IRQ preempts the low-priority BT worker every
       500 us and would MASK a spinning BT handler's marker (it did — every
       verdict read 0x1F). USB liveness is scratch[3]'s job. */
    watchdog_hw->scratch[3]++;   /* ticks since the main loop last ran: the
                                    loop zeroes this each pass. On a watchdog
                                    ~4000 = IRQs were ALIVE while the loop
                                    starved (stuck in thread code); ~0 = ALL
                                    interrupts dead (hard stall / IRQs left
                                    disabled). */
    tinyusb_task();
    return true;
}

bool bootsel_timer_callback(repeating_timer_t *rt) {
    (void)rt;
    check_bootsel_state();
    return true;    // keep repeating
}

// Crash forensics (hardened port from the main tree): a hard fault stores
// the faulting PC/LR/CFSR in watchdog scratch and reboots; the next boot
// prints them for addr2line. Marker is written FIRST so even a double-fault
// (bogus SP) still reports "HARD FAULT".
#define CRASH_MARK_HARDFAULT 0xDEADFA17u
#define CRASH_MARK_PANIC     0xDEADFA1Cu

/* Panic forensics: PICO_PANIC_FUNCTION routes every SDK/TinyUSB panic()
   here. The stock path just breakpoints (= silent HARD FAULT report with
   PC/LR inside panic itself — useless). Instead stash WHO panicked (the
   SDK shim pushes the caller's LR at [sp]), the format string (flash
   pointer: still readable NEXT boot) and the first vararg, then reboot.
   No printf here — panic can fire in IRQ context where nothing flushes. */
void __attribute__((noreturn, used)) usbpods_panic_c(const char *fmt, uint32_t caller, uint32_t arg0) {
    watchdog_hw->scratch[0] = CRASH_MARK_PANIC;
    watchdog_hw->scratch[1] = caller;
    watchdog_hw->scratch[2] = (uint32_t)(uintptr_t)fmt;
    watchdog_hw->scratch[4] = arg0;
    hw_set_bits(&watchdog_hw->ctrl, WATCHDOG_CTRL_TRIGGER_BITS);
    while (1) tight_loop_contents();
}
void __attribute__((naked, noreturn)) usbpods_panic(const char *fmt, ...) {
    pico_default_asm_volatile(
        "mov r2, r1\n"       /* first vararg (e.g. the ep address)      */
        "ldr r1, [sp]\n"     /* panic()'s pushed {lr} = the REAL caller */
        "b   usbpods_panic_c\n"
    );
}

/* Reset-reason stash: boot prints vanish in the CDC reconnect window, so
   the 'R' debug dump re-prints these. 0 clean, 1 watchdog, 2 hard fault. */
uint32_t last_fault_pc, last_fault_lr, last_fault_cfsr;
uint8_t  last_reset_kind;
uint8_t  last_hang_phase;   /* scratch[6] snapshot: WHERE the loop starved */
uint8_t  last_hang_irq;     /* scratch[7]: 0x10 usb-task 0x20 bootsel 0x30 pump (0xNF = clean exit) */
uint32_t last_hang_ticks;   /* scratch[3]: usb-IRQ ticks during the starvation window */

void __not_in_flash_func(hardfault_c)(uint32_t *sp) {
    watchdog_hw->scratch[0] = CRASH_MARK_HARDFAULT;
    /* Only read the frame if the faulting SP is plausibly in SRAM — after a
       runaway overflow it may point outside, and faulting HERE would loop
       the handler forever instead of rebooting. */
    if ((uintptr_t)sp >= 0x20000000u && (uintptr_t)sp <= 0x20081F00u) {
        watchdog_hw->scratch[1] = sp[6];   /* stacked PC */
        watchdog_hw->scratch[2] = sp[5];   /* stacked LR */
    }
    watchdog_hw->scratch[4] = (*(volatile uint32_t *)0xE000ED28u & 0x0FFFFFFFu) |
                              (*(volatile uint32_t *)0xD0000000u << 28);
    hw_set_bits(&watchdog_hw->ctrl, WATCHDOG_CTRL_TRIGGER_BITS);
    while (1) tight_loop_contents();
}

/* SRAM-resident entry stub. It must CLEAR MSPLIM and PIVOT to a dedicated
   emergency stack: with MSPLIM armed (see main), a stack-overflow fault
   would otherwise re-fault on the handler's own pushes below the limit ->
   recursive descent -> LOCKUP with no report. */
uint32_t crash_stack[64] __attribute__((aligned(8)));
uint32_t *const crash_stack_top = &crash_stack[64];

void __attribute__((naked)) __not_in_flash_func(isr_hardfault)(void) {
    __asm volatile(
        "movs r1, #0      \n"
        "msr  MSPLIM, r1  \n"
        "mrs  r0, msp     \n"   /* faulting frame pointer (may be garbage)  */
        "ldr  r2, =crash_stack_top \n"
        "ldr  r2, [r2]    \n"
        "mov  sp, r2      \n"   /* emergency stack: pushes can't fault      */
        "ldr  r2, =hardfault_c \n"
        "bx   r2          \n");
}

static bool radio_absent;   /* universal build: no radio found — BT disabled */
/* Plugged into a plain Pico 2 (no CYW43): USB audio enumerates and the console
   works, but there is no Bluetooth at all. Surfaced so the web UI can say so
   instead of showing a dongle that simply never connects to anything. */
bool radio_is_absent(void) { return radio_absent; }

int main() {

    /* FIRST: disarm any watchdog left ticking by the path that got us here.
       The PICOBOOT REBOOT2 (web-flasher update) reboots via a short-period
       watchdog — on the Waveshare it stayed armed across the reset and
       kept firing before boot finished: "bootloop after web update, fine
       after manual reset". A power-on/manual reset clears it, this line
       makes every boot equivalent. */
    hw_clear_bits(&watchdog_hw->ctrl, WATCHDOG_CTRL_ENABLE_BITS);

    /* crt0 zeroes MSPLIM — so the old 4 KB stack overflowed SILENTLY (or
       fatally: the parked-AAC hardfault) under FDK decode in the BT IRQ.
       Stack is now 24 KB at top of RAM (memmap_bigstack.ld); arm the
       hardware stack limit so any future overflow faults loudly. */
    extern char __StackBottom;
    __asm volatile ("msr msplim, %0" :: "r" (&__StackBottom));

    vreg_set_voltage(VREG_VOLTAGE_1_20);
    sleep_ms(100);
    set_sys_clock_khz(250000, true);

    /* EARLY synchronous UART console (GPIO 0/1 @ 115200) — the only window
       into boots that die before USB on SWD-less boards (Waveshare). It
       prints BLOCKING from here on; once the non-blocking USB console is
       up we unhook it (the diag-print-poisoning lesson) and the runtime
       UART mirror takes over per the USBSET_UART setting. Initialized
       AFTER the sys-clock switch so the baud divider is computed against
       the final 250 MHz clocks. */
    safe_yank_init();   /* boot2 RAM copy for the button poll's XIP restore */
    stdio_uart_init();
    printf("\n[boot] early UART console up\n");

    // stdio: USB CDC only (usb_serial.c ring, non-blocking, microseconds).
    // UART stdio is REMOVED from the printf path: it blocks ~87us/char at
    // 115200, so one stats line stalled the whole BT/audio context (which
    // shares core 0 with the radio transport) for ~10-25 ms — audible as
    // random bursts in the headset whenever anything printed (the ESP32
    // "diag-print poisoning" lesson, Pico edition). Re-enable by restoring
    // stdout_uart_init() if USB-dead debugging ever needs it.
    stdio_init_all();

    /* Boot-time prints are LOST in the CDC reconnect window (every -bk boot
       log starts at "init ctw43.") — stash the report in globals too, and
       re-print it in the 'R' debug dump where it can actually be read. */
    if (watchdog_caused_reboot()) {
        if (watchdog_hw->scratch[0] == CRASH_MARK_HARDFAULT) {
            last_reset_kind = 2;
            last_fault_pc   = watchdog_hw->scratch[1];
            last_fault_lr   = watchdog_hw->scratch[2];
            last_fault_cfsr = watchdog_hw->scratch[4];
            printf("!!! last reset: HARD FAULT at PC=0x%08lx LR=0x%08lx CORE %lu CFSR=0x%07lx (addr2line -e build-universal/USBPods_Pico2W.elf)\n",
                   (unsigned long)last_fault_pc,
                   (unsigned long)last_fault_lr,
                   (unsigned long)(last_fault_cfsr >> 28),
                   (unsigned long)(last_fault_cfsr & 0x0FFFFFFFu));
        } else if (watchdog_hw->scratch[0] == CRASH_MARK_PANIC) {
            last_reset_kind = 3;
            last_fault_pc   = watchdog_hw->scratch[1];   /* panic caller */
            last_fault_lr   = watchdog_hw->scratch[2];   /* fmt (flash)  */
            last_fault_cfsr = watchdog_hw->scratch[4];   /* first vararg */
            const char *pm = (const char *)(uintptr_t)last_fault_lr;
            if (last_fault_lr < 0x10000000u || last_fault_lr >= 0x10400000u)
                pm = "(fmt not in flash)";
            printf("!!! last reset: PANIC from 0x%08lx arg0=0x%lx: \"%s\" (addr2line the from-address)\n",
                   (unsigned long)last_fault_pc, (unsigned long)last_fault_cfsr, pm);
        } else {
            last_reset_kind = 1;
            last_hang_phase = (uint8_t)watchdog_hw->scratch[6];
            last_hang_irq   = (uint8_t)watchdog_hw->scratch[7];
            last_hang_ticks = watchdog_hw->scratch[3];
            printf("!!! last reset: WATCHDOG (loop starved >2s, phase %u, irq 0x%02x, "
                   "usb_irq_ticks %lu — ~4000: IRQs alive/thread stuck; ~0: all IRQs dead)\n",
                   last_hang_phase, last_hang_irq, (unsigned long)last_hang_ticks);
        }
    } else {
        watchdog_hw->scratch[5] = 0;   /* power-on/flash: fresh radio-probe chain */
    }
    watchdog_hw->scratch[0] = 0;
    watchdog_hw->scratch[1] = 0;
    watchdog_hw->scratch[2] = 0;
    watchdog_hw->scratch[4] = 0;

    flash_safe_execute_core_init();

    settings_init();   /* journaled store: load or migrate the legacy pages */

    {   /* device-bound license: verify blob vs live HW ids (~100 ms when a
           blob is present, instant otherwise). Never persisted as a bool. */
        extern void license_init(void);
        extern void license_unbind_boot_check(bool);
        license_init();
        /* unbind handshake: a genuine power-cycle (replug) confirms an armed
           unbind. watchdog_caused_reboot() is true for our soft reboots, so a
           script cannot fake the replug by rebooting the device. */
        license_unbind_boot_check(!watchdog_caused_reboot());
    }

    uint8_t currect_slot = read_uint8_last_flash();

    printf("init slot is %d\n", currect_slot);

    if (currect_slot != 0x1 && currect_slot != 0x2){
        printf("Doesn't has slot, set the slot to 1\n");
        write_uint8_last_flash(0x1);
        currect_slot = read_uint8_last_flash();
    }

    /* USB personality order must be latched BEFORE the USB stack comes up. */
    extern void usb_desc_set_uac1_first(bool on);
    usb_desc_set_uac1_first((read_settings_byte() & USBSET_UAC1) != 0);
    /* Connect Mode 2 boots without the audio function — no headset can be
       connected yet, and this skips the replug right after enumeration. */
    extern void usb_desc_set_audio_exposed(bool on);
    usb_desc_set_audio_exposed(settings()->connmode != 2);

    tinyusb_main();
    usb_serial_init();   /* printf -> USB CDC too */
    /* Service USB from HERE, not after the radio probe. tusb_init() only sets
       the stack up; nothing enumerates until tud_task runs, and usb_serial_task
       (the non-blocking UART mirror) rides the same timer. The probe sits
       between the two, so on a board with no CYW43 it hung with USB unserviced
       AND the console undrained: a Pico 2 (non-W) enumerated nothing at all and
       the log died at "init slot is 1" with every print after it stuck in the
       ring. Arming first means USB and the console survive whatever the radio
       does. */
    static repeating_timer_t usb_timer;
    add_repeating_timer_us(-500, usb_timer_callback, NULL, &usb_timer);
    stdio_set_driver_enabled(&stdio_uart, false);   /* blocking early console off:
                                                       the non-blocking mirror owns
                                                       the UART from here (early
                                                       boot prints stay on UART) */
    printf("[boot] switched to USB console + UART mirror\n");
    qmi_regs_dump();   /* BOOT-time flash mode — compare vs live in 'r' */
    {   /* one-time JEDEC ID: WHICH flash chip is this board carrying?
           (9Fh: manufacturer, memory type, capacity). Identifies whether
           the chip supports quad + whose QE-bit convention it uses. */
        uint8_t tx[4] = { 0x9F, 0, 0, 0 }, rx[4] = { 0 };
        flash_do_cmd(tx, rx, 4);
        printf("[flash] JEDEC %02x %02x %02x\n", rx[1], rx[2], rx[3]);

        /* ONE-TIME QE FIX (root cause of the slow Waveshare): its Puya
           P25Q128 (mfr 0x85) ships with the quad-enable bit UNPROGRAMMED,
           so the boot path's quad setup fails and the ROM falls back to
           dual 0xBB with a command prefix per access — ~4x slower per XIP
           miss, which the relay decoder's ~45M-miss thrash turns into
           130 ms/frame (Pico's Winbond ships QE=1: quad 0xEB, no issue).
           QE lives in non-volatile SR2 bit 1: set it ONCE (persists in the
           chip), then reboot so the quad probe succeeds. Runs only when
           the bit is actually clear — one reboot ever per chip. */
        if (rx[1] == 0x85) {
            uint8_t sr2_tx[2] = { 0x35, 0 }, sr2_rx[2] = { 0 };
            flash_do_cmd(sr2_tx, sr2_rx, 2);
            printf("[flash] Puya SR2=0x%02x (QE=%d)\n", sr2_rx[1], (sr2_rx[1] >> 1) & 1);
            if (!(sr2_rx[1] & 0x02)) {
                uint8_t wren[1] = { 0x06 }, wren_rx[1];
                uint8_t wrsr2[2] = { 0x31, (uint8_t)(sr2_rx[1] | 0x02) }, wrsr2_rx[2];
                flash_do_cmd(wren, wren_rx, 1);
                flash_do_cmd(wrsr2, wrsr2_rx, 2);
                for (int t = 0; t < 200; t++) {          /* WIP poll, <=200 ms */
                    uint8_t sr1_tx[2] = { 0x05, 0 }, sr1_rx[2] = { 0 };
                    flash_do_cmd(sr1_tx, sr1_rx, 2);
                    if (!(sr1_rx[1] & 0x01)) break;
                    busy_wait_ms(1);
                }
                flash_do_cmd(sr2_tx, sr2_rx, 2);
                printf("[flash] QE programmed, SR2 now 0x%02x — rebooting into quad mode\n",
                       sr2_rx[1]);
                if (sr2_rx[1] & 0x02) {
                    busy_wait_ms(50);                    /* let the print flush */
                    watchdog_reboot(0, 0, 100);
                    while (1) { tight_loop_contents(); }
                }
            }
        }
    }
    printf("usbpods fw %s (code %u, build %s %s)\n",
           USBPODS_VERSION_NAME, (unsigned)USBPODS_VERSION_CODE,
           __DATE__, __TIME__);

    audio_slot_queue_init();

    printf("init ctw43.\n");

#if defined(CYW43_PIN_WL_DYNAMIC) && CYW43_PIN_WL_DYNAMIC
    /* UNIVERSAL radio bring-up: ONE cyw43_arch_init per boot (the exact
       code path of the proven per-board builds — a same-boot
       deinit/re-init probe crashed into a bootloop). The cached
       board_type picks the pin set; if the LIVENESS test fails (arch
       init is lazy — only cyw43_ensure_up inside the LED write talks to
       the chip), flip the cache and REBOOT into the other personality.
       watchdog scratch[5] counts probe attempts (survives resets, clears
       on power-cycle): after both sets fail, continue WITHOUT Bluetooth
       so USB/console stay alive instead of looping forever. */
    {
        static const uint wl_pinsets[2][CYW43_PIN_INDEX_WL_COUNT] = {
            /* REG_ON, DATA_OUT, DATA_IN, HOST_WAKE, CLOCK, CS */
            { 23, 24, 24, 24, 29, 25 },   /* 1: Raspberry Pi Pico (2) W  */
            { 36, 37, 37, 37, 39, 38 },   /* 2: Waveshare RP2350B-Plus-W */
        };
        static const char *wl_names[2] = { "Pico 2 W", "Waveshare RP2350B-Plus-W" };
        /* scratch[5] = bitmask of personalities tried since POWER-ON (bit0
           pico, bit1 waveshare) — reboot-flip state lives HERE, not in
           settings, so a failed settings commit can't wedge the chain
           (HW: the flip never persisted -> same wrong pins every boot,
           and a stale counter skipped the second personality entirely). */
        uint32_t tried = watchdog_hw->scratch[5] & 0x3;
        int idx = -1;
        if (tried == 0)      idx = (settings()->board_type == 2) ? 1 : 0;
        else if (tried == 1) idx = 1;    /* pico tried -> waveshare next  */
        else if (tried == 2) idx = 0;    /* waveshare tried -> pico next  */
        if (idx >= 0) {
            cyw43_set_pins_wl((uint *)wl_pinsets[idx]);
            printf("radio: trying %s pins (tried-mask 0x%lx)\n",
                   wl_names[idx], (unsigned long)tried);
            bool radio_up = (cyw43_arch_init() == 0) &&
                            (cyw43_gpio_set(&cyw43_state, CYW43_WL_GPIO_LED_PIN, true) == 0);
            if (radio_up) {
                printf("radio UP: %s\n", wl_names[idx]);
                watchdog_hw->scratch[5] = 0;
                if (settings()->board_type != idx + 1) {
                    settings()->board_type = (uint8_t)(idx + 1);
                    settings_mark_dirty();   /* fast path for later boots */
                }
                /* NO Waveshare QSPI slowdown anymore: the hangs were the
                   CS-yank corrupting continuous-read mode (now self-healed
                   in get_bootsel_button via boot2 re-entry), NOT flash
                   speed margin. clkdiv 4 was insurance for that wrong
                   theory — and it made FDK AAC-LC relay decode thrash the
                   XIP cache at ~150 ms/frame (rpktdrop 696/824, underruns
                   63: "plays only short time between mix gates"). Full
                   clkdiv-2 speed on every board, like the Pico. */
            } else {
                watchdog_hw->scratch[5] = tried | (1u << idx);
                if ((watchdog_hw->scratch[5] & 0x3) != 0x3) {
                    printf("radio not on %s pins — rebooting to try %s\n",
                           wl_names[idx], wl_names[1 - idx]);
                    watchdog_reboot(0, 0, 100);
                    while (1) { tight_loop_contents(); }
                }
                printf("radio not found on ANY pin set — Bluetooth DISABLED this boot\n");
                radio_absent = true;
            }
        } else {
            printf("radio not found on ANY pin set — Bluetooth DISABLED this boot\n");
            printf("*** This board has NO wireless. USB audio and the console work,\n");
            printf("*** but there is no Bluetooth — USBPods needs a Pico 2 W.\n");
            radio_absent = true;
        }
    }
#else
    // initialize CYW43 driver
    if (cyw43_arch_init()) {
        printf("cyw43_arch_init() failed.\n");
        return -1;
    }
#endif

    if (!radio_absent) {
        cyw43_arch_gpio_put(CYW43_WL_GPIO_LED_PIN, 1);
        btstack_main(0, NULL);
        if (read_settings_byte() & USBSET_GAME) {   /* restore gaming mode */
            extern void aap_set_game(bool on);
            aap_set_game(true);
        }
    }
    sleep_ms(200);

    // NOTE: FDK-AAC hangs on Core 1 (likely internal global state / malloc contention)
    // Keep encoding on Core 0 for now
    // multicore_launch_core1_with_stack(core1_aaceld_encoder_loop, core1_stack, sizeof(core1_stack));

    static repeating_timer_t bootsel_timer;
    /* Button poll is safe on EVERY board now — get_bootsel_button re-enters
       XIP via boot2 after each CS yank (see SAFE-YANK above). */
    add_repeating_timer_us(20000, bootsel_timer_callback, NULL, &bootsel_timer);

    // Enable watchdog — auto-resets if main loop stops feeding it (2 seconds)
    watchdog_enable(2000, true);

    uint16_t boot_ticks = 0;
    bool boot_dialed = false;
    while (1) {
        /* scratch[6] = which step the loop died in if the watchdog fires
           (1 usb, 2 settings, 3 relay decode, 4 eld rebuild, 5 tail) —
           printed by the reset banner. */
        watchdog_update();  // feed the watchdog
        watchdog_hw->scratch[3] = 0;   /* usb-IRQ liveness window resets */
        watchdog_hw->scratch[6] = 1;
        tinyusb_control_task();
        watchdog_hw->scratch[6] = 2;
        settings_tick();    // debounced flash commits (main loop ONLY)
        { extern void license_task(void); license_task(); }  // deferred re-verify
        watchdog_hw->scratch[6] = 3;
        /* Relay AAC decode (2-4 ms FDK work per packet lives HERE, not in
           the radio IRQ). BOUNDED drain: under RF contention (dialing while
           dual-streaming) packets can arrive as fast as we decode — the old
           drain-everything loop then never exited and starved the watchdog
           ("crash after bt sink started playing", both boards). 6 packets
           (~140 ms of audio) per 50 ms pass still catches up fast; overflow
           beyond the 8-slot ring degrades to counted drops, not a reboot. */
        /* Both of these reach into BTstack/codec state that only exists once
           the radio came up. On a board with no CYW43 (a plain Pico 2) that
           state was never initialised, and the loop died here — phase 3, all
           IRQs dead, so USB never enumerated and the console mirror stopped
           mid-boot. Skip them and the board runs as a USB device that simply
           has no Bluetooth, which is what the UI now says. */
        if (!radio_absent) {
            for (int rn = 0; rn < 6 && bt_sink_relay_work(); rn++) { }
            watchdog_hw->scratch[6] = 4;
            a2dp_source_main_work();   /* deferred ELD encoder rebuild (TX stall) */
        } else {
            /* console_init() — and with it the key handler — is registered
               inside btstack_main(), which never runs on a radio-less board.
               btstack_stdin_setup() needs the run loop, so poll stdin here
               instead: without this the console is mute on exactly the board
               whose problem you are trying to diagnose. */
            extern void console_key(char cmd);
            int ch;
            while ((ch = getchar_timeout_us(0)) != PICO_ERROR_TIMEOUT)
                console_key((char) ch);
        }
        watchdog_hw->scratch[6] = 5;
        /* Auto reconnect when powered on (USB settings bit): one dial attempt
           a few seconds after boot, once BTstack has settled. */
        if (!boot_dialed && ++boot_ticks == 60) {   /* ~3 s */
            boot_dialed = true;
            if (!radio_absent &&
                (read_settings_byte() & USBSET_BOOTCONN) && !get_a2dp_connected_flag()) {
                printf("auto-connect at power-on\n");
                a2dp_source_reconnect();
            }
        }
        watchdog_hw->scratch[6] = 6;   /* 6 = inside the loop wait */
        /* sleep_ms itself is fine (the "wedged sleep" was the CS-yank spin).
           CADENCE MATTERS UNDER RELAY: capacity = drain_bound / (decode_time
           + sleep). AAC-LC decode really costs ~18 ms/pkt (dec_us avg 17935,
           max 43 ms) — at sleep 50 that is 6/(108+50) ~= 38 pkt/s, UNDER the
           phone's ~43 pkt/s (HW: rpktdrop 14%, underruns, gate cycling on
           the Pico — "aac-lc no longer stable"). Sleep 10 under relay:
           6/(108+10) ~= 51 pkt/s with margin. The earlier revert to a flat
           50 ms used ceiling math that ignored decode time — wrong. */
        sleep_ms(bt_sink_relay_streaming() ? 10 : 50);
    }

}
