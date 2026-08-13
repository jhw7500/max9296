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

Mode is `0664`. Start syntax is exactly:

```text
1 <generation> <width> <height> <fps> <enable>
```

`generation` is a non-zero unsigned 64-bit orchestration identifier and `fps`
is `1..120`. Supported hardware tuples are:

| width x height | enable | table |
| --- | --- | --- |
| 2560x720 | 3 | dual-wide |
| 3840x1080 | 3 | dual-wide |
| 1280x720 | 1 | single left |
| 1280x720 | 2 | single right |
| 1920x1080 | 1 | single left |
| 1920x1080 | 2 | single right |

The media-bus format is always the driver's UYVY format and is not an input.
Extra fields, signed values, unsupported dimensions/masks, generation zero,
and out-of-range FPS are rejected.

## Parallel use

Issue one blocking write per independent CSI domain in the background, then
wait for both. For example:

```sh
gen="$(date +%s)"
printf '1 %s 2560 720 30 3\n' "$gen" \
  > /sys/bus/i2c/devices/2-0048/prepare &
pid0=$!
printf '1 %s 2560 720 30 3\n' "$gen" \
  > /sys/bus/i2c/devices/3-0048/prepare &
pid1=$!

wait "$pid0" || exit $?
wait "$pid1" || exit $?
```

After both writes succeed, read status before starting the application:

```sh
cat /sys/bus/i2c/devices/2-0048/prepare
cat /sys/bus/i2c/devices/3-0048/prepare
```

Status is one newline-terminated key/value line:

```text
state=READY generation=123 epoch=7 mode=dual-wide table=dual width=2560 height=720 fps=30 code=0x2006 enable=3 errno=0 lease=1 match=1
```

Treat field order and names as the v1 machine-readable contract. `lease=1`
means the driver still owns the temporary power reference. `match=1` means the
current runtime tuple and initialized hardware fingerprint still agree in the
current board-power epoch.

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

Cancel an unused lease with:

```sh
printf '0\n' > /sys/bus/i2c/devices/2-0048/prepare
```

Cancel never powers down a lease already consumed by V4L2 and never stops an
active stream. Such owner/stream transitions are reported busy. Cancellation,
expiry, or successful prepare is not a GMSL reconnection or hard-reset policy;
physical cable recovery remains the responsibility of the higher-level camera
recovery flow.
