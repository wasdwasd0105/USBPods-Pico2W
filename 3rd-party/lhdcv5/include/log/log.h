/* usbpods shim for Android's <log/log.h>, needed by lhdcv5BT_enc.c.
 * Errors go to the usbpods console (non-blocking CDC/UART ring); the
 * warn/info/debug tiers are compiled out — spontaneous prints during
 * streaming are audible (the diag-print poisoning lesson). */
#pragma once
#include <stdio.h>

#define ALOGE(fmt, ...) printf("[lhdc] " fmt "\n", ##__VA_ARGS__)
#define ALOGW(fmt, ...) do {} while (0)
#define ALOGI(fmt, ...) do {} while (0)
#define ALOGD(fmt, ...) do {} while (0)
