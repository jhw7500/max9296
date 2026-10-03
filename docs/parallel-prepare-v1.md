# MAX9296 parallel prepare ABI (v1)

## Purpose and boundary

Each MAX9296 `prepare` file initializes one MAX9296/CSI domain before
GStreamer opens the V4L2 graph. A dual-wide domain is one indivisible request;
it is not two independently recoverable links.

A successful write means only that the selected SERDES table, AP1302 firmware,
and post-firmware configuration completed. It does **not** mean that frames are
flowing, that GMSL links are healthy, or that a disconnected camera recovered.
The command does not enable CSI output or start FSYNC.

## ABI

The blocking read/write attribute is:

```text
/sys/bus/i2c/devices/<bus>-0048/prepare
```

`<bus>` is the Linux i2c adapter number, which is one less than the
device-tree node name because `imx8mp.dtsi` declares `i2c1 = &i2c2` and
`i2c2 = &i2c3`. Do not copy the device-tree node number into the path. The
current board mapping is:

| Device-tree node | prepare node | Channels | Video node |
| --- | --- | --- | --- |
| `max9296_0` (`&i2c2`) | `/sys/bus/i2c/devices/1-0048/prepare` | ch2/ch3 | `/dev/video3` |
| `max9296_1` (`&i2c3`) | `/sys/bus/i2c/devices/2-0048/prepare` | ch0/ch1 | `/dev/video4` |

Mode is `0664`. Start syntax is exactly:

```text
1 <generation> <width> <height> <fps> <enable>
```

`generation` is a non-zero unsigned 64-bit orchestration identifier. `fps` is
at least 1 and must not exceed the selected tuple's ordinary limit:

| width x height | enable | table | ordinary max FPS | exposure-write max FPS |
| --- | --- | --- | ---: | ---: |
| 2560x720 | 3 | dual-wide (1280x720 per channel) | 60 | 30 |
| 3840x1080 | 3 | dual-wide (1920x1080 per channel) | 30 | 30 |
| 1280x360 | 3 | dual-wide (640x360 per channel) | 120 | 30 |
| 1280x720 | 1 | single left | 60 | 30 |
| 1280x720 | 2 | single right | 60 | 30 |
| 1920x1080 | 1 | single left | 30 | 30 |
| 1920x1080 | 2 | single right | 30 | 30 |
| 640x360 | 1 | single left | 120 | 30 |
| 640x360 | 2 | single right | 120 | 30 |

The media-bus format is always the driver's UYVY format and is not an input.
Extra fields, signed values, unsupported dimensions/masks, generation zero,
and out-of-range FPS are rejected.

`ordinary max FPS` is the rate the driver accepts, not a rate it delivers.
Commit `eb3c54a` raised the 720p value to 60 in 2.12, citing a measurement this
repository records: `docs/fps-limit-analysis.md` logs 54.0-55.5 FPS for a 60 FPS
request on a single 1280x720 channel. That same file states the delivered rate
under the raised ceiling was not measured, and draws the same distinction for
360p, whose 120 ceiling has a recorded 113-115. This ABI cannot close that gap:
the `fps` in the status line below is the requested value, and
`V4L2_CID_PIXEL_RATE` is derived from that same value, so neither can disagree
with what was asked for. Delivered rate is measured out of band --
`docs/fps-limit-analysis.md` section 8.1 uses `cam_fps_stack.sh` and
`cam_fps_watch.sh`. This table was written before the 2.12 change and said 30
until it was corrected.

The 640x360 default `KEEP` policy changes each AP1302 preview/CSI output to
640x360 but does not claim that AR0234 sensor readout also became 640x360.
Sensor-readout candidates are separate build artifacts and are classified only
from AR0234 timing/read-mode plus full-FOV evidence.

Hardware digital crop is orthogonal to this tuple. `crop_enable=false` is the
default; it does not honour the cached user tuple, but it still writes the
per-resolution seed to AP1302 `0x1012`, `0x118c`, `0x118e` and `0x1010` (every
mode currently seeds 1.00x at the frame centre), so a previously applied crop
cannot outlive the flag. The cache itself is untouched, so re-enabling restores
the user tuple. gstApp submits crop enable and the complete cached crop tuple
before `prepare`; the prepare fingerprint includes enable state. A streaming
enable transition fails with `EBUSY`.

Above the 30 FPS exposure-safety limit — 640x360 at 31-120 FPS and 1280x720 at
31-60 FPS — AE-auto preparation skips the `0x500c` exposure seed but preserves
the remaining AE/gain/AWB controls. A pending manual exposure is **applied after
an operator-visible warning**, not rejected: the write proceeds and the log
records the channel, mode, FPS, requested exposure, frame period, `over_period`
and `action=write`. Only an FPS the mode does not allow — or a zero FPS or an
invalid safety limit — is rejected, with `-EINVAL`, before the first mode-table
I2C write. No manual-WB `0x510a` write is part of this ABI.

## Parallel use

Issue one blocking write per independent CSI domain in the background, then
wait for both. For example:

```sh
gen="$(date +%s)"
printf '1 %s 2560 720 30 3\n' "$gen" \
  > /sys/bus/i2c/devices/1-0048/prepare &
pid0=$!
printf '1 %s 2560 720 30 3\n' "$gen" \
  > /sys/bus/i2c/devices/2-0048/prepare &
pid1=$!

wait "$pid0" || exit $?
wait "$pid1" || exit $?
```

Both writes must be in flight at the same time. Issuing them one after the
other is **not supported**: each write is accepted and reports `state=READY`
with the expected firmware download, but the second domain then streams only
one or two frames before stalling. Measured 2026-08-21, reproducing every
time:

```text
sequential   prepare(READY,READY) fw=2   video3 ok   video4 timeout   ISI 11 / 2
parallel     prepare(READY,READY) fw=2   video3 ok   video4 ok        ISI 11 / 12
```

The driver does not reject the sequential form, so a caller cannot detect the
problem from the ABI - the status line looks correct while the hardware is
not. The cause is not established; the shared FSYNC handover is the obvious
suspect but has not been confirmed. Treat the parallel form as the contract.

The two MAX9296 instances share one physical FSYNC signal, so both commands
must request the same FPS. The first request reserves that cadence for the
current board-power epoch and propagates it to the idle peer; a conflicting
request is rejected with `ESTALE` before its live/request tuple is published.
Width, height, and enable remain per-CSI-domain values.

After both writes succeed, read status before starting the application:

```sh
cat /sys/bus/i2c/devices/1-0048/prepare
cat /sys/bus/i2c/devices/2-0048/prepare
```

Status is one newline-terminated key/value line:

```text
state=READY generation=123 epoch=7 mode=dual-wide table=dual width=2560 height=720 fps=30 code=0x2006 enable=3 crop_enable=0 errno=0 worker_errno=0 lease=1 match=1
```

Treat field order and names as the v1 machine-readable contract. That promise
has been broken twice inside v1, both times by insertion rather than append:
`02c014a` put `worker_errno` between `errno` and `lease`, and `1108e57` put
`crop_enable` between `enable` and `errno`, so every field after `enable` has
moved twice. This example did not list `crop_enable` until that was corrected.
Until that record improves, parse by key and tolerate a key this example does
not show rather than reading by position. `lease=1`
means the driver still owns the temporary power reference. `match=1` means two
things hold in the current board-power epoch: the current runtime tuple equals
the requested tuple including `fps`, and the initialized hardware fingerprint
equals the runtime one.

That second term compares what `fps` actually programs -- the preview ceiling
and the cached-exposure seed route -- rather than the raw rate, so `match=1`
does not by itself prove the hardware was initialized at the exact `fps` printed
on the same line. It proves no register the driver derives from the rate
differs. Two rates that program the same registers are the same hardware, which
is what lets a cadence change reuse an initialized epoch; a rate change that
moves either derivation reports `match=0` until the hardware is reprogrammed.

`worker_errno=0` means the local enable worker and the current physical FSYNC
owner worker are available. A negative value is a durable output-path
diagnostic; STREAMON fails with that error even though a prior `READY` still
means its firmware/config preparation completed successfully.

`V4L2_CID_PIXEL_RATE` is derived on every read, not stored. The two MAX9296
instances share one FSYNC cadence, so accepting a rate on one rewrites the
other's `fps`; the transaction that does so must not take the peer's V4L2 lock,
which is also its control-handler lock, and therefore cannot refresh the peer's
control. The control is `VOLATILE` instead, so `VIDIOC_G_EXT_CTRLS` re-derives
it and reflects the current rate and mode. `VIDIOC_G_CTRL` cannot serve it at
all, before or after this change: the control is `INTEGER64`, and
`v4l2_g_ctrl()` rejects a control that is not `is_int` with `EINVAL` before the
volatile path is reached. A caller that caches the value across a cadence change
holds a stale one -- re-read it rather than remembering it.

Two readers do not re-derive it: `VIDIOC_LOG_STATUS` and a `V4L2_EVENT_CTRL`
subscription report the value stored at probe and never change it. Neither is an
input to any driver decision; use `VIDIOC_G_EXT_CTRLS` if the value matters.

## Handoff, expiry, and cancel

`gstApp` must open/configure the same width, height, FPS, channel mask, and UYVY
format. Identical ACTIVE V4L2/sysfs writes preserve readiness. A different
hardware tuple in the same power epoch is rejected as stale rather than being
reprogrammed without the required reset.

The first V4L2 power-on consumes the prepare lease without adding another
global power reference. An unused READY or STALE lease expires after 60
seconds. If another camera instance keeps the board powered, a later identical
prepare may reacquire a lease and reuse the still-current hardware; otherwise a
real power transition invalidates it and initialization runs again.

Cold initialization is attempted at most once per instance in a board-power
epoch. The attempt is remembered even if a table write, firmware transfer, or
subsequent configuration step fails: serializer addresses may already have
changed. A later prepare/STREAMON that needs cold initialization in that same
epoch returns `ESTALE` with `camera hard reset required`, before any table or
firmware replay. A new process discarding channel exposure overrides can also
require this reset; see `V4L2_CTRL_GUIDE.md`. Use this repository's
`tools/cam_hard_reset.sh -s -S` or `init_cam.sh` to perform the physical reset and
reapply the JSON configuration. Note that pim-package deploys a **different**
script under the same basename at `/opt/pim/bin/cam_hard_reset.sh`, which is a
deprecated compatibility boundary whose `-s`/`-S` no longer control services;
check which path you are invoking.
The driver does not forcibly cycle the peer's shared power in this error path.
An unchanged, valid configuration remains reusable without cold initialization.

Cancel an unused lease with:

```sh
printf '0\n' > /sys/bus/i2c/devices/1-0048/prepare
```

Cancel never powers down a lease already consumed by V4L2 and never stops an
active stream. Such owner/stream transitions are reported busy. Cancellation,
expiry, or successful prepare is not a GMSL reconnection or hard-reset policy;
physical cable recovery remains the responsibility of the higher-level camera
recovery flow.
