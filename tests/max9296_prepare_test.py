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


@dataclass
class Board:
    users: int = 0
    epoch: int = 0
    resets: int = 0

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


@dataclass
class Camera:
    board: Board
    state: str = "idle"
    lease: bool = False
    power_count: int = 0
    generation: int = 0
    initialized_epoch: int = 0
    hardware_valid: bool = False

    def prepare(self, succeeds: bool = True, generation: int = 1) -> None:
        assert not self.lease and self.power_count == 0
        self.board.get()
        self.lease = True
        self.generation = generation
        self.state = "preparing"
        if succeeds:
            self.state = "ready"
            self.initialized_epoch = self.board.epoch
            self.hardware_valid = True
        else:
            self.state = "failed"
            self.lease = False
            self.board.put()

    def power_on(self) -> None:
        if self.power_count == 0:
            if self.lease:
                self.lease = False
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
            self.lease = False
            self.state = state
            self.board.put()

    def hardware_current(self) -> bool:
        return self.hardware_valid and self.initialized_epoch == self.board.epoch

    def remove(self) -> None:
        if self.lease:
            self.lease = False
            self.board.put()
        elif self.power_count:
            self.power_count = 0
            self.board.put()


def check_model(failures: list[str]) -> None:
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
    left.release_lease("expired")
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


def function(source: str, name: str) -> str:
    start = source.find(f"static int {name}(")
    if start < 0:
        start = source.find(f"static void {name}(")
    if start < 0:
        return ""
    next_static = source.find("\nstatic ", start + 8)
    return source[start:] if next_static < 0 else source[start:next_static]


def check_source(source: str, failures: list[str]) -> None:
    required = (
        "enum max9296_prepare_request_state",
        "struct max9296_hw_fingerprint",
        "struct delayed_work prepare_lease_timeout",
        "bool prepare_lease_held",
        "bool hardware_valid",
        "u64 prepare_generation",
        "u64 initialized_epoch",
        "u64 stream_commit_epoch",
        "max9296_prepare_hardware_locked",
        "max9296_prepare_matches_locked",
        "max9296_prepare_lease_timeout",
        "sysfs_prepare_show",
        "sysfs_prepare_store",
        "static DEVICE_ATTR(prepare, 0664",
        "max9296_hw_epoch",
    )
    for token in required:
        if token not in source:
            failures.append(f"missing prepare contract token: {token}")

    stream = function(source, "max9296_s_stream")
    store = function(source, "sysfs_prepare_store")
    power = function(source, "max9296_s_power")
    timeout = function(source, "max9296_prepare_lease_timeout")
    remove = function(source, "max9296_remove")
    loadfw = function(source, "max9296_loadfw")

    if "max9296_prepare_hardware_locked" not in stream:
        failures.append("legacy s_stream does not use the common prepare helper")
    if "max9296_prepare_matches_locked" not in stream:
        failures.append("s_stream has no prepared fingerprint fast path")
    for token in ("initialized_epoch", "max9296_hw_epoch", "-ESTALE"):
        if token not in stream:
            failures.append(f"s_stream decision table missing: {token}")
    if "max9296_prepare_hardware_locked" not in store:
        failures.append("sysfs prepare does not use the common prepare helper")

    if not re.search(
        r"prepare_lease_held\s*=\s*false.*power_count\+\+", power, re.S
    ):
        failures.append("s_power has no atomic lease-to-local-owner transfer")
    if "WARN_ON(sensor->power_count <= 0)" not in power:
        failures.append("s_power does not reject local power-count underflow")

    if "MAX9296_PREP_PREPARING" in timeout:
        failures.append("lease timeout must never abort PREPARING hardware I/O")
    if "max9296_set_power(sensor, false)" not in timeout:
        failures.append("lease timeout does not return the board-global user")

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

    for attr_lifecycle in (
        "device_create_file(&client->dev, &dev_attr_prepare)",
        "device_remove_file(&client->dev, &dev_attr_prepare)",
    ):
        if attr_lifecycle not in source:
            failures.append(f"prepare sysfs lifecycle missing: {attr_lifecycle}")

    for admission in (
        "READ_ONCE(sensor->shared.probe_ready)",
        "sensor->dying",
        "-EAGAIN",
        "-ENODEV",
    ):
        if admission not in store:
            failures.append(f"prepare callback admission missing: {admission}")

    fsync = function(source, "max9296_fsync")
    enable = function(source, "max9296_enable")
    for name, body in (("fsync", fsync), ("enable", enable)):
        for gate in ("sensor->streaming", "initialized_epoch", "stream_commit_epoch"):
            if gate not in body:
                failures.append(f"{name} lacks current-epoch stream gate: {gate}")

    if "kthread_run(max9296_fw_init" in stream:
        failures.append("s_stream still creates the serial firmware waiter thread")
    if "sensor->state.firmware = MAX9296_STATE_DONE" in function(
        source, "max9296_fw_work_handler"
    ):
        failures.append("firmware worker still reports failure as DONE")
    if loadfw and loadfw.count("release_firmware(fw)") < 3:
        failures.append("firmware error exits do not all release the image")

    if "state=%s generation=%llu epoch=%llu" not in source or "lease=%u match=%u" not in source:
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
