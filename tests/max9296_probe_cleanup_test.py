#!/usr/bin/env python3
"""Static regression checks for MAX9296 probe resource ownership."""

from __future__ import annotations

import sys
from pathlib import Path


ROOT = Path(__file__).resolve().parents[1]
SOURCE = ROOT / "max9296.c"


def main() -> int:
    source = SOURCE.read_text(encoding="utf-8")
    shared_init_start = source.index("static int max9296_shared_init(")
    shared_cleanup_start = source.find("static void max9296_release_probe_shared(")
    probe_start = source.index("static int max9296_probe(")
    probe_end = source.index("static int max9296_remove(", probe_start)
    probe = source[probe_start:probe_end]
    shared_init = source[shared_init_start:shared_cleanup_start]

    register_call = probe.index("v4l2_async_register_subdev_sensor_common")
    first_worker = probe.index("if (sensor->fsync_gpio")
    health_attr = probe.index("device_create_file(&client->dev, &dev_attr_health_raw)")
    failures: list[str] = []

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
        "i2c_get_clientdata(sensor->shared.client)",
        "READ_ONCE(peer->shared.probe_ready)",
        "WRITE_ONCE(sensor->shared.sensor, peer)",
        unlock_call,
    ):
        if required not in shared_init:
            failures.append(f"shared resolver lifetime gate missing: {required}")

    resolver_order = (
        shared_init.find(lock_call),
        shared_init.find("i2c_get_clientdata(sensor->shared.client)"),
        shared_init.find("READ_ONCE(peer->shared.probe_ready)"),
        shared_init.find("WRITE_ONCE(sensor->shared.sensor, peer)"),
        shared_init.find(unlock_call),
    )
    if any(position < 0 for position in resolver_order) or list(
        resolver_order
    ) != sorted(resolver_order):
        failures.append(
            "resolver must lookup, validate, and publish the peer in one critical section"
        )

    if not (first_worker < health_attr < register_call):
        failures.append("V4L2 registration must follow every fallible worker/sysfs step")

    registered_tail = probe[register_call:probe.index("return 0;", register_call)]
    if "device_create_file(" in registered_tail or "kthread_run(" in registered_tail:
        failures.append("fallible setup remains after V4L2 registration")
    if "goto remove_health_raw_attr;" not in registered_tail:
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
