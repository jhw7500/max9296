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

    def worker_step(self) -> None:
        if not self.workers_running or self.published is None:
            return
        if self.published.freed:
            self.stale_raw_uses += 1


@dataclass
class SerializedRemoval:
    active: bool = False
    history: list[str] = field(default_factory=list)

    def remove(self, endpoint: PeerEndpoint) -> None:
        """Model lookup by the declared client, not the local raw pointer."""
        assert not self.active
        self.active = True
        self.history.append(f"enter:{endpoint.name}")

        peer = endpoint.declared
        if peer is not None and (not peer.clientdata_live or not peer.ready):
            peer = None
        endpoint.ready = False

        if peer is not None and peer.published is endpoint:
            peer.workers_running = False
            peer.published = None

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
    return True


def function(source: str, name: str) -> str:
    signatures = (
        f"static int {name}(",
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

    register_call = probe.index("v4l2_async_register_subdev_sensor_common")
    first_worker = probe.index("if (sensor->fsync_gpio")
    health_attr = probe.index("device_create_file(&client->dev, &dev_attr_health_raw)")
    prepare_attr = probe.index("device_create_file(&client->dev, &dev_attr_prepare)")
    failures: list[str] = []

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
    a = PeerEndpoint("A")
    b = PeerEndpoint("B")
    a.declared = b
    b.declared = a
    a.published = None
    b.published = a
    removal = SerializedRemoval()
    removal.remove(a)
    b.worker_step()
    if b.workers_running or b.published is not None or b.stale_raw_uses:
        failures.append(
            "asymmetric B->A publication survives A clientdata/devm withdrawal"
        )
    removal.remove(b)
    if removal.history != ["enter:A", "exit:A", "enter:B", "exit:B"]:
        failures.append("concurrent peer removes are not serialized as whole callbacks")

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
    peer_enable_stop = remove.find("kthread_stop(peer->thread_en)")
    detach_power_lock = remove.find(
        "mutex_lock(&max9296_power_lock)", peer_enable_stop
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
        < peer_fsync_stop < peer_enable_stop < detach_power_lock
        < detach_shared_lock < reverse_clear < detach_shared_unlock
        < detach_power_unlock
        < clientdata_withdraw < remove_unlock
    ):
        failures.append(
            "remove does not detach an independently resolved reverse peer before free"
        )
    if "peer = READ_ONCE(sensor->shared.sensor)" in remove:
        failures.append("remove still relies only on its possibly-missing raw peer link")
    if "peer->" in remove[clientdata_withdraw:]:
        failures.append("raw peer access remains after clientdata withdrawal")

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
    if worker_failure_paths.count("goto free_ctrls;") != 3:
        failures.append("fsync, enable, and first sysfs failures must stop local workers")

    for thread in ("thread_fsync", "thread_en"):
        failure_start = worker_failure_paths.index(f"if (IS_ERR(sensor->{thread}))")
        failure_end = worker_failure_paths.index("}", failure_start)
        failure_block = worker_failure_paths[failure_start:failure_end]
        if f"ret = PTR_ERR(sensor->{thread});" not in failure_block:
            failures.append(f"{thread} failure does not propagate its errno")

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
