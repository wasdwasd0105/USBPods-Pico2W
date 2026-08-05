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

/* Overridable: a build may pre-define both (compile definitions) and these
   defaults step aside — the values below are this tree's own version. */
#ifndef USBPODS_VERSION_NAME
#define USBPODS_VERSION_NAME  "1.0.0"
#endif
#ifndef USBPODS_VERSION_CODE
#define USBPODS_VERSION_CODE  1
#endif
