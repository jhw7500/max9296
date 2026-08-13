#!/usr/bin/env python3
"""Contract tests for the MAX9296 pre-GStreamer prepare lifecycle.

The small model fixes the ownership semantics independently of the kernel
implementation.  Source checks then make sure the driver exposes the same
contract; target tests remain the authority for hardware concurrency.
"""

from __future__ import annotations

import re
import sys
from dataclasses import dataclass
from pathlib import Path


ROOT = Path(__file__).resolve().parents[1]
SOURCE = ROOT / "max9296.c"


def parse_prepare_command(text: str) -> tuple[int, ...]:
    """Executable model of the strict, whitespace-delimited v1 grammar."""
    fields = text.strip(" \t\n").split()
    if fields == ["0"]:
        return (0,)
    if len(fields) != 6 or fields[0] != "1":
        raise ValueError("syntax")
    if any(not field.isascii() or not field.isdecimal() for field in fields[1:]):
        raise ValueError("unsigned decimal fields only")

    generation, width, height, fps, enable = map(int, fields[1:])
    if generation == 0 or generation > (1 << 64) - 1:
        raise ValueError("generation")
    if fps < 1 or fps > 120:
        raise ValueError("fps")
    if (width, height) in ((2560, 720), (3840, 1080)):
        if enable != 3:
            raise ValueError("dual mask")
    elif (width, height) in ((1280, 720), (1920, 1080)):
        if enable not in (1, 2):
            raise ValueError("single mask")
    else:
        raise ValueError("tuple")
    return (1, generation, width, height, fps, enable)


@dataclass
class Board:
    users: int = 0
    epoch: int = 0
    resets: int = 0
    fsync_pulses: int = 0
    des_writes: int = 0
    epoch_guarded: bool = False

    def get(self) -> None:
        if self.users == 0:
            self.epoch += 1
            self.resets += 1
        self.users += 1

    def put(self) -> None:
        assert self.users > 0
        self.users -= 1
        if self.users == 0:
            self.epoch += 1

    def invalidate(self) -> None:
        assert not self.epoch_guarded
        self.epoch += 1


@dataclass
class Camera:
    board: Board
    state: str = "idle"
    lease: bool = False
    power_count: int = 0
    generation: int = 0
    lease_generation: int = 0
    initialized_epoch: int = 0
    hardware_valid: bool = False
    stream_commit_epoch: int = 0
    streaming: bool = False
    dying: bool = False
    releasing: bool = False
    timeout_pending: bool = False
    timeout_running: bool = False
    firmware_loads: int = 0
    hardware_fingerprint: tuple[int, int, int, int] | None = None
    request_fingerprint: tuple[int, int, int, int] | None = None
    runtime_fingerprint: tuple[int, int, int, int] | None = None

    def drain_timeout_sync(self) -> None:
        """Mirror cancel_delayed_work_sync before request fields are mutated."""
        if self.timeout_running:
            self.finish_timeout()
        self.timeout_pending = False
        assert not self.timeout_running

    def arm_timeout(self) -> None:
        assert not self.timeout_pending and not self.timeout_running
        self.timeout_pending = True

    def begin_timeout(self) -> None:
        assert self.timeout_pending and not self.timeout_running
        self.timeout_pending = False
        self.timeout_running = True

    def finish_timeout(self) -> None:
        if not self.timeout_running:
            return
        self.timeout_running = False
        if (
            not self.dying
            and self.lease
            and self.power_count == 0
            and self.state in ("ready", "stale")
            and self.generation == self.lease_generation
        ):
            self.releasing = True
            self.release_lease("expired")
            self.releasing = False

    def prepare(
        self,
        succeeds: bool = True,
        generation: int = 1,
        fingerprint: tuple[int, int, int, int] = (2560, 720, 30, 3),
    ) -> None:
        self.drain_timeout_sync()
        assert not self.lease and self.power_count == 0
        self.board.get()
        self.lease = True
        self.generation = generation
        self.lease_generation = generation
        self.request_fingerprint = fingerprint
        self.runtime_fingerprint = fingerprint
        self.state = "preparing"
        if succeeds:
            self.firmware_loads += 1
            self.state = "ready"
            self.initialized_epoch = self.board.epoch
            self.hardware_valid = True
            self.hardware_fingerprint = fingerprint
            self.arm_timeout()
        else:
            self.state = "failed"
            self.lease = False
            self.board.put()

    def request_prepare(
        self, generation: int, fingerprint: tuple[int, int, int, int]
    ) -> str:
        """Model renewal, expired-current reuse, and stale rejection."""
        self.drain_timeout_sync()
        if self.dying:
            return "enodev"
        if self.streaming or self.power_count or self.releasing:
            return "ebusy"
        if (
            self.generation == generation
            and self.request_fingerprint is not None
            and self.request_fingerprint != fingerprint
        ):
            if self.lease:
                self.arm_timeout()
            return "estale"
        if self.hardware_current() and self.hardware_fingerprint != fingerprint:
            if self.lease:
                self.arm_timeout()
            return "estale"

        if not self.lease:
            self.board.get()
            self.lease = True
        self.generation = generation
        self.lease_generation = generation
        self.request_fingerprint = fingerprint
        self.runtime_fingerprint = fingerprint
        if not self.hardware_current():
            self.firmware_loads += 1
            self.hardware_valid = True
            self.initialized_epoch = self.board.epoch
            self.hardware_fingerprint = fingerprint
        self.state = "ready"
        self.arm_timeout()
        return "ready"

    def rewrite_runtime(
        self, fingerprint: tuple[int, int, int, int], active: bool = True
    ) -> None:
        if not active or self.runtime_fingerprint == fingerprint:
            return
        self.runtime_fingerprint = fingerprint
        if self.state in ("ready", "consumed"):
            self.state = "stale"

    def rearm_timeout(self, generation: int | None = None) -> bool:
        """A running old callback drains before mutable request revalidation."""
        self.drain_timeout_sync()
        if (
            self.dying
            or not self.lease
            or self.power_count != 0
            or self.state not in ("ready", "stale")
            or self.generation != self.lease_generation
        ):
            return False
        if generation is not None:
            self.generation = generation
            self.lease_generation = generation
        self.arm_timeout()
        return True

    def power_on(self) -> None:
        if self.power_count == 0:
            if self.lease:
                self.timeout_pending = False
                self.lease = False
                if self.state != "stale":
                    self.state = "consumed"
            else:
                self.board.get()
        self.power_count += 1

    def power_off(self) -> None:
        assert self.power_count > 0
        self.power_count -= 1
        if self.power_count == 0:
            self.board.put()

    def release_lease(self, state: str) -> None:
        if self.lease:
            self.timeout_pending = False
            self.lease = False
            self.state = state
            self.board.put()

    def hardware_current(self) -> bool:
        return self.hardware_valid and self.initialized_epoch == self.board.epoch

    def stream_current(self) -> bool:
        return (
            not self.dying
            and self.streaming
            and self.hardware_current()
            and self.stream_commit_epoch == self.board.epoch
        )

    def commit_stream(self, remove_before_final_gate: bool = False) -> bool:
        if remove_before_final_gate:
            self.dying = True
        self.board.epoch_guarded = True
        if self.dying:
            self.board.epoch_guarded = False
            return False
        self.streaming = True
        self.stream_commit_epoch = self.board.epoch
        self.board.epoch_guarded = False
        return True

    def stream_on_admission(
        self, request_lock_available: bool, sensor_lock_available: bool
    ) -> str:
        if self.dying:
            return "enodev"
        if (
            self.state == "preparing"
            or self.releasing
            or not request_lock_available
            or not sensor_lock_available
        ):
            return "ebusy"
        return "admitted"

    def pulse_fsync(self, invalidate_before_final_gate: bool = False) -> None:
        # An unlocked preliminary DONE/current observation is not authority.
        _preliminary = self.stream_current()
        if invalidate_before_final_gate:
            self.board.invalidate()
        self.board.epoch_guarded = True
        if self.stream_current():
            self.board.fsync_pulses += 1
        self.board.epoch_guarded = False

    def write_des_output(self, invalidate_before_final_gate: bool = False) -> None:
        _preliminary = self.stream_current()
        if invalidate_before_final_gate:
            self.board.invalidate()
        self.board.epoch_guarded = True
        if self.stream_current():
            self.board.des_writes += 1
        self.board.epoch_guarded = False

    def remove(self) -> None:
        self.dying = True
        self.drain_timeout_sync()
        if self.lease:
            self.lease = False
            self.board.put()
        elif self.power_count:
            self.power_count = 0
            self.board.put()


def check_model(failures: list[str]) -> None:
    valid_commands = (
        "0\n",
        "1 1 2560 720 30 3\n",
        "1 2 3840 1080 120 3",
        "1 3 1280 720 1 1",
        "1 4 1280 720 60 2",
        "1 5 1920 1080 30 1",
        "1 6 1920 1080 30 2",
    )
    for command in valid_commands:
        try:
            parse_prepare_command(command)
        except ValueError:
            failures.append(f"valid prepare command rejected by model: {command!r}")

    invalid_commands = (
        "",
        "00",
        "0 1",
        "1",
        "1 1 2560 720 30",
        "1 1 2560 720 30 3 trailing",
        "1 0 2560 720 30 3",
        "1 -1 2560 720 30 3",
        "1 +1 2560 720 30 3",
        "1 18446744073709551616 2560 720 30 3",
        "1 1 2560 720 0 3",
        "1 1 2560 720 121 3",
        "1 1 2560 720 30 1",
        "1 1 1280 720 30 3",
        "1 1 640 480 30 1",
        "1 1 2560 720 30 3\n2",
        "1 1 2560 720 30 3\x00ignored",
    )
    for command in invalid_commands:
        try:
            parse_prepare_command(command)
        except ValueError:
            pass
        else:
            failures.append(f"invalid prepare command accepted by model: {command!r}")

    board = Board()
    cam = Camera(board)
    cam.prepare()
    if (board.users, board.resets, cam.lease) != (1, 1, True):
        failures.append("prepare must own one global user and run first reset")
    cam.power_on()
    if (board.users, cam.power_count, cam.lease, cam.state) != (
        1,
        1,
        False,
        "consumed",
    ):
        failures.append("first s_power must transfer, not duplicate, the lease")
    cam.power_on()
    cam.power_off()
    cam.power_off()
    if board.users != 0:
        failures.append("last local close must return exactly one global user")

    board = Board()
    left, right = Camera(board), Camera(board)
    left.prepare()
    right.prepare()
    if (board.users, board.resets) != (2, 1):
        failures.append("two prepares must share one reset and own two users")
    left.begin_timeout()
    left.finish_timeout()
    if not left.hardware_current():
        failures.append("peer-retained power must preserve expired hardware validity")
    right.power_on()
    if board.users != 1:
        failures.append("expiry must not power off a peer-owned camera domain")
    right.power_off()
    if left.hardware_current():
        failures.append("final power-off epoch must invalidate expired hardware")

    board = Board()
    failed = Camera(board)
    failed.prepare(succeeds=False)
    if board.users != 0 or failed.lease:
        failures.append("failed prepare must return its unused lease")

    board = Board()
    renewed = Camera(board)
    renewed.prepare(generation=11)
    if not renewed.rearm_timeout(generation=12) or board.users != 1:
        failures.append("pending expiry must drain and rearm without duplicating ownership")
    if (renewed.generation, renewed.lease_generation) != (12, 12):
        failures.append("renewal must publish new generation only after old work drains")
    renewed.begin_timeout()  # old callback is now running and waiting for the lock
    if renewed.rearm_timeout(generation=13):
        failures.append("renewal must revalidate after a running old callback drains")
    renewed.prepare(generation=13)
    if renewed.timeout_running or not renewed.timeout_pending or board.users != 1:
        failures.append("old callback must be gone before a renewed lease is published")

    preparing = Camera(Board())
    preparing.state = "preparing"
    if preparing.stream_on_admission(False, False) != "ebusy":
        failures.append("STREAMON must return busy instead of waiting through prepare")
    preparing.releasing = True
    preparing.state = "ready"
    if preparing.stream_on_admission(True, False) != "ebusy":
        failures.append("STREAMON must return busy instead of waiting through release")
    preparing.releasing = False
    if preparing.stream_on_admission(False, True) != "ebusy":
        failures.append("STREAMON must be gated during drain-before-PREPARING window")
    preparing.dying = True
    if preparing.stream_on_admission(False, False) != "enodev":
        failures.append("dying must take precedence over contended STREAMON")

    removed_before_commit = Camera(Board())
    removed_before_commit.hardware_valid = True
    removed_before_commit.initialized_epoch = removed_before_commit.board.epoch
    if removed_before_commit.commit_stream(remove_before_final_gate=True):
        failures.append("removal racing final STREAMON admission must prevent commit")

    gated = Camera(Board())
    gated.prepare()
    gated.power_on()
    gated.commit_stream()
    gated.pulse_fsync()
    gated.write_des_output()
    if (gated.board.fsync_pulses, gated.board.des_writes) != (1, 1):
        failures.append("current committed stream must reach FSYNC and DES output")
    gated.pulse_fsync(invalidate_before_final_gate=True)
    gated.write_des_output()
    if (gated.board.fsync_pulses, gated.board.des_writes) != (1, 1):
        failures.append("stale DONE bits after epoch advance must produce no writes")

    board = Board()
    cancelled = Camera(board)
    cancelled.prepare()
    cancelled.release_lease("idle")
    if board.users != 0:
        failures.append("prepare=0 must return an unused lease")

    board = Board()
    leased = Camera(board)
    leased.prepare()
    leased.remove()
    if board.users != 0:
        failures.append("remove must reconcile a driver-owned lease")

    board = Board()
    owned = Camera(board)
    owned.power_on()
    owned.remove()
    if board.users != 0:
        failures.append("remove must reconcile a V4L2-owned power reference")

    board = Board()
    first = Camera(board)
    tuple_a = (1280, 720, 30, 1)
    tuple_b = (1280, 720, 30, 2)
    if first.request_prepare(41, tuple_a) != "ready":
        failures.append("fresh valid prepare request must reach READY")
    first_loads = first.firmware_loads
    if first.request_prepare(41, tuple_a) != "ready":
        failures.append("same generation and tuple must be idempotent")
    if first.request_prepare(42, tuple_a) != "ready":
        failures.append("new generation with the same tuple must renew")
    if first.firmware_loads != first_loads or board.users != 1:
        failures.append("same-hardware renewal must not reload or duplicate power")
    if first.request_prepare(42, tuple_b) != "estale":
        failures.append("one generation must not be rebound to another tuple")
    if first.request_prepare(43, tuple_b) != "estale":
        failures.append("same-epoch hardware tuple switch must be ESTALE")
    if not first.timeout_pending:
        failures.append("rejected mismatch must preserve the old lease expiry")

    board = Board()
    keeper, expired = Camera(board), Camera(board)
    keeper.prepare(generation=50)
    expired.prepare(generation=51, fingerprint=tuple_a)
    expired.begin_timeout()
    expired.finish_timeout()
    expired_loads = expired.firmware_loads
    resets = board.resets
    if expired.request_prepare(52, tuple_a) != "ready":
        failures.append("expired current hardware must be reusable while a peer owns power")
    if (
        expired.firmware_loads != expired_loads
        or board.resets != resets
        or board.users != 2
    ):
        failures.append("expired-current reuse must acquire one user without reset/reload")

    unchanged = Camera(Board())
    unchanged.prepare(generation=60, fingerprint=tuple_a)
    unchanged.rewrite_runtime(tuple_a)
    if unchanged.state != "ready":
        failures.append("identical ACTIVE rewrite must preserve prepare readiness")
    unchanged.rewrite_runtime(tuple_b, active=False)
    if unchanged.state != "ready":
        failures.append("TRY format must not invalidate prepare readiness")
    unchanged.rewrite_runtime(tuple_b)
    if unchanged.state != "stale":
        failures.append("changed ACTIVE hardware value must mark prepare stale")
    unchanged.power_on()
    if unchanged.state != "stale":
        failures.append("lease transfer must not erase a detected stale tuple")


def function(source: str, name: str) -> str:
    start = -1
    for return_type in ("int", "void", "bool", "ssize_t"):
        for annotation in ("", "__maybe_unused "):
            start = source.find(f"static {return_type} {annotation}{name}(")
            if start >= 0:
                break
        if start >= 0:
            break
    if start < 0:
        return ""
    next_static = source.find("\nstatic ", start + 8)
    return source[start:] if next_static < 0 else source[start:next_static]


def code_only(source: str) -> str:
    """Remove comments and literals so register checks match executable code."""
    token = re.compile(
        r"/\*.*?\*/|//[^\n]*|\"(?:\\.|[^\"\\])*\"|'(?:\\.|[^'\\])*'",
        re.S,
    )
    return token.sub(lambda match: "\n" * match.group(0).count("\n"), source)


def check_source(source: str, failures: list[str]) -> None:
    code = code_only(source)
    required = (
        "enum max9296_prepare_request_state",
        "struct max9296_hw_fingerprint",
        "struct delayed_work prepare_lease_timeout",
        "bool prepare_lease_held",
        "bool hardware_valid",
        "u64 prepare_generation",
        "u64 initialized_epoch",
        "u64 stream_commit_epoch",
        "bool prepare_releasing",
        "max9296_prepare_hardware_locked",
        "max9296_prepare_matches_locked",
        "max9296_prepare_lease_timeout",
        "sysfs_prepare_show",
        "sysfs_prepare_store",
        "static DEVICE_ATTR(prepare, 0664",
        "max9296_hw_epoch",
    )
    for token in required:
        if token not in code:
            failures.append(f"missing prepare contract token: {token}")

    stream = function(code, "max9296_s_stream")
    show = function(code, "sysfs_prepare_show")
    store = function(code, "sysfs_prepare_store")
    parser = function(code, "max9296_parse_prepare_command")
    cancel_prepare = function(code, "max9296_cancel_prepare")
    power = function(code, "max9296_s_power")
    timeout = function(code, "max9296_prepare_lease_timeout")
    remove = function(code, "max9296_remove")
    loadfw = function(code, "max9296_loadfw")
    request_locked = function(code, "max9296_prepare_request_locked")
    request = function(code, "max9296_prepare_request")
    rearm = function(code, "max9296_rearm_prepare_lease")
    can_arm = function(code, "max9296_prepare_lease_can_arm_locked")
    epoch_gate = function(code, "max9296_stream_epoch_current")
    fsync_pulse = function(code, "max9296_fsync_pulse_current")
    output_gate = function(code, "max9296_enable_output_locked")
    output_disable = function(code, "max9296_disable_stream_mipi")
    set_fmt = function(code, "max9296_set_fmt")
    set_interval = function(code, "max9296_s_frame_interval")
    enable_store = function(code, "sysfs_enable_store")
    probe = function(code, "max9296_probe")

    request_scaffolding = (
        "MAX9296_PREP_IDLE",
        "MAX9296_PREP_PREPARING",
        "MAX9296_PREP_READY",
        "MAX9296_PREP_STALE",
        "MAX9296_PREP_CONSUMED",
        "MAX9296_PREP_FAILED",
        "MAX9296_PREP_EXPIRED",
        "prepare_state",
        "prepare_lease_generation",
        "prepare_errno",
        "bool dying",
        "bool prepare_releasing",
    )
    for token in request_scaffolding:
        if token not in source:
            failures.append(f"missing Task 3 request scaffolding: {token}")

    for token in (
        "max9296_set_power(sensor, true)",
        "max9296_prepare_hardware_locked",
        "prepare_lease_held = true",
        "MAX9296_PREP_READY",
    ):
        if token not in request_locked:
            failures.append(f"prepare request ownership transition missing: {token}")

    if "mod_delayed_work" in code:
        failures.append("mutable delayed work must not be requeued with mod_delayed_work")

    for name, body in (("request", request), ("renewal", rearm)):
        if name == "request":
            request_lock = body.find(
                "mutex_trylock(&sensor->prepare_request_lock)"
            )
        else:
            request_lock = body.find("mutex_lock(&sensor->prepare_request_lock)")
        cancel = body.find("cancel_delayed_work_sync(&sensor->prepare_lease_timeout)")
        lock = body.find("mutex_lock(&sensor->lock)")
        if name == "request":
            prepared = body.find("max9296_prepare_request_locked")
            revalidate = body.find(
                "max9296_prepare_lease_can_arm_locked", prepared
            )
            arm = body.find("queue_delayed_work", revalidate)
        else:
            prepared = body.find("sensor->prepare_generation = generation")
            revalidate = body.find(
                "max9296_prepare_lease_can_arm_locked", prepared
            )
            arm = body.find("queue_delayed_work", revalidate)
        unlock = body.rfind("mutex_unlock(&sensor->lock)")
        request_unlock = body.rfind(
            "mutex_unlock(&sensor->prepare_request_lock)"
        )
        if not (
            0 <= request_lock < cancel < lock < prepared < revalidate < arm
            < unlock < request_unlock
        ):
            failures.append(
                f"{name} must hold request admission across sync-drain, "
                "sensor revalidation, and final arm"
            )
        if "60 * HZ" not in body:
            failures.append(f"{name} does not arm the 60-second lease timeout")

    generation_write = rearm.find("sensor->prepare_generation = generation")
    lease_generation_write = rearm.find(
        "sensor->prepare_lease_generation = generation"
    )
    renewal_revalidate = rearm.find("max9296_prepare_lease_can_arm_locked")
    renewal_arm = rearm.find("queue_delayed_work")
    if not (
        0 <= generation_write < lease_generation_write < renewal_revalidate < renewal_arm
    ):
        failures.append("renewal generation must be mutated only post-drain and pre-arm")

    for token in (
        "prepare_lease_held",
        "power_count == 0",
        "MAX9296_PREP_READY",
        "MAX9296_PREP_STALE",
        "prepare_generation",
        "prepare_lease_generation",
        "sensor->dying",
    ):
        if token not in can_arm:
            failures.append(f"post-drain lease revalidation missing: {token}")

    for token in (
        "INIT_DELAYED_WORK(&sensor->prepare_lease_timeout",
        "MAX9296_PREP_IDLE",
    ):
        if token not in probe:
            failures.append(f"probe lease initialization missing: {token}")

    if "max9296_prepare_hardware_locked" not in stream:
        failures.append("legacy s_stream does not use the common prepare helper")
    if "max9296_prepare_matches_locked" not in stream:
        failures.append("s_stream has no prepared fingerprint fast path")
    for token in ("initialized_epoch", "max9296_hw_epoch", "-ESTALE"):
        if token not in stream:
            failures.append(f"s_stream decision table missing: {token}")
    if "max9296_prepare_request" not in store:
        failures.append("sysfs prepare does not use the common prepare helper")

    for token in (
        "kmemdup_nul",
        "memchr",
        "strsep",
        "kstrtoull",
        "generation == 0",
        "fps < 1",
        "fps > 120",
        "MAX9296_MODE_2560x720",
        "MAX9296_MODE_1280x720",
        "MAX9296_MODE_3840x1080",
        "MAX9296_MODE_1920x1080",
        "-EINVAL",
    ):
        if token not in parser:
            failures.append(f"strict prepare parser/tuple validation missing: {token}")
    if "MEDIA_BUS_FMT_UYVY8_2X8" not in store:
        failures.append("prepare command must derive the sole UYVY media-bus code")

    if not re.search(
        r"prepare_lease_held\s*=\s*false.*power_count\+\+", power, re.S
    ):
        failures.append("s_power has no atomic lease-to-local-owner transfer")
    if "prepare_state != MAX9296_PREP_STALE" not in power:
        failures.append("s_power lease transfer erases a changed prepared tuple")
    if "WARN_ON(sensor->power_count <= 0)" not in power:
        failures.append("s_power does not reject local power-count underflow")
    if "sensor->dying" not in power or "-ENODEV" not in power:
        failures.append("s_power remains open after remove admission is withdrawn")

    if "MAX9296_PREP_PREPARING" in timeout:
        failures.append("lease timeout must never abort PREPARING hardware I/O")
    if "max9296_set_power(sensor, false)" not in timeout:
        failures.append("lease timeout does not return the board-global user")
    for token in (
        "max9296_prepare_lease_can_arm_locked",
        "prepare_lease_generation",
        "prepare_lease_held = false",
        "MAX9296_PREP_EXPIRED",
        "prepare_releasing",
    ):
        if token not in timeout:
            failures.append(f"lease timeout recheck missing: {token}")

    if "cancel_delayed_work_sync(&sensor->prepare_lease_timeout)" not in remove:
        failures.append("remove does not synchronously drain lease expiry")
    else:
        cancel = remove.find(
            "cancel_delayed_work_sync(&sensor->prepare_lease_timeout)"
        )
        before = remove[:cancel]
        if before.rfind("mutex_lock(&sensor->lock)") > before.rfind(
            "mutex_unlock(&sensor->lock)"
        ):
            failures.append("remove sync-cancels timeout while holding sensor lock")

    for token in (
        "WRITE_ONCE(sensor->dying, true)",
        "WRITE_ONCE(sensor->shared.probe_ready, false)",
        "prepare_lease_held",
        "power_count > 0",
        "max9296_power_users--",
        "max9296_hw_epoch++",
    ):
        if token not in remove:
            failures.append(f"remove lease reconciliation missing: {token}")

    for attr_lifecycle in (
        "device_create_file(&client->dev, &dev_attr_prepare)",
        "device_remove_file(&client->dev, &dev_attr_prepare)",
    ):
        if attr_lifecycle not in source:
            failures.append(f"prepare sysfs lifecycle missing: {attr_lifecycle}")

    for callback_name, callback in (("show", show), ("store", store)):
        for admission in (
            "READ_ONCE(sensor->shared.probe_ready)",
            "sensor->dying",
            "-EAGAIN",
            "-ENODEV",
        ):
            if admission not in callback:
                failures.append(
                    f"prepare {callback_name} callback admission missing: {admission}"
                )

    cancel_request_lock = cancel_prepare.find(
        "mutex_trylock(&sensor->prepare_request_lock)"
    )
    cancel_sync = cancel_prepare.find(
        "cancel_delayed_work_sync(&sensor->prepare_lease_timeout)"
    )
    cancel_sensor_lock = cancel_prepare.find("mutex_lock(&sensor->lock)")
    cancel_release = cancel_prepare.find("max9296_set_power(sensor, false)")
    if not (
        0 <= cancel_request_lock < cancel_sync < cancel_sensor_lock < cancel_release
    ):
        failures.append("prepare=0 must sync-drain outside sensor lock before release")
    for token in (
        "sensor->streaming",
        "sensor->power_count > 0",
        "prepare_lease_held",
        "prepare_lease_held = false",
        "-EBUSY",
    ):
        if token not in cancel_prepare:
            failures.append(f"prepare=0 ownership guard missing: {token}")

    for name, body in (
        ("ACTIVE format", set_fmt),
        ("ACTIVE fps", set_interval),
        ("enable", enable_store),
    ):
        if "max9296_mark_prepare_stale_locked" not in body:
            failures.append(f"{name} rewrite cannot mark a changed request stale")
    if set_fmt.find("V4L2_SUBDEV_FORMAT_TRY") > set_fmt.find(
        "max9296_mark_prepare_stale_locked"
    ):
        failures.append("TRY format can reach prepare invalidation")
    enable_lock = enable_store.find("mutex_lock(&sensor->lock)")
    enable_write = enable_store.find("sensor->enable =")
    enable_unlock = enable_store.find("mutex_unlock(&sensor->lock)")
    if not (0 <= enable_lock < enable_write < enable_unlock):
        failures.append("enable sysfs mutation is not protected by sensor->lock")

    admission_start = stream.find("if (enable) {")
    request_trylock = stream.find("mutex_trylock(&sensor->prepare_request_lock)")
    trylock = stream.find("mutex_trylock(&sensor->lock)")
    if not (0 <= admission_start < request_trylock < trylock) or "mutex_lock(&sensor->lock)" in stream[admission_start:trylock]:
        failures.append("STREAMON can wait through a long PREPARING/releasing owner")
    for token in (
        "READ_ONCE(sensor->dying)",
        "READ_ONCE(sensor->prepare_state) == MAX9296_PREP_PREPARING",
        "READ_ONCE(sensor->prepare_releasing)",
        "mutex_trylock(&sensor->prepare_request_lock)",
        "mutex_trylock(&sensor->lock)",
        "-ENODEV",
        "-EBUSY",
    ):
        if token not in stream:
            failures.append(f"s_stream nonblocking admission missing: {token}")
    action = stream.find("max9296_normalize_fingerprint_locked")
    locked_dying = stream.find("sensor->dying", trylock)
    locked_preparing = stream.find(
        "sensor->prepare_state == MAX9296_PREP_PREPARING", trylock
    )
    locked_releasing = stream.find("sensor->prepare_releasing", trylock)
    if not (
        0 <= trylock < locked_dying < action
        and trylock < locked_preparing < action
        and trylock < locked_releasing < action
    ):
        failures.append("s_stream does not revalidate admission after acquiring the lock")
    final_power_lock = stream.find("mutex_lock(&max9296_power_lock)", action)
    final_dying = stream.find("READ_ONCE(sensor->dying)", final_power_lock)
    final_commit = stream.find("max9296_stream_commit_locked(sensor)")
    final_streaming = stream.find("sensor->streaming = true", final_commit)
    final_power_unlock = stream.find("mutex_unlock(&max9296_power_lock)", final_commit)
    if not (
        0 <= final_power_lock < final_dying < final_commit < final_streaming
        < final_power_unlock
    ):
        failures.append("final STREAMON revalidation and commit are not removal-atomic")
    sensor_unlock = stream.rfind("mutex_unlock(&sensor->lock)")
    request_unlock = stream.rfind("mutex_unlock(&sensor->prepare_request_lock)")
    if not (0 <= sensor_unlock < request_unlock):
        failures.append("STREAMON releases its request admission gate too early")

    fsync = function(code, "max9296_fsync")
    enable = function(code, "max9296_enable")
    for gate in (
        "sensor->streaming",
        "hardware_valid",
        "initialized_epoch",
        "stream_commit_epoch",
        "max9296_hw_epoch",
    ):
        if gate not in epoch_gate:
            failures.append(f"epoch stream predicate missing: {gate}")
    for name, body in (("fsync", fsync), ("enable", enable)):
        if "max9296_stream_epoch_current" not in body:
            failures.append(f"{name} lacks current-epoch stream gate")

    actual_pulses = re.findall(
        r"gpiod_set_value_cansleep\s*\([^;]*fsync_gpio[^;]*;", code, re.S
    )
    helper_pulses = re.findall(
        r"gpiod_set_value_cansleep\s*\([^;]*fsync_gpio[^;]*;", fsync_pulse, re.S
    )
    if len(actual_pulses) != 2 or actual_pulses != helper_pulses:
        failures.append("every actual FSYNC edge must be centralized in the final gate")
    pulse_values = []
    for pulse in helper_pulses:
        match = re.search(r",\s*([01])\s*\)\s*;\s*$", pulse)
        pulse_values.append(int(match.group(1)) if match else None)
    if pulse_values != [1, 0]:
        failures.append("FSYNC final gate must emit exactly one high then one low edge")
    power_lock = fsync_pulse.find("mutex_lock(&max9296_power_lock)")
    primary_gate = fsync_pulse.find("max9296_stream_epoch_current(primary)")
    secondary_gate = fsync_pulse.find("max9296_stream_epoch_current(secondary)")
    owner_dying_gate = fsync_pulse.find("READ_ONCE(gpio_owner->dying)")
    dying_gate = fsync_pulse.find("READ_ONCE(primary->dying)")
    first_pulse = fsync_pulse.find("gpiod_set_value_cansleep")
    last_pulse = fsync_pulse.rfind("gpiod_set_value_cansleep")
    power_unlock = fsync_pulse.find("mutex_unlock(&max9296_power_lock)")
    if not (
        0 <= power_lock < owner_dying_gate < dying_gate < primary_gate < first_pulse
        and power_lock < secondary_gate < first_pulse
        and first_pulse < last_pulse < power_unlock
    ):
        failures.append("FSYNC final predicates and full pulse are not epoch-atomic")
    if fsync.count("max9296_fsync_pulse_current") != 4:
        failures.append(
            "each single, dual, local-only, and peer-only FSYNC branch needs the final helper"
        )
    if not re.search(
        r"max9296_fsync_pulse_current\s*\(\s*sensor\s*,\s*"
        r"sensor->shared\.sensor\s*,\s*NULL\s*,",
        fsync,
        re.S,
    ):
        failures.append("peer-only FSYNC must keep the local GPIO owner separate")

    output_write_pattern = re.compile(
        r"maxim_ops_i2c_write\s*\([^;]*\b0x0313\b\s*,\s*"
        r"(?P<value>0x[0-9a-fA-F]+)\b[^;]*;",
        re.S,
    )
    all_output_writes = list(output_write_pattern.finditer(code))
    nonzero_output_writes = [
        match.group(0)
        for match in all_output_writes
        if int(match.group("value"), 16) != 0
    ]
    zero_output_writes = [
        match.group(0)
        for match in all_output_writes
        if int(match.group("value"), 16) == 0
    ]
    helper_output_writes = [
        match.group(0) for match in output_write_pattern.finditer(output_gate)
    ]
    disable_output_writes = [
        match.group(0) for match in output_write_pattern.finditer(output_disable)
    ]
    if (
        len(all_output_writes) != 3
        or nonzero_output_writes != helper_output_writes
        or zero_output_writes != disable_output_writes
        or len(helper_output_writes) != 2
        or len(disable_output_writes) != 1
    ):
        failures.append(
            "all executable DES 0x0313 writes must be either gated nonzero "
            "enables or the sole stream-off disable"
        )
    table_output_pattern = re.compile(
        r"\{\s*0x00\s*,\s*0x0313\s*,\s*\d+\s*,\s*"
        r"(?P<value>0x[0-9a-fA-F]+)\s*,\s*\d+\s*,\s*\d+\s*\}"
    )
    table_output_writes = list(table_output_pattern.finditer(code))
    if len(table_output_writes) != 3 or any(
        int(match.group("value"), 16) != 0 for match in table_output_writes
    ):
        failures.append("every 0x0313 register-table entry must remain output-disabled")
    if code.count("0x0313") != len(all_output_writes) + len(table_output_writes):
        failures.append("an executable 0x0313 occurrence escaped structural classification")
    held_assert = output_gate.find("lockdep_assert_held(&sensor->lock)")
    output_power_lock = output_gate.find("mutex_lock(&max9296_power_lock)")
    output_dying = output_gate.find("sensor->dying")
    output_epoch = output_gate.find("max9296_stream_epoch_current(sensor)")
    first_output = output_gate.find("0x0313")
    last_output = output_gate.rfind("0x0313")
    output_power_unlock = output_gate.find("mutex_unlock(&max9296_power_lock)")
    if not (
        0 <= held_assert < output_power_lock < output_dying < output_epoch < first_output
        and first_output < last_output < output_power_unlock
    ):
        failures.append("DES writes lack the sensor-locked, epoch-atomic final gate")
    enable_lock = enable.find("mutex_lock(&sensor->lock)")
    output_call = enable.find("max9296_enable_output_locked(sensor)")
    enable_unlock = enable.find("mutex_unlock(&sensor->lock)", output_call)
    if not (0 <= enable_lock < output_call < enable_unlock):
        failures.append("DES output helper is not called while holding sensor->lock")
    disable_power_lock = stream.find("mutex_lock(&max9296_power_lock)", final_power_unlock)
    streaming_false = stream.find("sensor->streaming = false", disable_power_lock)
    disable_call = stream.find("max9296_disable_stream_mipi(sensor)", streaming_false)
    disable_power_unlock = stream.find(
        "mutex_unlock(&max9296_power_lock)", disable_call
    )
    if not (
        0 <= disable_power_lock < streaming_false < disable_call
        < disable_power_unlock
    ):
        failures.append("STREAMOFF publication and output disable are not pulse-atomic")

    remove_power_lock = remove.find("mutex_lock(&max9296_power_lock)")
    remove_dying = remove.find("WRITE_ONCE(sensor->dying, true)")
    remove_power_unlock = remove.find("mutex_unlock(&max9296_power_lock)")
    unregister = remove.find("v4l2_async_unregister_subdev")
    request_lock = remove.find("mutex_lock(&sensor->prepare_request_lock)")
    cancel_work = remove.find("cancel_delayed_work_sync")
    reconcile_lock = remove.find("mutex_lock(&sensor->lock)", cancel_work)
    request_unlock = remove.find("mutex_unlock(&sensor->prepare_request_lock)")
    if not (
        0 <= remove_power_lock < remove_dying < remove_power_unlock < unregister
        < request_lock < cancel_work < reconcile_lock < request_unlock
    ):
        failures.append("remove does not atomically withdraw, drain, then reconcile")

    remove_prepare_attr = remove.find(
        "device_remove_file(&client->dev, &dev_attr_prepare)"
    )
    if not (0 <= remove_dying < remove_prepare_attr < unregister < request_lock):
        failures.append(
            "normal remove must close admission, drain prepare sysfs, unregister, "
            "then drain expiry"
        )

    create_prepare = probe.find(
        "device_create_file(&client->dev, &dev_attr_prepare)"
    )
    register_subdev = probe.find("v4l2_async_register_subdev_sensor_common")
    remove_prepare = probe.find(
        "device_remove_file(&client->dev, &dev_attr_prepare)", register_subdev
    )
    remove_health = probe.find(
        "device_remove_file(&client->dev, &dev_attr_health_raw)", register_subdev
    )
    if not (
        0 <= create_prepare < register_subdev < remove_prepare < remove_health
    ):
        failures.append("prepare attribute probe setup/unwind is not reverse ordered")

    if "kthread_run(max9296_fw_init" in stream:
        failures.append("s_stream still creates the serial firmware waiter thread")
    if "sensor->state.firmware = MAX9296_STATE_DONE" in function(
        code, "max9296_fw_work_handler"
    ):
        failures.append("firmware worker still reports failure as DONE")
    if loadfw and loadfw.count("release_firmware(fw)") < 3:
        failures.append("firmware error exits do not all release the image")

    status_fields = (
        "state=%s generation=%llu epoch=%llu",
        "mode=%s table=%s width=%u height=%u fps=%u code=0x%x enable=%u ",
        "errno=%d lease=%u match=%u\\n",
    )
    if any(field not in source for field in status_fields):
        failures.append("prepare read ABI has no stable key=value status line")


def main() -> int:
    failures: list[str] = []
    check_model(failures)
    check_source(SOURCE.read_text(encoding="utf-8"), failures)
    if failures:
        for failure in failures:
            print(f"FAIL: {failure}", file=sys.stderr)
        return 1
    print("PASS: parallel prepare ABI and power ownership contract")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
