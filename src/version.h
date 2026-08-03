/* usbpods firmware version — SINGLE SOURCE OF TRUTH.
 *
 * Android-style two-part scheme:
 *   USBPODS_VERSION_NAME  human-readable semver "MAJOR.MINOR.PATCH"
 *                         MAJOR: breaking/protocol change, MINOR: features,
 *                         PATCH: fixes only.
 *   USBPODS_VERSION_CODE  monotonic integer, bump on EVERY release, never
 *                         reuse or decrease — machines compare this one
 *                         (the web UI uses it for old-firmware hints).
 *
 * CMakeLists.txt greps these two lines for picotool binary info — keep the
 * "#define NAME value" single-line shape.
 */
#pragma once

#define USBPODS_VERSION_NAME  "1.0.0"
#define USBPODS_VERSION_CODE  1
