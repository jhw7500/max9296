# MAX9296 360p Readout and 120 FPS Design

## Goal

Provide a reproducible comparison between the existing per-camera 640x360
output path and an experimental AR0234 640x360 sensor-crop path, while keeping
the user-visible digital zoom request at its default `dz=100`. Apply the AP1302
high-frame-rate preview policy required to determine whether the complete
sensor-to-ISI path can sustain 120 fps.

This design covers the `max9296` driver, gstApp edgeconf integration, target
deployment, measurement, and rollback. The experimental crop path is not a
claim that a new vendor-native AR0234 mode has been created.

## Verified Baseline

All measurements below used dual output `1280x360@1/120`, which represents two
640x360 camera images. Each run changed `/root/shared_v/edgeconf_pim.json`, ran
`/root/camtest/cam_hard_reset.sh -s -S`, measured the live hardware, restored
the original file, and ran the hard reset again. The original and restored
configuration SHA-256 was
`b075884b59ce0d44b3361bd7ae37b0fa4bb04da2c96b94bddf4d9deb42447a6f`.

| AP1302 zoom request | AR0234 active window | CSI fps | ISI fps | Meaning |
|---:|---:|---:|---:|---|
| 100 | 1920x1080 | 113.580 | 112.348 | Existing full-readout ISP-scale path |
| 200 | 960x540 | 115.379 | 115.013 | Existing 2x zoom path |
| 300 | 640x360 | 114.083 | 112.884 | Firmware-managed sensor-crop probe |

The 15-second raw IRQ measurements divide CSI IRQs by two because the measured
CSI source emits one Frame Start and one Frame End interrupt per frame. The
sensor window was read independently through AP1302 DMA using AR0234 registers
`0x3002`, `0x3004`, `0x3006`, and `0x3008` on both channels.

The crop probe proves that AP1302 firmware can configure a coherent 640x360
AR0234 window, including the associated line/frame timing. It does not show a
meaningful frame-rate improvement by itself. The common AP1302 high-fps policy
must therefore be applied before judging either readout path.

## Scope and Non-goals

In scope:

- Per-camera 640x360 output in single and dual MAX9296 modes.
- A default `isp_scale` readout profile and an experimental `sensor_crop`
  profile.
- A common readout profile per MAX9296/CSI domain.
- edgeconf configuration and gstApp V4L2 control application.
- AP1302 preview timing for requested 31 through 120 fps in 360p modes.
- Existing exposure safety, AE, gain, AWB, zoom-center, prepare, and rollback
  behavior.
- A controlled A/B test at `dz=100` and 120 requested fps.

Out of scope:

- Direct, guessed AR0234 timing-register writes from the driver.
- Claiming a vendor-certified native 640x360 sensor mode.
- Preserving the same field of view between full-readout and sensor-crop paths.
- Additional digital zoom inside the experimental sensor-crop path.
- New writes to AP1302 manual white-balance register `0x510A`.
- Changes to non-360p mode timing.

## Interface

### V4L2 control

Add one common custom menu control per MAX9296 subdevice:

| Control | ID | Values | Default |
|---|---:|---|---:|
| `readout_mode` | `V4L2_CID_USER_BASE + 0x102b` | `0=isp_scale`, `1=sensor_crop` | 0 |

The control is common because two different sensor heights cannot safely be
concatenated into one dual output frame. It is cached while the device is off,
included in the hardware fingerprint, and restored after firmware reload.

`sensor_crop` is valid only when each active camera outputs 640x360 and
user-visible `dz` is exactly 100. An incompatible mode or zoom request returns
`-EINVAL` before an AP1302 I2C write. Changing `readout_mode` while streaming
returns `-EBUSY`.

The `readout_mode` setter is cache-only: while stopped it records the request,
marks any prepared hardware state stale, and performs no AP1302 write. Because
gstApp may submit controls before the final caps have selected a mode, the
authoritative mode/fps/zoom compatibility check occurs at preparation entry,
before the first mode-table I2C write. A `dz` request other than 100 is also
rejected immediately whenever cached `readout_mode` is `sensor_crop`.

### edgeconf

Each CSI/I2C object accepts an optional integer:

```json
{
  "i2c2": {
    "readout_mode": 0,
    "dz": 100
  }
}
```

Missing or invalid values fall back to `0` with an operator-visible log. The
field is common to the two channels under that I2C object.

Changing this field operationally requires `cam_hard_reset.sh -s -S` or
`init_cam.sh`; restarting gstApp alone does not establish a new driver hardware
epoch.

## Readout Semantics

### `isp_scale` (`readout_mode=0`)

The existing contract remains unchanged:

```text
AR0234 readout selected by AP1302 dz
    -> AP1302 processing/scaling
    -> 640x360 per-camera output
```

`dz=100` maps directly to AP1302 `DZ_TGT_FCT(0x1010)=0x0100` and produces the
verified 1920x1080 sensor window. Values 100 through 300 retain their current
meaning.

### `sensor_crop` (`readout_mode=1`)

This comparison-only profile uses the already verified AP1302 firmware path to
establish the coherent AR0234 640x360 window:

```text
AP1302 firmware-managed base crop (effective factor 3.00)
    -> AR0234 640x360 active window
    -> AP1302 640x360 output without spatial downscale
```

User-visible `dz=100` means no additional zoom relative to this selected
readout. Internally AP1302 `DZ_TGT_FCT` is `0x0300` because the current firmware
uses that factor to own and program the sensor window and timing. The driver
logs both requested and effective values so this translation is never silent.
V4L2 readback of `dz` remains 100; the effective value is verified from the
kernel apply log and a direct AP1302 `0x1010` read after preparation.

This is an explicit exception to the direct `dz`-to-register mapping in the
experimental profile. Strict physical preservation of
`dz=100 -> AP1302 0x1010=0x0100` would require a separate vendor firmware
sensor-mode control that is not available in the current firmware. Direct DMA
writes are not substituted because AP1302 firmware owns those timing registers
and can overwrite or desynchronize them.

`dz_x` and `dz_y` continue to select the normalized crop center. Additional
zoom (`dz>100`) is rejected in this profile because the firmware's supported
factor ceiling is already consumed by the base 3x crop.

## AP1302 360p High-FPS Policy

After firmware load and preview width/height programming, configure each active
AP1302 independently when the per-camera output is 640x360 and requested fps is
greater than 30:

1. Write `ATOMIC(0x1184)=0x0001`.
2. Write `PREVIEW_MAX_FPS(0x2020)=fps << 8` in unsigned 8.8 format.
3. Write `TRIGGER_MAX_MISMATCH(0x6112)=0x0000`.
4. Write `ATOMIC(0x1184)=0x0013`.

`PREVIEW_LINE_TIME(0x201c)` remains firmware-controlled (`0`) initially. The
sensor already reports approximately 120 fps, and forcing an unneeded line
time would mix sensor-timing changes into the readout comparison.

If an intermediate write fails, the driver makes a best-effort atomic-finish
write, returns the first error, leaves hardware preparation invalid, and never
enables MIPI output or FSYNC for that preparation. At 30 fps or lower, and for
all non-360p modes, no new timing-register write occurs.

## Driver State and Data Flow

Add `readout_mode` to the shared control cache and
`max9296_hw_fingerprint`. The fingerprint comparison therefore treats readout
profile changes as real hardware changes rather than warm-reusing a stale
firmware configuration.

Preparation order is:

1. Validate mode, fps, readout profile, and requested zoom without I2C.
2. Load the existing MAX9296/MAX9295 mode table.
3. Load AP1302 firmware.
4. Program per-camera preview output dimensions.
5. Apply the 360p high-fps atomic policy when applicable.
6. Resolve requested versus effective readout zoom and apply both channel
   centers.
7. Replay existing exposure, gain, AE/AWB, flip, LSC, and LED controls.
8. Publish the hardware fingerprint, then allow FSYNC and output enable.

The effective factor is derived for each preparation and is not written back
into the user control cache. Thus firmware replay cannot accidentally turn the
experimental base crop into a persistent user request of `dz=300`.

The existing exposure policy remains independent: manual exposure and any AE
transition that would write `EXP_TIME(0x500c)` above the safe 30 fps ceiling is
still rejected before I2C. No `0x510A` write is introduced.

## gstApp Integration

Parser and control-builder changes are limited to:

- Store one `readout_mode` value per CSI domain, defaulting to `isp_scale`.
- Validate the integer range `0..1` and fall back to zero on invalid JSON.
- Emit the readout-mode V4L2 control before `dz` and center controls during
  `VideoBin::init()`.
- Keep the existing prepare ABI unchanged. VideoBin applies controls before
  `max9296_prepare_all()`, allowing the driver to include the cached mode in
  its internal hardware fingerprint.
- Log CSI domain, readout mode, requested `dz`, and center values once at
  startup.

The GStreamer pipeline remains unchanged. Both profiles expose the same
`1280x360@120` dual or `640x360@120` single caps, so encoders, RTSP appsrc, and
application-level resolution freedom are unaffected.

## Testing

### Automated driver policy tests

Put pure readout/timing decisions in a small shared policy header consumed by
the driver and a host-compiled test. Test these observable decisions:

- `isp_scale`, 640x360, `dz=100` resolves to effective 100.
- `sensor_crop`, 640x360, `dz=100` resolves to effective 300.
- `sensor_crop` rejects non-360p output and `dz=101..300`.
- Invalid readout values are rejected.
- 360p at 31, 60, and 120 fps enables the timing policy and produces
  `0x1f00`, `0x3c00`, and `0x7800` respectively.
- 360p at 30 fps and non-360p modes produce no timing-policy writes.

The test must fail before production integration, then pass after the minimal
policy implementation. Existing health, prepare, 360p/zoom/exposure, and
script-classification tests must also pass, followed by a kernel-module build.

### Automated gstApp tests

Extend the existing max9296 controls host test to verify:

- default and invalid `readout_mode` normalization;
- readout control emission before common `dz` and per-channel centers;
- both single-slot and dual-slot control lists;
- crop-mode compatibility validation at 640x360 and `dz=100`.

Build gstApp only through `./make-for-imx8`.

### Target A/B procedure

Use one common driver and gstApp build for both cases:

| Case | `readout_mode` | `dz` | Required sensor window |
|---|---:|---:|---:|
| A: ISP scale | 0 | 100 | 1920x1080 |
| B: sensor crop | 1 | 100 | 640x360 |

For each case:

1. Back up edgeconf and record its hash.
2. Set 640x360 per camera, fps 120, the selected readout mode, and `dz=100`.
   Keep identical recorded `dz_x`/`dz_y` center values in both cases.
3. Run `cam_hard_reset.sh -s -S`.
4. Read back `0x2020=0x7800`, `0x6112=0`, the readout control, effective zoom,
   and both sensors' window/timing registers. The requested zoom comes from
   V4L2; the effective zoom comes from each AP1302 `0x1010` register.
5. Run corrected `cam_fps_stack.sh -c ch01 -d 20 -i 1`.
6. Measure raw CSI/ISI IRQ deltas over at least 15 seconds without I2C polling.
7. Record gstApp CPU/RSS, whole-system CPU, DDR PMU busy cycles, temperature,
   and kernel CSI/ISI errors over the same stable interval. Resource and raw
   IRQ sampling run without concurrent AP1302/AR0234 I2C polling, which was
   observed to perturb sensor-counter sampling.
8. Decode RTSP and record signal statistics plus a frame image to detect green
   corruption. RTSP caps or `videorate` output alone are not FPS evidence.
9. Restore edgeconf and run the hard reset again before the next case or handoff.

An actual-120 pass requires raw CSI and ISI rates of at least 118.8 fps over the
15-second interval, CSI-to-ISI loss no greater than 1%, and no CSI/ISI overflow,
CRC, ECC, lost-frame, or green-frame failure. Results below that threshold are
reported as measured values, not rounded up to 120.

Resource results are comparative, not pass/fail. Because both paths emit the
same UYVY frame size and cadence, SoC/encoder use may remain similar even if
sensor/AP1302 work changes. Image sharpness and field of view are reported
separately: case B is a centered/normally aimed 3x base crop and is not
image-equivalent to case A.

## Deployment and Rollback

Deploy only after both repository builds and automated tests pass. Save the
installed module, gstApp binary, and edgeconf with run-specific names before
replacement. Use the board-lock wrapper for every target mutation.

Rollback restores all three artifacts, runs `cam_hard_reset.sh -s -S`, verifies
the original configuration hash and active service, and confirms the board has
no remaining holder or reservation. A failed test never leaves a 120 fps or
experimental readout configuration active.
