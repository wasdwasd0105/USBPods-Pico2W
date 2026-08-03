# Local pico-sdk patches — LOAD-BEARING, verify before building

The firmware **requires** local modifications to the shared pico-sdk at
`$PICO_SDK_PATH` (normally `~/.pico-sdk/sdk/2.1.1`). A fresh SDK clone
compiles almost clean but produces **silently wrong wire behaviour** — worst
case, stock BTstack **disconnects the ACL on every flush event**, turning the
manual jam-breaker flush into a link killer. `CMakeLists.txt` fails the
configure step with a pointer here if the patches are missing.

## Apply

```bash
cd $PICO_SDK_PATH/lib/btstack  && git apply /path/to/usbpods-pico2w/sdk-patches/btstack-usbpods.patch
cd $PICO_SDK_PATH/lib/tinyusb  && git apply /path/to/usbpods-pico2w/sdk-patches/tinyusb-usbpods.patch
```

## Regenerate (after editing the SDK)

```bash
git -C $PICO_SDK_PATH/lib/btstack diff > sdk-patches/btstack-usbpods.patch
git -C $PICO_SDK_PATH/lib/tinyusb diff > sdk-patches/tinyusb-usbpods.patch
```

## What the patches do and why each is required

### btstack-usbpods.patch
- **`src/hci.c` — flush ≠ death.** Stock BTstack treats `HCI_EVENT_FLUSH_OCCURRED`
  as a link watchdog and sets `SEND_DISCONNECT`. macOS semantics: a flush is the
  200 ms timeout doing its job on a degraded link. Without this patch, any flush
  (automatic or our manual jam-breaker fallback) drops the whole connection.
  The event is counted ONCE in `src/btstack/btstack_hci.c` (an earlier revision
  also counted here — every event then read as 2, which produced the
  "+2 events per manual flush" misinterpretation in `cyw43-flush-defect.md`).
- **`src/l2cap.c` + `src/l2cap.h` — per-CID flushability (macOS parity).**
  Adds `l2cap_set_flushable_local_cid()`. Only the registered channel (the AVDTP
  media CID, registered at STREAMING_CONNECTION_ESTABLISHED) is sent PB=0b10
  auto-flushable; signaling/AACP stay PB=0b00 so a fading link never flushes
  reliable protocol traffic. `usbpods_flushable_tx_count` increments on every
  marked packet — **if it reads 0 while streaming, the registration is missing
  and the auto-flush timer cannot apply to media at all** (this exact mis-marking
  invalidated the 4ae498d "CYW43 never arms the timer" measurements — see
  `review-4ae498d-findings.md`).
- **`src/classic/avdtp_source.c` — RTP SSRC = 0.** macOS sends SSRC 0 always
  (wire-verified over 3732 packets); stock BTstack sends 0x11223344.

### tinyusb-usbpods.patch
- **`src/portable/raspberrypi/rp2040/rp2040_usb.c`** — comments out the
  `panic("ep %02X was already available")` in
  `_hw_endpoint_buffer_control_update32`: host-side EP churn (Windows driver
  install, profile switches) can re-prime a live endpoint; crashing the dongle
  for it is worse than tolerating the re-prime.

### cyw43-driver
No patch — the BT firmware blob must be **stock 2.1.1** (`0031`). A newer blob
(`0065`, from pico-sdk 2.3.0) was swapped in during the 4ae498d investigation
and has been reverted; that A/B is superseded by the PB mis-marking finding
anyway (`review-4ae498d-findings.md`).
