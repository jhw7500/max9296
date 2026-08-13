# MAX9296 Parallel Prepare Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Add a safe per-MAX9296 pre-GStreamer initialization command whose two CSI instances can run concurrently while preserving the existing V4L2 fallback.

**Architecture:** A synchronous per-instance prepare helper owns mode-table, AP1302 firmware, and post-firmware programming. A sysfs command temporarily owns one board-global power lease; the first V4L2 power-on atomically consumes it, and a fingerprint plus global hardware epoch prevents stale fast starts.

**Tech Stack:** Linux 5.10 V4L2 subdevice driver, I2C, sysfs device attributes, mutex/delayed-work primitives, Python static/model regression tests, ARM64 kernel-module cross-build.

## Global Constraints

- Modify only the `max9296` repository in this phase; do not modify `gstApp` or `pim-package-jhw`.
- One MAX9296 instance is one prepare unit; never split the two channels of a dual-wide CSI domain.
- Preserve no-prepare `s_power`/`s_stream` compatibility.
- Never abort in-flight I2C/firmware work from the lease timeout.
- Lock order is `sensor->lock` then `max9296_power_lock`; never synchronously cancel or join while holding `sensor->lock`.
- The explicit prepare command is `1 <generation> <width> <height> <fps> <enable>` and cancel is `0`.
- Unused driver-owned power leases expire after 60 seconds.

---

### Task 1: Specify and test the prepare contract

**Files:**
- Create: `tests/max9296_prepare_test.py`
- Modify: `tests/run_health_tests.sh`

**Interfaces:**
- Consumes: `max9296.c` source text and a small test-only ownership model.
- Produces: failing assertions for the `prepare` attribute, exact state fields, global epoch, lease transfer, legacy fallback, and cleanup ordering.

- [ ] **Step 1: Write the failing tests**

Add table-driven cases for valid dual/left/right tuples and invalid generation,
dimensions, fps, and masks.  Model these transitions and assert global users:

```text
prepare -> lease -> s_power(1) transfer -> final s_power(0)
prepare -> cancel
prepare -> expiry
prepare failure -> automatic release
two prepares -> one shared reset and two users
remove with lease, with V4L2 owner, and with neither
```

Add source assertions that `s_stream` and sysfs call one common synchronous
helper; READY matching skips it; no-prepare calls it; firmware failures remain
failures; prepare attr setup/unwind/remove are paired; timeout sync-cancel is
outside `sensor->lock`.

- [ ] **Step 2: Run the test and confirm RED**

Run: `rtk python3 tests/max9296_prepare_test.py`

Expected: FAIL because the prepare state, ABI, common helper, and lease
accounting do not yet exist.

- [ ] **Step 3: Register the test runner entry**

Append `python3 tests/max9296_prepare_test.py` to
`tests/run_health_tests.sh`.

- [ ] **Step 4: Commit the red contract test**

```bash
git add tests/max9296_prepare_test.py tests/run_health_tests.sh
git commit -m "test: define max9296 prepare contract"
```

### Task 2: Make firmware and mode initialization synchronous and truthful

**Files:**
- Modify: `max9296.c:3125-3460`
- Test: `tests/max9296_prepare_test.py`

**Interfaces:**
- Consumes: existing register tables and `max9296_loadfw()`.
- Produces: `max9296_prepare_hardware_locked(struct max9296_dev *, const struct max9296_prepare_fingerprint *) -> int` and a stream-commit helper.

- [ ] **Step 1: Run the focused test before production edits**

Run: `rtk python3 tests/max9296_prepare_test.py`

Expected: FAIL on missing common helper and error propagation.

- [ ] **Step 2: Remove the nested firmware thread/workqueue wait**

Call firmware loading synchronously from the common helper.  Release firmware
on every exit, retain FAILED rather than DONE on error, and return exact
request/I2C errno.  Remove `thread_fw_init`, `fw_work`, and `fw_wait` lifecycle
code after their last caller is gone.

- [ ] **Step 3: Extract post-firmware programming**

Move the register writes currently between firmware completion and
`state.enable=DONE` into one helper that stops at and returns the first error.
Do not set `stream_on`, FSYNC RUNNING, output enable, or `streaming` here.

- [ ] **Step 4: Preserve the compatibility path**

When no explicit preparation exists, `s_stream(1)` calls the common helper
synchronously and then commits the stream.  A matching explicit preparation
skips the helper.  A same-epoch structural mismatch returns `-ESTALE`; an
expired preparation has already power-cycled and follows the legacy helper.

- [ ] **Step 5: Run focused and existing tests**

Run: `rtk tests/run_health_tests.sh`

Expected: all tests PASS.

- [ ] **Step 6: Commit the initialization refactor**

```bash
git add max9296.c tests/max9296_prepare_test.py
git commit -m "refactor: split max9296 prepare from stream commit"
```

### Task 3: Add hardware epoch and power-lease ownership

**Files:**
- Modify: `max9296.c:360-470, 1560-1760, 4260-4740`
- Test: `tests/max9296_prepare_test.py`

**Interfaces:**
- Consumes: common prepare helper from Task 2.
- Produces: global monotonic `max9296_hw_epoch`, per-instance fingerprint/state, `lease_held`, and 60-second lease expiry.

- [ ] **Step 1: Confirm the ownership tests fail**

Run: `rtk python3 tests/max9296_prepare_test.py`

Expected: FAIL on missing epoch, lease transfer, expiry, and remove accounting.

- [ ] **Step 2: Implement exact ownership transitions**

Increment the hardware epoch only for an actually executed global power-on
reset or final power-off.  A successful prepare owns one global user.  On the
first local `s_power(1)`, clear `lease_held`, cancel expiry non-synchronously,
set `power_count=1`, and do not increment the global count.  Reject local
power-count underflow.

- [ ] **Step 3: Implement safe expiry**

The delayed worker records state/generation under `sensor->lock`, clears only
an unused READY/STALE lease, unlocks, and then returns the global user.  It
never modifies PREPARING and never cancels firmware I/O.

- [ ] **Step 4: Integrate removal**

Mark the instance dying, withdraw sysfs/V4L2 admission, call
`cancel_delayed_work_sync()` outside `sensor->lock`, then reconcile exactly one
global user for either a lease or positive local `power_count`.  Preserve the
existing probe-failure publication handshake and reverse sysfs unwind.

- [ ] **Step 5: Run regression tests**

Run: `rtk tests/run_health_tests.sh`

Expected: all tests PASS without ownership-model underflow or static-order
failures.

- [ ] **Step 6: Commit lease ownership**

```bash
git add max9296.c tests/max9296_prepare_test.py
git commit -m "feat: hold max9296 prepare power lease"
```

### Task 4: Expose the blocking sysfs prepare ABI

**Files:**
- Modify: `max9296.c:3890-4300, 4520-4740`
- Create: `docs/parallel-prepare-v1.md`
- Test: `tests/max9296_prepare_test.py`

**Interfaces:**
- Consumes: prepare helper, fingerprint, epoch, and lease transitions.
- Produces: `/sys/bus/i2c/devices/<bus>-0048/prepare` read/write ABI.

- [ ] **Step 1: Confirm ABI tests fail**

Run: `rtk python3 tests/max9296_prepare_test.py`

Expected: FAIL because `dev_attr_prepare` and its parser/status output are
absent.

- [ ] **Step 2: Implement strict command parsing and validation**

Accept only `0` or six fields for command 1.  Require non-zero generation,
exact supported tuple/mask combinations, fps `1..120`, non-streaming state,
and no V4L2 power owner.  Same READY generation/fingerprint is idempotent;
same generation with another tuple returns `-ESTALE`; other busy transitions
return `-EBUSY`.

- [ ] **Step 3: Implement status output**

Emit one line with stable `key=value` fields for state, generation, epoch,
mode/table side, width, height, fps, code, enable, errno, lease, and match.

- [ ] **Step 4: Pair sysfs lifecycle paths**

Create `prepare` before final async V4L2 registration.  Add reverse-order probe
unwind, normal remove, and update the probe-cleanup test so registration
remains the final fallible operation.

- [ ] **Step 5: Document operator usage**

Document two background writes plus `wait`, status parsing, 60-second expiry,
cancel, and the requirement that `gstApp` use the same channel tuple.

- [ ] **Step 6: Run all host tests**

Run: `rtk tests/run_health_tests.sh`

Expected: all tests PASS.

- [ ] **Step 7: Commit the ABI**

```bash
git add max9296.c tests docs/parallel-prepare-v1.md
git commit -m "feat: expose max9296 parallel prepare ABI"
```

### Task 5: Build, review, and define the board gate

**Files:**
- Modify only if review finds a defect in files already listed.

**Interfaces:**
- Consumes: complete driver change.
- Produces: build/test evidence and a commit ready for the later PIM/gstApp integration phase.

- [ ] **Step 1: Run source hygiene and host tests**

```bash
rtk git diff --check
rtk python3 -m py_compile tests/*.py tools/*.py
rtk tests/run_health_tests.sh
```

Expected: no diff errors; Python compilation and all tests PASS.

- [ ] **Step 2: Cross-build the module**

Use the repository's known i.MX8 ARM64 kernel tree/toolchain and run the same
isolated build command used for commit `2106d91`.

Expected: `max9296.ko` builds; `modinfo` reports the intended module version
and target vermagic.

- [ ] **Step 3: Run independent review**

Review state transitions, all global-user increments/decrements, lock order,
sysfs removal versus active write, stale epoch gates, error-resource cleanup,
and legacy no-prepare behavior.  Fix every blocking finding and rerun Steps 1
and 2.

- [ ] **Step 4: Record the board stop condition**

Do not enable this path in boot scripts yet.  The next phase begins only after
the target demonstrates overlapping two-CSI firmware intervals, no second
download at matching GStreamer STREAMON, correct expiry/cancel/error cleanup,
and successful single/dual 100-cycle tests.

- [ ] **Step 5: Commit review fixes**

```bash
git add max9296.c tests docs
git commit -m "test: validate max9296 parallel prepare lifecycle"
```

