/* usbpods UNIVERSAL board: ONE binary for both supported dongles —
 *   - Raspberry Pi Pico 2 W          (RP2350A, RM2 on GPIO 23/24/25/29)
 *   - Waveshare RP2350B-Plus-W       (RP2350B, RM2 on GPIO 36/37/38/39)
 *
 * HOW: compiled as RP2350B (the 48-GPIO superset — the A-package die has
 * the same GPIO controllers, merely unbonded, so touching high pins on a
 * Pico 2 W is harmless) with CYW43_PIN_WL_DYNAMIC: main() PROBES the radio
 * at the Pico pins first, then the Waveshare pins (a wrong-pin init fails
 * and returns — HW-proven), and caches the winner in settings so later
 * boots go straight to the right pins.
 *
 * Flash is mapped at the 4 MB common denominator: identical settings /
 * link-key layout on both boards (the Waveshare's 16 MB chip simply has
 * the top 12 MB unused).
 */
#ifndef _BOARDS_USBPODS_UNIVERSAL_H
#define _BOARDS_USBPODS_UNIVERSAL_H

// pico_cmake_set PICO_PLATFORM=rp2350
// pico_cmake_set PICO_CYW43_SUPPORTED = 1

#define USBPODS_UNIVERSAL
#define PICO_RP2350A 0                 /* B superset; runs on A-package too */

#ifndef PICO_DEFAULT_UART
#define PICO_DEFAULT_UART 0
#endif
#ifndef PICO_DEFAULT_UART_TX_PIN
#define PICO_DEFAULT_UART_TX_PIN 0     /* same on both boards */
#endif
#ifndef PICO_DEFAULT_UART_RX_PIN
#define PICO_DEFAULT_UART_RX_PIN 1
#endif

#define PICO_BOOT_STAGE2_CHOOSE_W25Q080 1
#ifndef PICO_FLASH_SPI_CLKDIV
#define PICO_FLASH_SPI_CLKDIV 2
#endif
// pico_cmake_set_default PICO_FLASH_SIZE_BYTES = (4 * 1024 * 1024)
#ifndef PICO_FLASH_SIZE_BYTES
#define PICO_FLASH_SIZE_BYTES (4 * 1024 * 1024)   /* common denominator */
#endif

#ifndef PICO_RP2350_A2_SUPPORTED
#define PICO_RP2350_A2_SUPPORTED 1
#endif

/* ---- CYW43 radio: DYNAMIC pins, probed at boot ---------------------------- */
#define CYW43_PIN_WL_DYNAMIC 1
/* Seeds only — main()'s probe overrides via cyw43_set_pins_wl(). */
#ifndef CYW43_DEFAULT_PIN_WL_REG_ON
#define CYW43_DEFAULT_PIN_WL_REG_ON 23u
#endif
#ifndef CYW43_DEFAULT_PIN_WL_DATA_OUT
#define CYW43_DEFAULT_PIN_WL_DATA_OUT 24u
#endif
#ifndef CYW43_DEFAULT_PIN_WL_DATA_IN
#define CYW43_DEFAULT_PIN_WL_DATA_IN 24u
#endif
#ifndef CYW43_DEFAULT_PIN_WL_HOST_WAKE
#define CYW43_DEFAULT_PIN_WL_HOST_WAKE 24u
#endif
#ifndef CYW43_DEFAULT_PIN_WL_CLOCK
#define CYW43_DEFAULT_PIN_WL_CLOCK 29u
#endif
#ifndef CYW43_DEFAULT_PIN_WL_CS
#define CYW43_DEFAULT_PIN_WL_CS 25u
#endif

#ifndef CYW43_WL_GPIO_COUNT
#define CYW43_WL_GPIO_COUNT 3
#endif
#ifndef CYW43_WL_GPIO_LED_PIN
#define CYW43_WL_GPIO_LED_PIN 0        /* RM2 GPIO0 LED — both boards */
#endif
#ifndef CYW43_USES_VSYS_PIN
#define CYW43_USES_VSYS_PIN 0          /* VSYS sense differs per board — unused */
#endif

#endif
