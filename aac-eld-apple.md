# Apple AAC-ELD over A2DP (Vendor Codec 0x8001)

## Overview

Apple uses AAC-ELD as a **vendor-specific A2DP codec** (not standard MPEG-2/4 AAC). It is registered under Apple's Bluetooth vendor ID `0x004C` with codec ID `0x8001`, and is carried on the normal A2DP media channel.

## AVDTP Codec Discovery

### SEP Capability (14 bytes)

AirPods advertises AAC-ELD as a non-A2DP vendor codec in its AVDTP SEP capabilities:

```
4C 00 00 00   vendor_id = 0x0000004C (Apple, little-endian)
01 80         codec_id  = 0x8001 (AAC-ELD, little-endian)
00 80         object_type bitmap = 0x0080 (AAC-ELD)
00 8C         sampling_freq bitmap (0x008 = 48kHz) | channels (0xC = mono + stereo)
00            reserved
83 E8 00      VBR=1 (bit 23), max bitrate = 256000 (bits 22..0)
```

### SET_CONFIGURATION (14 bytes)

The source sends back the selected configuration:

```
4C 00 00 00   vendor_id = 0x0000004C
01 80         codec_id  = 0x8001
00 80         object_type = AAC-ELD
00 84         freq = 48kHz (0x008) | channels = stereo (0x4)
00            reserved
83 E8 00      VBR=1, bitrate = 256000
```

Note the **`VBR = 1` bit** — the 256 kbps figure is a **ceiling, not a target**. See "Bitrate behaviour" below.

## Codec Parameters

| Parameter | Value | Notes |
|-----------|-------|-------|
| **AOT** | 39 (ER AAC ELD) | Error-Resilient AAC Enhanced Low Delay |
| **Sample rate** | 48000 Hz | |
| **Channels** | 2 (stereo) | |
| **Granule length** | 480 samples | 10 ms per frame at 48 kHz |
| **SBR** | on, **downsampled (ratio = 1)** | SBR extension runs at the core rate, unlike HE-AAC's dual-rate SBR |
| **Bitrate** | **VBR**, ~227 kbps observed average, 256 kbps ceiling | |
| **Transport** | raw Access Units | no LATM/LOAS/ADTS framing |
| **Frame rate** | 100 fps | 48000 / 480 |

Because SBR operates at ratio = 1, a host may report the device's stream as 24 kHz — that is the core coder rate, with SBR reconstructing the upper band. The frame length stays 480 samples / 10 ms.

## RTP Packet Format

Wire-verified over **3732 consecutive media packets** from a macOS → AirPods session. Standard 12-byte RTP header; invariant fields held at 100 %:

| Field | Value | Notes |
|---|---|---|
| Version | 2 | |
| Padding / Extension / CC | 0 / 0 / 0 | |
| **Marker** | **0 — never set** | not used to flag discontinuity |
| **Payload type** | **96 (0x60)** | dynamic |
| Sequence | +1 per packet | |
| **Timestamp** | **1 kHz clock (milliseconds), +30 per packet** | see below |
| **SSRC** | **0x00000000 — always** | never randomised |

### RTP timestamp is millisecond-based, not sample-based

Measured over a 58.408 s contiguous run, the timestamp advanced **58 287** units — an implied clock of **997.9 Hz ≈ 1 kHz**. A sample-based clock would have advanced 2 803 584 @ 48 kHz. Mean advance is exactly **30.00 per packet** against a 30.06 ms mean packet interval: one unit per millisecond, matching 3 × 480 samples @ 48 kHz = 30 ms.

Per-packet deltas jitter across 29/30/31/32/33 with a mean of exactly 30.00, consistent with stamping from a real millisecond clock rather than accumulating a computed sample count.

> A sample-based `timestamp += 1440` also plays correctly on AirPods — they tolerate it — but it is not what macOS sends. Worth aligning if A/V sync or jitter-buffer behaviour is ever suspect.

### Payload structure

Each RTP packet carries **3 AAC-ELD frames**, each preceded by a 4-byte Apple header:

```
+-- Frame 1 ------------------------------------------+
| Apple Header (4 bytes) | Raw AAC-ELD AU (~300 bytes) |
+-- Frame 2 ------------------------------------------+
| Apple Header (4 bytes) | Raw AAC-ELD AU (~300 bytes) |
+-- Frame 3 ------------------------------------------+
| Apple Header (4 bytes) | Raw AAC-ELD AU (~300 bytes) |
+-----------------------------------------------------+

Total payload: ~912 bytes (3 * (4 + ~300))
```

### Apple AAC-ELD Size Header (4 bytes per frame)

```
Byte 0: 0xB0 | (sequence >> 8) & 0x0F    -- marker 0xB0 + seq high 4 bits
Byte 1: sequence & 0xFF                    -- seq low 8 bits
Byte 2: 0x10 | (au_size >> 8) & 0x0F      -- marker 0x10 + size high 4 bits
Byte 3: au_size & 0xFF                     -- size low 8 bits
```

- **Sequence**: 12-bit counter (0–4095), increments **per frame**, wraps at 0xFFF
- **AU size**: 12-bit field; the practical maximum is **2047 bytes (0x7FF)**
- **Marker nibbles**: `0xB` (byte 0 high) and `0x1` (byte 2 high) are fixed identifiers

Example: `B0 0A 11 33` = sequence 10, AU size 307 bytes.

### Verified full packet layout

```
ACL   (4B)  0B 20 9F 03   handle 0x00B | PB=0b10 AUTO-FLUSHABLE | len
L2CAP (4B)  9B 03 07 04   len | CID 0x0407
RTP  (12B)  80 60 33 61 00 0D 14 0F 00 00 00 00
            V=2  PT=96  seq=0x3361  ts=857103 (ms)  SSRC=0
AU #1       B9 FA 11 2B   seq=0x9FA(2554) size=0x12B(299)  + 299 bytes AAC-ELD
AU #2       [4B hdr]      seq=2555                          + AU
AU #3       [4B hdr]      seq=2556                          + AU
```

Confirmed across all 3732 packets: header nibbles `0xB` / `0x1` at 100 %, Apple AU sequence **+3 per packet** (exactly 3 AUs), AU sizes **4–320 bytes**, ACL length always L2CAP length + 4, and the ACL packet-boundary flag **always `0b10` (automatically flushable)**.

That last one matters for link robustness — see "Link behaviour under loss".

## Packet Timing

| Metric | Value |
|--------|-------|
| Frames per packet | 3 |
| Samples per frame | 480 |
| Samples per packet | 1440 |
| Packet interval | 30 ms |
| Packets per second | ~33 |
| Payload per packet | ~912 bytes |
| Effective bitrate (with headers) | ~250 kbps |

## Bitrate behaviour

**The steady state is VBR**, confirmed three independent ways:

1. The AVDTP `SET_CONFIGURATION` carries **`VBR = 1`**, with 256000 as a *maximum*.
2. Measured AU sizes vary continuously — **4–320 bytes** across 3732 wire-captured packets.
3. Frame-size sampling gives min 278 / max 320 / avg 303 bytes, ~227 kbps average — not constant.

**macOS does not reduce its bitrate when the link degrades.** Measured directly: during 30 s at roughly 70 % packet loss with repeated total audio dropout, encoder output was completely flat —

| 10 s window | AU count | mean AU size | min | max |
|---|---|---|---|---|
| clean | 223 | **300.0 B** | 228 | 320 |
| **loss** | 315 | **300.3 B** | 225 | 320 |
| **loss** | 315 | **301.7 B** | 230 | 320 |
| **loss** | 329 | **300.8 B** | 231 | 320 |
| clean | 333 | **300.3 B** | 232 | 320 |

— even with the link reporting up to **100 % retransmission at −106 dBm**. Do not assume the source throttles on a bad link; it keeps encoding at full rate.

## Link behaviour under loss

Two observations that matter far more than codec tuning for real-world stability:

1. **Media packets need to be auto-flushable** (ACL packet-boundary flag `0b10`), with a **200 ms automatic flush timeout** set on the link.
2. **The encoder is never paused, throttled, or restarted** because of link trouble. Transmission continues at full real-time rate throughout.

The combination means a degraded link causes packets to be **discarded**, not queued. The sink therefore sees a stream with *gaps* — which its decoder conceals — rather than a burst of stale audio arriving late, which is what desynchronises an ELD decoder's overlap-add state.

**If your controller does not honour the automatic flush timeout**, media will queue and retransmit unboundedly, and the sink will drain badly delayed audio long after the link recovers. In that case, issue a flush from the host when the controller's TX buffers stay full past the deadline.

## Compatibility Notes

### What works

- **AOT 39 (ER AAC ELD), 480 samples, SBR ratio = 1, VBR with a 256 kbps peak** — the full macOS shape — with the Apple per-frame header. Hardware-verified.
- **AOT 39, 480 samples, no SBR, 265 kbps CBR** also works — AirPods accept it — but it is not what macOS sends.

### What doesn't work

- **512-sample granule length**: AirPods play ~0.1 s then stop. Must be 480 (10 ms mode).
- **No Apple header** (raw AU concatenation): no audio output.
- ~~**SBR**: unstable~~ — **corrected: SBR was never actually enabled in those tests.** See the FDK note below; `aacEncOpen()` must allocate the SBR module or `AACENC_SBR_MODE` is silently ignored.

### FDK-AAC encoder settings (macOS parity — SBR + VBR)

```c
aacEncOpen(&handleAAC, 0x03, 2);                          // 0x03 = AAC core + SBR module!
                                                          // 0x01 makes SBR_MODE a silent no-op
aacEncoder_SetParam(handleAAC, AACENC_AOT, 39);           // ER AAC ELD
aacEncoder_SetParam(handleAAC, AACENC_BITRATE, 256000);   // used only in CBR mode (ignored under VBR)
aacEncoder_SetParam(handleAAC, AACENC_SAMPLERATE, 48000);
aacEncoder_SetParam(handleAAC, AACENC_CHANNELMODE, 2);    // Stereo
aacEncoder_SetParam(handleAAC, AACENC_GRANULE_LENGTH, 480); // 10 ms frames
aacEncoder_SetParam(handleAAC, AACENC_SBR_MODE, 1);       // LD-SBR on
aacEncoder_SetParam(handleAAC, AACENC_SBR_RATIO, 1);      // downsampled, matching Apple —
                                                          // NOT dual-rate; granule stays 480/10 ms
aacEncoder_SetParam(handleAAC, AACENC_BITRATEMODE, 5);    // VBR_5 (~192k stereo nominal, the nearest
                                                          // FDK preset to the ~227k observed average)
aacEncoder_SetParam(handleAAC, AACENC_PEAK_BITRATE, 256000); // ceiling: maxBitsPerFrame
                                                          // = 256000/100 = 320 B — exactly the
                                                          // max AU size seen on the wire
aacEncoder_SetParam(handleAAC, AACENC_AFTERBURNER, 0);    // Off (CPU constraint)
aacEncoder_SetParam(handleAAC, AACENC_TRANSMUX, TT_MP4_RAW); // Raw AUs
```

**Verify SBR actually made it into the bitstream** by reading the encoder's ASC (`aacEncInfo` → `confBuf`): `ldSbrPresentFlag` must be 1 and frameLength must stay 480. `aacEncOpen(..., 0x01, ...)` allocates the AAC core only; the later `AACENC_SBR_MODE` call then checks for an SBR encoder instance, finds none, and **still returns `AACENC_OK`** — so the setting silently does nothing.

Two operational notes:

- **Drop to CBR (`AACENC_BITRATEMODE = 0`) whenever a specific bitrate must actually bind.** Under VBR, FDK ignores `AACENC_BITRATE`, so a coexistence cap, a low-latency gaming rate, or a user-selected rate silently becomes a no-op.
- **Budget packets for the peak, not the average.** Under VBR the per-AU size bound is `PEAK_BITRATE / 800` bytes (320 B at 256 kbps), not `bitrate / 800`.

## Signal Loss Recovery

AAC-ELD uses inter-frame prediction (overlap-add windowing), so encoder and decoder both carry state that depends on previous frames. If frames are lost or a discontinuity is injected mid-stream, the decoder's overlap context diverges from the encoder's and simply resuming transmission can produce silence.

### Symptoms

The L2CAP send buffer fills, `can_send_now` stops firing, and the encode path stalls with data staged. After the link recovers, packets flow again but the sink produces no audio despite receiving a well-formed stream.

### Recovery: SUSPEND → START with encoder reset

A full AVDTP SUSPEND → START cycle makes the sink flush and reinitialise its decoder. Combined with a local encoder reset, both ends restart from a clean state:

1. **Reset the encoder** — close and reopen with the same parameters to clear overlap/prediction buffers
2. **Reset the AU sequence counter**
3. **Reset the RTP timestamp**
4. **Send AVDTP SUSPEND**
5. **On SUSPEND confirmation, send AVDTP START**

```c
int suspend_threshold = (cur_codec == 4) ? 1 : 3;

if (fail_count >= suspend_threshold) {
    if (cur_codec == 4) {
        aacEncClose(&handleAAC);
        aacEncOpen(&handleAAC, 0x03, 2);   // 0x03: keep the SBR module allocated
        // ... re-apply all AACENC_* params ...
        aacEncEncode(handleAAC, NULL, NULL, NULL, NULL);

        aaceld_frame_sequence = 1;
        context->rtp_timestamp = 0;
    }

    a2dp_demo_timer_stop(context);
    avdtp_source_suspend(cid, seid);
    // On AVDTP_SI_SUSPEND callback → avdtp_source_start_stream()
    // On AVDTP_SI_START callback → a2dp_demo_timer_start()
}
```

### Why sequence/timestamp reset alone is not enough

Resetting the sequence and timestamp without SUSPEND → START does not work: the sink still holds stale overlap buffers from the last frame it decoded, while frames from a freshly reset encoder assume zeroed overlap. The mismatch yields silence or artifacts. Only the SUSPEND → START cycle clears the sink's decoder state.

### Suspend recovery timer

If the sink is out of range when SUSPEND is sent, the response may never arrive. Arm a 3-second timer after sending SUSPEND; if nothing comes back, call start-stream directly to force recovery.

### Prevention beats recovery

A recovery cycle costs about a second of audio, so avoid needing one:

- **Never stop the encoder or the packet pump** because the link looks bad — a paused-then-resumed stream is itself the discontinuity that desynchronises the decoder.
- **Bound the transmit backlog** so stale audio is discarded rather than delivered late (see "Link behaviour under loss").
- **Keep the AU sequence continuous** across any dropped packets, so the sink sees a plain content gap rather than a sequence break.
