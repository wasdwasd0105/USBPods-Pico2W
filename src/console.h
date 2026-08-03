/*
 * console.h — the USBPods serial console (the UI of the no-web-UI build).
 *
 * Owns key dispatch and every user-facing screen. Advanced/debug keys are
 * forwarded to the module that owns the state they poke
 * (a2dp_source_console_key), so this file stays free of protocol internals.
 */
#ifndef USBPODS_CONSOLE_H
#define USBPODS_CONSOLE_H

#include <stdbool.h>

void console_init(void);        /* registers the key handler (call at startup) */
void console_key(char cmd);     /* one keystroke from the console transport    */
void console_show_menu(void);   /* the user menu ('h')                         */

#endif /* USBPODS_CONSOLE_H */
