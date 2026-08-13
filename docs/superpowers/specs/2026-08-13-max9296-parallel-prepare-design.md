# MAX9296 Parallel Prepare Design

## Scope

This change adds only the MAX9296 driver half of pre-GStreamer camera
initialization.  It does not parse JSON, decide which CSI domains are required,
or launch `gstApp`.  One invocation prepares one MAX9296 instance, which means
one CSI stream domain; a dual-wide instance remains one indivisible prepare
operation.

The existing V4L2 `s_power`/`s_stream` path remains usable when no prepare
request was made.  Automatic recovery and runtime mode switching are outside
this change.

## Motivation

The i.MX8 media graph calls the two subdevices' blocking `s_stream(1)` methods
serially.  Each method currently loads the SERDES table and AP1302 firmware,
taking about six seconds.  A command issued to the two independent I2C devices
in parallel before GStreamer starts avoids that media-graph serialization.

An external helper cannot merely STREAMON and exit: its final V4L2 close may
release the board-global power reference and discard the firmware it just
loaded.  The driver therefore owns a temporary power lease and atomically
transfers that ownership to the first subsequent V4L2 `s_power(1)`.

## ABI

The existing I2C device sysfs group gains `prepare` with mode `0664`.

Start a synchronous prepare:

```text
1 <generation> <width> <height> <fps> <enable>
```

Constraints:

- `generation` is a non-zero unsigned 64-bit orchestration identifier.
- Dual-wide tuples are `2560x720` or `3840x1080` with `enable=3`.
- Single-channel tuples are `1280x720` or `1920x1080` with `enable=1` or
  `enable=2`.
- `fps` is in the existing driver range `1..120`.
- The media-bus code is the driver's only supported code,
  `MEDIA_BUS_FMT_UYVY8_2X8`.

The write blocks until mode-table programming, AP1302 firmware download, and
post-firmware programming finish.  Independent writes to I2C1 and I2C2 can be
run in shell background jobs and proceed concurrently after the single shared
power reset.

Cancel an unused lease:

```text
0
```

Reading `prepare` returns one machine-parseable line containing state,
generation, hardware epoch, resolved mode/table, format, fps, enable mask,
last errno, lease ownership, and whether the current runtime fingerprint still
matches.  Invalid syntax/tuple/generation returns `-EINVAL`; an active stream,
V4L2 power owner, or concurrent prepare returns `-EBUSY`; hardware operations
return their original negative errno.

## State and identity

Per instance:

```text
IDLE -> PREPARING -> READY -> CONSUMED
                    \-> FAILED
READY/STALE -> EXPIRED
```

The prepared fingerprint consists of the resolved register-table identity
(including left versus right single-channel table), width, height, media-bus
code, fps, enable mask, opaque generation, and board hardware epoch.

The board-global epoch advances whenever the driver actually executes a
global power-on reset or final power-off.  `READY` is usable only while the
epoch and hardware-relevant fingerprint match.  An ACTIVE format, FPS, or
enable change marks an unused/consumed preparation stale; TRY formats do not.
Because changing between dual and single tables without a reset is unsafe, a
same-epoch structural mismatch after explicit prepare is rejected rather than
silently loading an incompatible table.  A launch without explicit prepare,
or one after its lease expired and power cycled, uses the legacy synchronous
fallback.

## Power ownership

`prepare` takes exactly one board-global user for its instance and records
`lease_held=true`.  The first local `s_power(1)` changes that lease into the
instance's normal `power_count=1` without incrementing the global user count.
Further opens only increment the local count.  The final close returns the one
global user as before.

An unused READY or STALE lease expires after 60 seconds.  Expiry never aborts
PREPARING hardware I/O; it only withdraws an idle lease after rechecking state
and generation.  A failed prepare returns its lease immediately.  `prepare=0`,
expiry, probe removal, and V4L2 close may each release only the ownership they
currently hold.

The lock order is always:

```text
sensor->lock -> max9296_power_lock
```

No `cancel_delayed_work_sync()`, `kthread_stop()`, or other join occurs while
holding `sensor->lock`.

## Initialization refactor

The first-stream initialization becomes a common synchronous helper called by
both sysfs prepare and the compatibility `s_stream(1)` path:

1. Resolve the exact dual/left/right table and mark initialization running.
2. Load the SERDES table and propagate its errno.
3. Download AP1302 firmware synchronously and propagate request/I2C/work
   errors instead of converting failure to DONE.
4. Run the existing post-firmware AP1302/DES writes with first-error
   propagation.
5. Record the fingerprint and epoch only after every step succeeds.

Successful prepare leaves MIPI output, stream request, and FSYNC/enable commit
inactive.  Matching `s_stream(1)` performs only the stream commit; the existing
FSYNC and enable workers then activate output.  This prevents an unused
prepare lease from generating a nominal live stream.

## Removal and invalidation

Removal first marks the instance dying, removes the new command surface and
unregisters V4L2, synchronously drains only its lease-expiry work outside
`sensor->lock`, waits for an already admitted synchronous prepare, and then
reconciles its lease and local power ownership exactly once before existing
worker/media cleanup.

Hard reset, module removal, a real global power transition, and failed
initialization invalidate fast-path readiness.  FSYNC and output-enable paths
must not treat stale DONE bits from a previous hardware epoch as current.

## Validation

Host regression tests enforce the ABI parser, state/lease accounting model,
common-helper use, error propagation, sysfs unwind, lock/join ordering, and
legacy fallback.  The full existing health tests and ARM64 module build must
pass.

The board gate is intentionally separate:

1. Run the two prepare writes concurrently and show overlapping firmware
   intervals and wall time near one CSI preparation rather than their sum.
2. Start `gstApp` with the same tuple and verify no second firmware download or
   board reset occurs.
3. Exercise single-left, single-right, dual-wide, one-link failure, firmware
   failure, expiry, explicit cancel, SIGTERM, module removal, and 100 cold
   cycles.

