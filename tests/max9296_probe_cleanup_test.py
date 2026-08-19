#!/usr/bin/env python3
"""Static regression checks for MAX9296 probe resource ownership."""

from __future__ import annotations

import sys
from dataclasses import dataclass, field
from pathlib import Path


ROOT = Path(__file__).resolve().parents[1]
SOURCE = ROOT / "max9296.c"


@dataclass
class PeerEndpoint:
    name: str
    declared: PeerEndpoint | None = None
    published: PeerEndpoint | None = None
    ready: bool = True
    clientdata_live: bool = True
    workers_running: bool = True
    freed: bool = False
    stale_raw_uses: int = 0
    fsync_owner: bool = False
    fsync_worker_live: bool = False
    enable_worker_live: bool = True
    worker_errno: int = 0

    def refresh_worker_status(self) -> None:
        if not self.enable_worker_live:
            self.worker_errno = self.worker_errno or -12
            return
        owner = self if self.fsync_owner else self.published
        if owner is None or not owner.fsync_owner:
            if self.worker_errno in (0, -11):
                self.worker_errno = -19
        elif not owner.fsync_worker_live:
            if owner.worker_errno not in (0, -11):
                self.worker_errno = owner.worker_errno
            elif self.worker_errno in (0, -11):
                self.worker_errno = -19
        else:
            self.worker_errno = 0

    def stream_on(self) -> int:
        return self.worker_errno

    def worker_step(self) -> None:
        if not self.workers_running or self.published is None:
            return
        if self.published.freed:
            self.stale_raw_uses += 1


@dataclass
class SerializedRemoval:
    active: bool = False
    history: list[str] = field(default_factory=list)

    def remove(
        self,
        endpoint: PeerEndpoint,
        restart_fsync_succeeds: bool = True,
    ) -> None:
        """Model lookup by the declared client, not the local raw pointer."""
        assert not self.active
        self.active = True
        self.history.append(f"enter:{endpoint.name}")

        peer = endpoint.declared
        if peer is not None and (not peer.clientdata_live or not peer.ready):
            peer = None
        endpoint.ready = False

        if peer is not None and peer.published is endpoint:
            if not peer.worker_errno:
                peer.worker_errno = -11
            stopped_fsync = peer.fsync_owner and peer.fsync_worker_live
            if stopped_fsync:
                peer.fsync_worker_live = False
            peer.published = None

            if stopped_fsync:
                if restart_fsync_succeeds:
                    peer.fsync_worker_live = True
                else:
                    peer.worker_errno = -12
            peer.workers_running = (
                (not peer.fsync_owner or peer.fsync_worker_live)
                and peer.enable_worker_live
            )
            if peer.worker_errno == -11 or stopped_fsync:
                peer.refresh_worker_status()

        endpoint.clientdata_live = False
        endpoint.freed = True
        self.history.append(f"exit:{endpoint.name}")
        self.active = False


def publish_reciprocal(endpoint: PeerEndpoint) -> bool:
    peer = endpoint.declared
    if (
        not endpoint.ready
        or not endpoint.clientdata_live
        or peer is None
        or not peer.ready
        or not peer.clientdata_live
        or peer.declared is not endpoint
    ):
        return False
    endpoint.published = peer
    peer.published = endpoint
    endpoint.refresh_worker_status()
    peer.refresh_worker_status()
    return True


def function(source: str, name: str) -> str:
    signatures = (
        f"static int {name}(",
        f"static ssize_t {name}(",
        f"static void {name}(",
        f"static bool {name}(",
        f"static struct max9296_dev *{name}(",
    )
    for signature in signatures:
        start = source.find(signature)
        if start >= 0:
            next_static = source.find("\nstatic ", start + 8)
            return source[start:] if next_static < 0 else source[start:next_static]
    return ""


def main() -> int:
    source = SOURCE.read_text(encoding="utf-8")
    shared_init_start = source.index("static int max9296_shared_init(")
    shared_cleanup_start = source.find("static void max9296_release_probe_shared(")
    probe_start = source.index("static int max9296_probe(")
    probe_end = source.index("static int max9296_remove(", probe_start)
    probe = source[probe_start:probe_end]
    shared_init = source[shared_init_start:shared_cleanup_start]
    declared_peer = function(source, "max9296_declared_shared_peer_locked")
    ready_peer = function(source, "max9296_ready_shared_peer_locked")
    remove = function(source, "max9296_remove")
    restart_workers = function(source, "max9296_start_workers")
    stream = function(source, "max9296_s_stream")
    enable_worker = function(source, "max9296_enable")
    prepare_status = function(source, "sysfs_prepare_show")

    register_call = probe.index("v4l2_async_register_subdev_sensor_common")
    restartable_worker_call = probe.find("max9296_start_workers(sensor")
    first_worker = (
        restartable_worker_call
        if restartable_worker_call >= 0
        else probe.index("if (sensor->fsync_gpio")
    )
    health_attr = probe.index("device_create_file(&client->dev, &dev_attr_health_raw)")
    prepare_attr = probe.index("device_create_file(&client->dev, &dev_attr_prepare)")
    failures: list[str] = []
    if restartable_worker_call < 0:
        failures.append("probe does not use the restart-safe worker constructor")

    # A resolver that runs before its own probe commit must keep polling rather
    # than publish a half-probed devm pointer. Once both reciprocal probes are
    # committed, either resolver atomically installs both raw directions.
    early_a = PeerEndpoint("early-A", ready=False)
    early_b = PeerEndpoint("early-B")
    early_a.declared = early_b
    early_b.declared = early_a
    if publish_reciprocal(early_a) or early_a.published or early_b.published:
        failures.append("resolver published a peer before both probe commits")
    early_a.ready = True
    if not publish_reciprocal(early_a) or not (
        early_a.published is early_b and early_b.published is early_a
    ):
        failures.append("committed reciprocal probes were not linked atomically")

    failed = PeerEndpoint("failed", ready=False, clientdata_live=False, freed=True)
    survivor = PeerEndpoint("survivor")
    failed.declared = survivor
    survivor.declared = failed
    if publish_reciprocal(survivor) or survivor.published is not None:
        failures.append("peer probe failure left a published raw devm pointer")

    absent = PeerEndpoint("absent")
    absent.declared = None
    if publish_reciprocal(absent) or absent.published is not None:
        failures.append("absent declared peer must remain single-side")

    # The resolver can historically publish only B -> A. Removing A must find
    # B from A's declared phandle/client even when A's raw pointer is NULL,
    # stop every B worker that can retain A, and clear B before A is freed.
    a = PeerEndpoint("A", fsync_owner=True, fsync_worker_live=True)
    b = PeerEndpoint("B", fsync_owner=False)
    a.declared = b
    b.declared = a
    a.published = None
    b.published = a
    removal = SerializedRemoval()
    removal.remove(a)
    b.worker_step()
    if not b.workers_running or b.published is not None or b.stale_raw_uses:
        failures.append(
            "asymmetric B->A publication survives A clientdata/devm withdrawal"
        )
    removal.remove(b)
    if removal.history != ["enter:A", "exit:A", "enter:B", "exit:B"]:
        failures.append("concurrent peer removes are not serialized as whole callbacks")

    # Both unbind orders must leave every survivor-owned worker live. The GPIO
    # owner can operate alone; a non-owner without that owner fails STREAMON
    # closed until a reciprocal replacement is linked. Relink must not replace
    # or duplicate an already-live survivor worker.
    for first_name in ("A", "B"):
        owner = PeerEndpoint("A", fsync_owner=True, fsync_worker_live=True)
        nonowner = PeerEndpoint("B", fsync_owner=False)
        owner.declared = nonowner
        nonowner.declared = owner
        publish_reciprocal(owner)
        removed = owner if first_name == "A" else nonowner
        survivor = nonowner if first_name == "A" else owner
        SerializedRemoval().remove(removed)
        if not survivor.enable_worker_live:
            failures.append(f"{first_name}-first unbind left survivor enable worker dead")
        if survivor.fsync_owner and not survivor.fsync_worker_live:
            failures.append(f"{first_name}-first unbind left FSYNC owner worker dead")
        expected_immediate = 0 if survivor.fsync_owner else -19
        if survivor.stream_on() != expected_immediate:
            failures.append(
                f"{first_name}-first survivor did not enforce peer-absent FSYNC topology"
            )

        replacement = PeerEndpoint(first_name, fsync_owner=removed.fsync_owner)
        replacement.fsync_worker_live = replacement.fsync_owner
        replacement.declared = survivor
        survivor.declared = replacement
        before_workers = (
            survivor.fsync_worker_live,
            survivor.enable_worker_live,
        )
        if not publish_reciprocal(replacement):
            failures.append(f"{first_name}-first replacement did not relink")
        if survivor.stream_on() != 0:
            failures.append(f"{first_name}-first survivor did not recover after relink")
        if before_workers != (
            survivor.fsync_worker_live,
            survivor.enable_worker_live,
        ):
            failures.append("reciprocal relink duplicated/replaced live survivor workers")

    fail_a = PeerEndpoint("fail-A", fsync_owner=True, fsync_worker_live=True)
    fail_b = PeerEndpoint("fail-B", fsync_owner=False)
    fail_a.declared = fail_b
    fail_b.declared = fail_a
    publish_reciprocal(fail_a)
    SerializedRemoval().remove(fail_b, restart_fsync_succeeds=False)
    if fail_a.stream_on() >= 0 or fail_a.fsync_worker_live:
        failures.append("survivor FSYNC restart failure was not durable/fail-closed")

    preserved_a = PeerEndpoint(
        "preserved-A", fsync_owner=True, fsync_worker_live=False,
        worker_errno=-12,
    )
    preserved_b = PeerEndpoint("preserved-B", fsync_owner=False)
    preserved_a.declared = preserved_b
    preserved_b.declared = preserved_a
    preserved_a.published = preserved_b
    preserved_b.published = preserved_a
    SerializedRemoval().remove(preserved_b)
    if preserved_a.worker_errno != -12:
        failures.append("existing survivor worker errno was overwritten by detach gate")

    for required in (
        "lockdep_assert_held(&max9296_shared_lock)",
        "sensor->shared.np",
        "of_find_i2c_device_by_node(sensor->shared.np)",
        "i2c_get_clientdata(sensor->shared.client)",
        "to_max9296_dev(sd)",
    ):
        if required not in declared_peer:
            failures.append(f"declared peer lookup missing: {required}")

    for required in (
        "READ_ONCE(sensor->shared.probe_ready)",
        "READ_ONCE(sensor->dying)",
        "max9296_declared_shared_peer_locked(sensor)",
        "READ_ONCE(peer->shared.probe_ready)",
        "READ_ONCE(peer->dying)",
        "peer->shared.np != sensor->i2c_client->dev.of_node",
        "WRITE_ONCE(sensor->shared.sensor, peer)",
        "WRITE_ONCE(peer->shared.sensor, sensor)",
        "max9296_refresh_worker_status_locked(sensor)",
        "max9296_refresh_worker_status_locked(peer)",
    ):
        if required not in ready_peer:
            failures.append(f"atomic reciprocal peer publication missing: {required}")

    local_ready = ready_peer.find("READ_ONCE(sensor->shared.probe_ready)")
    declared_lookup = ready_peer.find("max9296_declared_shared_peer_locked(sensor)")
    peer_ready = ready_peer.find("READ_ONCE(peer->shared.probe_ready)")
    reciprocal = ready_peer.find(
        "peer->shared.np != sensor->i2c_client->dev.of_node"
    )
    local_publish = ready_peer.find("WRITE_ONCE(sensor->shared.sensor, peer)")
    reverse_publish = ready_peer.find("WRITE_ONCE(peer->shared.sensor, sensor)")
    if not (
        0 <= local_ready < declared_lookup < peer_ready < reciprocal
        < local_publish < reverse_publish
    ):
        failures.append(
            "peer publication must validate both committed reciprocal probes "
            "before atomically linking them"
        )

    remove_lock = remove.find("mutex_lock(&max9296_remove_lock)")
    remove_dying = remove.find("WRITE_ONCE(sensor->dying, true)", remove_lock)
    remove_shared_lock = remove.find(
        "mutex_lock(&max9296_shared_lock)", remove_dying
    )
    remove_declared_lookup = remove.find(
        "max9296_declared_shared_peer_locked(sensor)", remove_shared_lock
    )
    remove_ready_withdraw = remove.find(
        "WRITE_ONCE(sensor->shared.probe_ready, false)", remove_declared_lookup
    )
    remove_shared_unlock = remove.find(
        "mutex_unlock(&max9296_shared_lock)", remove_ready_withdraw
    )
    peer_fsync_stop = remove.find("kthread_stop(peer->thread_fsync)")
    peer_enable_live = "kthread_stop(peer->thread_en)" not in remove
    detach_power_lock = remove.find(
        "mutex_lock(&max9296_power_lock)", peer_fsync_stop
    )
    detach_shared_lock = remove.find(
        "mutex_lock(&max9296_shared_lock)", detach_power_lock
    )
    reverse_clear = remove.find("WRITE_ONCE(peer->shared.sensor, NULL)")
    detach_shared_unlock = remove.find(
        "mutex_unlock(&max9296_shared_lock)", reverse_clear
    )
    detach_power_unlock = remove.find(
        "mutex_unlock(&max9296_power_lock)", detach_shared_unlock
    )
    clientdata_withdraw = remove.find("i2c_set_clientdata(client, NULL)")
    remove_unlock = remove.rfind("mutex_unlock(&max9296_remove_lock)")
    if not (
        0 <= remove_lock < remove_dying < remove_shared_lock
        < remove_declared_lookup < remove_ready_withdraw < remove_shared_unlock
        < peer_fsync_stop < detach_power_lock
        < detach_shared_lock < reverse_clear < detach_shared_unlock
        < detach_power_unlock
        < clientdata_withdraw < remove_unlock
    ):
        failures.append(
            "remove does not detach an independently resolved reverse peer before free"
        )
    if "peer = READ_ONCE(sensor->shared.sensor)" in remove:
        failures.append("remove still relies only on its possibly-missing raw peer link")
    if not peer_enable_live:
        failures.append("peer enable worker is stopped despite retaining no raw peer")
    if "peer->" in remove[clientdata_withdraw:]:
        failures.append("raw peer access remains after clientdata withdrawal")

    for required in (
        "workers & MAX9296_WORKER_FSYNC",
        "!sensor->thread_fsync",
        "sensor->thread_fsync",
        "sensor->fsync_gpio",
        "kthread_run(max9296_fsync, sensor, \"max9296_fsync\")",
        "workers & MAX9296_WORKER_ENABLE",
        "!sensor->thread_en",
        "sensor->thread_en",
        "kthread_run(max9296_enable, sensor, name)",
        "struct task_struct *task",
        "IS_ERR(task)",
        "PTR_ERR(task)",
        "WRITE_ONCE(sensor->thread_fsync, task)",
        "WRITE_ONCE(sensor->thread_en, task)",
        "worker_errno",
    ):
        if required not in restart_workers:
            failures.append(f"restartable worker lifecycle missing: {required}")
    if "kthread_stop(" in restart_workers:
        failures.append("worker start helper must not join while creating workers")
    if "get_task_struct(" in restart_workers or "put_task_struct(" in restart_workers:
        failures.append("ordinary kthread_run workers acquired an extra task reference")
    if "sensor->thread_fsync = kthread_run(" in restart_workers:
        failures.append("FSYNC ERR_PTR is published before kthread_run validation")
    if "sensor->thread_en = kthread_run(" in restart_workers:
        failures.append("enable ERR_PTR is published before kthread_run validation")
    if "shared.sensor" in enable_worker:
        failures.append("live peer enable worker unexpectedly retains a raw peer pointer")

    for thread in ("thread_fsync", "thread_en"):
        publish = f"WRITE_ONCE(sensor->{thread}, task)"
        if source.count(publish) != 1:
            failures.append(f"{thread} has more than one live task publication path")
        own_stop = f"kthread_stop(sensor->{thread})"
        if source.count(own_stop) != 2:
            failures.append(
                f"{thread} is not stopped once in probe unwind and once in remove"
            )
    if source.count("kthread_stop(peer->thread_fsync)") != 1:
        failures.append("survivor FSYNC task does not have exactly one detach drain path")
    if "kthread_stop(peer->thread_en)" in source:
        failures.append("survivor enable task is stopped despite no raw-peer access")

    reverse_clear = remove.find("WRITE_ONCE(peer->shared.sensor, NULL)")
    detach_power_unlock = remove.find(
        "mutex_unlock(&max9296_power_lock)", reverse_clear
    )
    restart_call = remove.find(
        "max9296_start_workers(peer, peer_workers_stopped)", detach_power_unlock
    )
    clientdata_withdraw = remove.find("i2c_set_clientdata(client, NULL)")
    if not (0 <= reverse_clear < detach_power_unlock < restart_call < clientdata_withdraw):
        failures.append(
            "survivor workers must restart after safe detach and before removed devm free"
        )
    if "mutex_lock(&max9296_power_lock)" in remove[detach_power_unlock:restart_call]:
        failures.append("survivor worker restart occurs under max9296_power_lock")
    if "mutex_lock(&max9296_shared_lock)" in remove[detach_power_unlock:restart_call]:
        failures.append("survivor worker restart occurs under max9296_shared_lock")

    peer_gate = remove.find("WRITE_ONCE(peer->worker_errno, -EAGAIN)")
    preserve_prior_errno = remove.rfind(
        "if (!READ_ONCE(peer->worker_errno))", 0, peer_gate
    )
    peer_request_lock = remove.find("mutex_lock(&peer->prepare_request_lock)")
    stopped_mask = remove.find(
        "peer_workers_stopped |= MAX9296_WORKER_FSYNC", peer_request_lock
    )
    preserve_restart_errno = remove.find("if (!worker_errno)", restart_call)
    status_refresh = remove.find(
        "max9296_refresh_worker_status_locked(peer)", preserve_restart_errno
    )
    fail_close = remove.find("if (worker_errno)", status_refresh)
    peer_request_unlock = remove.find(
        "mutex_unlock(&peer->prepare_request_lock)", fail_close
    )
    if not (
        0 <= preserve_prior_errno < peer_gate < peer_request_lock
        < peer_fsync_stop < stopped_mask
        < reverse_clear < restart_call < preserve_restart_errno
        < status_refresh < fail_close
        < peer_request_unlock < clientdata_withdraw
    ):
        failures.append(
            "peer STREAMON admission, drain, detach, restart, and release are unordered"
        )

    fail_close_block = remove[fail_close:peer_request_unlock]
    for required in (
        "mutex_lock(&peer->lock)",
        "mutex_lock(&max9296_power_lock)",
        "peer->streaming = false",
        "peer->stream_on = 0",
        "peer->stream_commit_epoch = 0",
        "peer->state.fsync = MAX9296_STATE_IDLE",
        "max9296_disable_stream_mipi(peer)",
    ):
        if required not in fail_close_block:
            failures.append(f"survivor failure does not fail output closed: {required}")

    stream_worker_gate = stream.find("READ_ONCE(sensor->worker_errno)")
    stream_hardware_action = stream.find("max9296_normalize_fingerprint_locked")
    if not (
        0 <= stream_worker_gate < stream_hardware_action
        and "return worker_errno" in stream[:stream_hardware_action]
    ):
        failures.append("STREAMON does not fail closed on durable worker failure")
    if stream[:stream_hardware_action].count("READ_ONCE(sensor->worker_errno)") < 2:
        failures.append("STREAMON does not recheck worker failure after admission locking")

    probe_worker_status = probe.find("max9296_refresh_worker_status_locked(sensor)")
    probe_worker_start = probe.find("max9296_start_workers(sensor")
    ready_publish = probe.find("WRITE_ONCE(sensor->shared.probe_ready, true)")
    ready_link = probe.find("max9296_ready_shared_peer_locked(sensor)", ready_publish)
    if not (
        0 <= probe_worker_start < probe_worker_status < ready_publish < ready_link
    ):
        failures.append(
            "probe does not publish local worker health and synchronously recover relink"
        )

    for required in (
        "worker_errno = READ_ONCE(sensor->worker_errno)",
        "worker_errno=%d",
    ):
        if required not in prepare_status:
            failures.append(f"prepare diagnostics omit durable worker state: {required}")

    subdev_init = probe.index("v4l2_i2c_subdev_init(")
    lock_call = "mutex_lock(&max9296_shared_lock);"
    unlock_call = "mutex_unlock(&max9296_shared_lock);"
    subdev_init_lock = probe.rfind(lock_call, 0, subdev_init)
    subdev_init_unlock = probe.find(unlock_call, subdev_init)
    previous_unlock = probe.rfind(unlock_call, 0, subdev_init)
    if not (
        previous_unlock < subdev_init_lock < subdev_init < subdev_init_unlock
    ):
        failures.append("clientdata publication is not serialized with peer lookup")

    if shared_cleanup_start < 0:
        failures.append("probe has no shared peer/reference cleanup helper")
        shared_cleanup = ""
    else:
        shared_cleanup_end = source.index("\n}\n", shared_cleanup_start) + 3
        shared_cleanup = source[shared_cleanup_start:shared_cleanup_end]
        for required in (
            "kthread_stop(sensor->shared.thread_shared_init)",
            "mutex_lock(&max9296_shared_lock)",
            "WRITE_ONCE(sensor->shared.probe_ready, false)",
            "i2c_set_clientdata(sensor->i2c_client, NULL)",
            "mutex_unlock(&max9296_shared_lock)",
            "put_device(&sensor->shared.client->dev)",
            "of_node_put(sensor->shared.np)",
        ):
            if required not in shared_cleanup:
                failures.append(f"shared cleanup missing: {required}")

        for forbidden in (
            "peer->",
            "put_device(&peer",
            "WRITE_ONCE(peer",
        ):
            if forbidden in shared_cleanup:
                failures.append(
                    "failed probe must not mutate resources owned by its peer: "
                    f"{forbidden}"
                )

        cleanup_order = (
            shared_cleanup.find("kthread_stop(sensor->shared.thread_shared_init)"),
            shared_cleanup.find(lock_call),
            shared_cleanup.find("WRITE_ONCE(sensor->shared.probe_ready, false)"),
            shared_cleanup.find("i2c_set_clientdata(sensor->i2c_client, NULL)"),
            shared_cleanup.find(unlock_call),
            shared_cleanup.find("put_device(&sensor->shared.client->dev)"),
            shared_cleanup.find("of_node_put(sensor->shared.np)"),
        )
        if any(position < 0 for position in cleanup_order) or list(
            cleanup_order
        ) != sorted(cleanup_order):
            failures.append(
                "cleanup must join its resolver before withdrawing publication "
                "and releasing owned references"
            )

    if "bool probe_ready;" not in source:
        failures.append("shared peer candidates have no probe-ready lifetime gate")
    if "static DEFINE_MUTEX(max9296_shared_lock);" not in source:
        failures.append("shared resolver publication has no serialization lock")

    for required in (
        lock_call,
        "max9296_ready_shared_peer_locked(sensor)",
        unlock_call,
    ):
        if required not in shared_init:
            failures.append(f"shared resolver lifetime gate missing: {required}")

    resolver_order = (
        shared_init.find(lock_call),
        shared_init.find("max9296_ready_shared_peer_locked(sensor)"),
        shared_init.find(unlock_call),
    )
    if any(position < 0 for position in resolver_order) or list(
        resolver_order
    ) != sorted(resolver_order):
        failures.append(
            "resolver must lookup, validate, and publish the peer in one critical section"
        )

    if not (first_worker < health_attr < prepare_attr < register_call):
        failures.append("V4L2 registration must follow every fallible worker/sysfs step")

    registered_tail = probe[register_call:probe.index("return 0;", register_call)]
    if "device_create_file(" in registered_tail or "kthread_run(" in registered_tail:
        failures.append("fallible setup remains after V4L2 registration")
    if "goto remove_prepare_attr;" not in registered_tail:
        failures.append("registration failure does not unwind the completed sysfs setup")
    if "v4l2_async_unregister_subdev(&sensor->sd);" in probe:
        failures.append("probe error path unregisters a subdev that never registered")

    worker_failure_paths = probe[first_worker:health_attr]
    if worker_failure_paths.count("goto free_ctrls;") != 2:
        failures.append("worker helper and first sysfs failures must stop local workers")
    if "max9296_start_workers(sensor, MAX9296_WORKER_ALL);" not in worker_failure_paths:
        failures.append("probe does not use the restart-safe worker constructor")

    labels = (
        "remove_prepare_attr:",
        "remove_health_raw_attr:",
        "remove_link_status_attr:",
        "remove_enable_attr:",
        "remove_rotate_attr:",
        "free_ctrls:",
    )
    positions = [probe.find(label, register_call) for label in labels]
    if any(position < 0 for position in positions) or positions != sorted(positions):
        failures.append("registration/sysfs cleanup labels are not in reverse setup order")

    entity_cleanup = probe[probe.index("entity_cleanup:") :]
    if "max9296_release_probe_shared(sensor);" not in entity_cleanup:
        failures.append("common probe failure tail does not release shared ownership")

    resolver_create_start = probe.index(
        "sensor->shared.thread_shared_init =", probe.index("fsync,shared")
    )
    resolver_create_end = probe.index("v4l2_i2c_subdev_init(", resolver_create_start)
    resolver_create = probe[resolver_create_start:resolver_create_end]
    resolver_lifecycle_order = (
        resolver_create.find("kthread_create(max9296_shared_init"),
        resolver_create.find("get_task_struct(sensor->shared.thread_shared_init)"),
        resolver_create.find("wake_up_process(sensor->shared.thread_shared_init)"),
    )
    if any(position < 0 for position in resolver_lifecycle_order) or list(
        resolver_lifecycle_order
    ) != sorted(resolver_lifecycle_order):
        failures.append(
            "resolver task reference must be acquired after create and before wake"
        )
    if "kthread_run(max9296_shared_init" in probe:
        failures.append("resolver may exit before kthread_run caller acquires its task ref")

    success_return = probe.index("return 0;", register_call)
    success_tail = probe[register_call:success_return]
    ready_publish = "WRITE_ONCE(sensor->shared.probe_ready, true)"
    if source.count(ready_publish) != 1:
        failures.append("probe-ready commit must have exactly one writer")
    if ready_publish not in success_tail:
        failures.append("peer candidate becomes visible before all probe failures are past")
    elif not (
        success_tail.find(lock_call)
        < success_tail.find(ready_publish)
        < success_tail.find(unlock_call)
    ):
        failures.append("successful probe publication is not serialized with cleanup")

    committed_tail = success_tail[success_tail.find(ready_publish) :]
    for forbidden in ("goto ", "device_create_file(", "kthread_run("):
        if forbidden in committed_tail:
            failures.append(f"fallible operation remains after probe-ready commit: {forbidden}")

    if failures:
        for failure in failures:
            print(f"FAIL: {failure}", file=sys.stderr)
        return 1

    print("PASS: probe publication is atomic and V4L2 registration is final")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
