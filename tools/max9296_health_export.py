#!/usr/bin/env python3
"""Export MAX9296 health_raw sysfs snapshots as camera health v1 JSON."""

from __future__ import annotations

import argparse
import glob
import json
import os
import signal
import sys
import tempfile
import time
from pathlib import Path
from typing import Any, Dict, List, Mapping, Optional, Sequence


VALID_RAW_STATUS = {"OK", "FAIL", "UNKNOWN", "STARTING", "BLOCKED", "N/A"}
VALID_LINK_STATUS = {"OK", "DOWN", "UNKNOWN", "BLOCKED_BY_DES", "N/A"}


class ExportError(ValueError):
    """A driver snapshot cannot be safely exported."""


class SampleBusy(ExportError):
    """The driver refused observation while its control lock was busy."""


def _is_int(value: object) -> bool:
    return isinstance(value, int) and not isinstance(value, bool)


def load_raw(path: Path) -> Dict[str, Any]:
    try:
        document = json.loads(path.read_text(encoding="utf-8"))
    except (OSError, json.JSONDecodeError) as exc:
        raise ExportError(f"health_raw unavailable: {path}: {exc}") from exc
    if not isinstance(document, dict) or document.get("schema") != 1:
        raise ExportError(f"health_raw schema mismatch: {path}")
    if document.get("busy") is True:
        raise SampleBusy(f"health_raw busy: {path}")
    if document.get("busy") is not False:
        raise ExportError(f"health_raw busy field invalid: {path}")
    required_ints = (
        "adapter",
        "sequence",
        "observed_monotonic_ms",
        "configured_local_mask",
        "configured_global_mask",
    )
    if any(not _is_int(document.get(key)) for key in required_ints):
        raise ExportError(f"health_raw integer field invalid: {path}")
    if (
        document["adapter"] < 0
        or document["sequence"] < 0
        or document["observed_monotonic_ms"] < 0
    ):
        raise ExportError(f"health_raw negative field: {path}")
    if document["configured_local_mask"] < 0 or document["configured_local_mask"] > 3:
        raise ExportError(f"health_raw local mask invalid: {path}")
    if (
        document["configured_global_mask"] < 0
        or document["configured_global_mask"] > 15
    ):
        raise ExportError(f"health_raw global mask invalid: {path}")
    if document.get("mode") not in {"single", "dual-wide"}:
        raise ExportError(f"health_raw mode invalid: {path}")
    if not isinstance(document.get("streaming"), bool):
        raise ExportError(f"health_raw streaming invalid: {path}")
    deserializer = document.get("deserializer")
    channels = document.get("channels")
    if not isinstance(deserializer, dict) or deserializer.get("status") not in {
        "OK",
        "FAIL",
    }:
        raise ExportError(f"health_raw deserializer invalid: {path}")
    if not isinstance(channels, list) or len(channels) != 2:
        raise ExportError(f"health_raw channels invalid: {path}")
    seen = set()
    derived_mask = 0
    for channel in channels:
        if not isinstance(channel, dict):
            raise ExportError(f"health_raw channel entry invalid: {path}")
        channel_id = channel.get("channel")
        if (
            not _is_int(channel_id)
            or channel_id < 0
            or channel_id > 3
            or channel_id in seen
        ):
            raise ExportError(f"health_raw channel id invalid: {path}")
        seen.add(channel_id)
        if not isinstance(channel.get("enabled"), bool):
            raise ExportError(f"health_raw channel enable invalid: {path}")
        link = channel.get("link")
        if not isinstance(link, dict) or link.get("status") not in VALID_LINK_STATUS:
            raise ExportError(f"health_raw link invalid for ch{channel_id}: {path}")
        for key in ("serializer", "isp", "sensor"):
            item = channel.get(key)
            if not isinstance(item, dict) or item.get("status") not in VALID_RAW_STATUS:
                raise ExportError(
                    f"health_raw {key} invalid for ch{channel_id}: {path}"
                )
        if channel["enabled"]:
            derived_mask |= 1 << channel_id
    channel_ids = sorted(seen)
    if channel_ids not in ([0, 1], [2, 3]):
        raise ExportError(f"health_raw channel pair invalid: {path}")
    channel_base = channel_ids[0]
    derived_local_mask = sum(
        1 << (channel["channel"] - channel_base)
        for channel in channels
        if channel["enabled"]
    )
    if derived_local_mask != document["configured_local_mask"]:
        raise ExportError(f"health_raw local mask inconsistent: {path}")
    if derived_mask != document["configured_global_mask"]:
        raise ExportError(f"health_raw configured mask inconsistent: {path}")
    if document["mode"] == "dual-wide" and document["configured_local_mask"] not in {
        0,
        3,
    }:
        raise ExportError(f"health_raw dual-wide mask invalid: {path}")
    if document["mode"] == "single" and document["configured_local_mask"] == 3:
        raise ExportError(f"health_raw single mask invalid: {path}")
    return document


def scope(kind: str, scope_id: str, channels: Sequence[int]) -> Dict[str, Any]:
    result: Dict[str, Any] = {"kind": kind, "id": scope_id}
    if channels:
        result["channels"] = list(channels)
    return result


def evidence(name: str, source: str, value: object) -> Dict[str, Any]:
    return {"name": name, "source": source, "value": value}


def observation(
    block: str,
    item_scope: Mapping[str, Any],
    status: str,
    code: str,
    items: Sequence[Mapping[str, Any]],
    *,
    root_cause: Optional[bool] = None,
    blocked_by: Optional[Sequence[str]] = None,
    action_scope: Optional[str] = None,
    reason_detail: Optional[str] = None,
) -> Dict[str, Any]:
    result: Dict[str, Any] = {
        "block": block,
        "scope": dict(item_scope),
        "status": status,
        "code": code,
        "count": 1 if status == "FAIL" else 0,
        "evidence": [dict(item) for item in items],
    }
    if root_cause is not None:
        result["root_cause"] = root_cause
    if blocked_by:
        result["blocked_by"] = list(blocked_by)
    if action_scope:
        result["action_scope"] = action_scope
    if reason_detail:
        result["reason_detail"] = reason_detail
    return result


def disabled_observation(block: str, channel: int) -> Dict[str, Any]:
    return observation(
        block,
        scope("channel", f"ch{channel}", [channel]),
        "N/A",
        "DISABLED",
        [evidence("configured", "max9296/health_raw", False)],
    )


def link_observation(channel: Mapping[str, Any]) -> Dict[str, Any]:
    channel_id = int(channel["channel"])
    link = channel["link"]
    status = link["status"]
    base = [
        evidence("physical_link_up", "max9296/RX3", bool(link.get("up"))),
        evidence("physical_phy", "max9296/RX3", str(channel.get("phy", "NONE"))),
    ]
    if status == "OK":
        return observation(
            "gmsl_link",
            scope("link", f"ch{channel_id}", [channel_id]),
            "OK",
            "NONE",
            base,
        )
    if status == "DOWN":
        return observation(
            "gmsl_link",
            scope("link", f"ch{channel_id}", [channel_id]),
            "FAIL",
            "GMSL_LINK_DOWN",
            base,
            root_cause=True,
            action_scope="camera-domain",
        )
    blocked = status == "BLOCKED_BY_DES"
    return observation(
        "gmsl_link",
        scope("link", f"ch{channel_id}", [channel_id]),
        "BLOCKED" if blocked else "UNKNOWN",
        "REMOTE_PATH_UNAVAILABLE",
        base,
        blocked_by=["deserializer"] if blocked else None,
        root_cause=False,
    )


def serializer_observation(channel: Mapping[str, Any]) -> Dict[str, Any]:
    channel_id = int(channel["channel"])
    serializer = channel["serializer"]
    status = serializer["status"]
    items = [
        evidence("control_tunnel", "max9296/remote-i2c", channel.get("control_tunnel")),
        evidence("device_id", "MAX9295/R0x000D", serializer.get("device_id")),
        evidence("i2c_errno", "MAX9295/R0x000D", serializer.get("errno")),
    ]
    if status == "OK":
        return observation(
            "serializer",
            scope("channel", f"ch{channel_id}", [channel_id]),
            "OK",
            "NONE",
            items,
        )
    if status == "FAIL" and channel["isp"].get("errno") == 0:
        return observation(
            "serializer",
            scope("channel", f"ch{channel_id}", [channel_id]),
            "FAIL",
            "SER_DEVICE_ID_FAIL",
            items,
            root_cause=True,
            action_scope="camera-domain",
            reason_detail="AP1302 ACK independently confirms the remote control tunnel",
        )
    if status == "BLOCKED":
        return observation(
            "serializer",
            scope("channel", f"ch{channel_id}", [channel_id]),
            "BLOCKED",
            "REMOTE_PATH_UNAVAILABLE",
            items,
            blocked_by=["gmsl_link"],
            root_cause=False,
        )
    return observation(
        "serializer",
        scope("channel", f"ch{channel_id}", [channel_id]),
        "UNKNOWN",
        "REMOTE_PATH_UNAVAILABLE",
        items,
        root_cause=False,
        reason_detail="MAX9295 and AP1302 both NAK; serializer attribution is unsafe",
    )


def isp_observation(channel: Mapping[str, Any]) -> Dict[str, Any]:
    channel_id = int(channel["channel"])
    isp = channel["isp"]
    status = isp["status"]
    progress = isp.get("hinf_progress")
    items = [
        evidence("control_tunnel", "max9296/remote-i2c", channel.get("control_tunnel")),
        evidence("hinf_frame_count", "AP1302/R0x0002[15:8]", isp.get("hinf_count")),
        evidence("hinf_progress", "AP1302/R0x0002[15:8]", progress),
        evidence("i2c_errno", "AP1302/R0x0002", isp.get("errno")),
    ]
    if status == "OK":
        return observation(
            "isp",
            scope("channel", f"ch{channel_id}", [channel_id]),
            "OK",
            "NONE",
            items,
        )
    if status == "STARTING":
        return observation(
            "isp",
            scope("channel", f"ch{channel_id}", [channel_id]),
            "STARTING",
            "NONE",
            items,
        )
    if status == "FAIL" and channel["serializer"].get("errno") == 0:
        return observation(
            "isp",
            scope("channel", f"ch{channel_id}", [channel_id]),
            "FAIL",
            "ISP_I2C_FAIL",
            items,
            root_cause=True,
            action_scope="camera-domain",
            reason_detail="MAX9295 ACK independently confirms the remote control tunnel",
        )
    if status == "BLOCKED":
        return observation(
            "isp",
            scope("channel", f"ch{channel_id}", [channel_id]),
            "BLOCKED",
            "REMOTE_PATH_UNAVAILABLE",
            items,
            blocked_by=["gmsl_link"],
            root_cause=False,
        )
    code = (
        "AMBIGUOUS_SENSOR_ISP_STALL" if progress == "NO" else "REMOTE_PATH_UNAVAILABLE"
    )
    return observation(
        "isp",
        scope("channel", f"ch{channel_id}", [channel_id]),
        "UNKNOWN",
        code,
        items,
        root_cause=False,
        reason_detail=(
            "AP1302 HINF stopped; shallow evidence cannot separate sensor and ISP"
            if progress == "NO"
            else "AP1302 endpoint is not independently reachable"
        ),
    )


def sensor_observation(channel: Mapping[str, Any]) -> Dict[str, Any]:
    channel_id = int(channel["channel"])
    status = channel["sensor"]["status"]
    items = [evidence("probe", "max9296/health_raw", channel["sensor"].get("probe"))]
    if status == "BLOCKED":
        return observation(
            "sensor",
            scope("channel", f"ch{channel_id}", [channel_id]),
            "BLOCKED",
            "REMOTE_PATH_UNAVAILABLE",
            items,
            blocked_by=["isp"],
            root_cause=False,
        )
    return observation(
        "sensor",
        scope("channel", f"ch{channel_id}", [channel_id]),
        "UNKNOWN",
        "PRODUCER_STALE",
        items,
        root_cause=False,
        reason_detail="AR0234 static DMA probe is not part of the shallow ABI",
    )


def convert(
    raw_documents: Sequence[Mapping[str, Any]], boot_id: str, sequence: int, now_ms: int
) -> Dict[str, Any]:
    configured_mask = 0
    physical_mask = 0
    active_mask = 0
    observations: List[Dict[str, Any]] = []
    source_sequences: Dict[str, int] = {}
    domain_modes: List[str] = []
    seen_channels = set()

    for raw in sorted(raw_documents, key=lambda item: int(item["adapter"])):
        adapter = int(raw["adapter"])
        configured = int(raw["configured_global_mask"])
        if configured_mask & configured:
            raise ExportError("health_raw inputs overlap configured channels")
        configured_mask |= configured
        source_sequences[f"i2c{adapter}"] = int(raw["sequence"])
        enabled_channels = [
            int(item["channel"]) for item in raw["channels"] if item["enabled"]
        ]
        if enabled_channels:
            domain_modes.append(str(raw["mode"]))
        pair_id = f"ch{min(int(item['channel']) for item in raw['channels'])}{max(int(item['channel']) for item in raw['channels'])}"
        deserializer = raw["deserializer"]
        des_items = [
            evidence("device_id", "MAX9296/R0x000D", deserializer.get("device_id")),
            evidence("i2c_errno", "MAX9296/R0x000D", deserializer.get("errno")),
            evidence("ctrl3", "MAX9296/R0x0013", deserializer.get("ctrl3")),
            evidence("rx3", "MAX9296/R0x002F", deserializer.get("rx3")),
        ]
        des_failed = deserializer["status"] == "FAIL"
        observations.append(
            observation(
                "deserializer",
                scope("pair", pair_id, enabled_channels),
                "FAIL" if des_failed else "OK",
                "DES_I2C_FAIL" if des_failed else "NONE",
                des_items,
                root_cause=True if des_failed else None,
                action_scope="camera-domain" if des_failed else None,
            )
        )

        domain_channels: List[Mapping[str, Any]] = []
        for channel in raw["channels"]:
            channel_id = int(channel["channel"])
            if channel_id in seen_channels:
                raise ExportError(
                    f"duplicate channel across health_raw inputs: ch{channel_id}"
                )
            seen_channels.add(channel_id)
            if not channel["enabled"]:
                observations.extend(
                    disabled_observation(block, channel_id)
                    for block in ("gmsl_link", "serializer", "isp", "sensor")
                )
                continue
            domain_channels.append(channel)
            if channel["link"]["status"] == "OK":
                physical_mask |= 1 << channel_id
            observations.extend(
                (
                    link_observation(channel),
                    serializer_observation(channel),
                    isp_observation(channel),
                    sensor_observation(channel),
                )
            )

        if raw["streaming"]:
            progressing = [
                channel
                for channel in domain_channels
                if channel["isp"].get("hinf_progress") == "YES"
            ]
            if raw["mode"] == "dual-wide":
                if domain_channels and len(progressing) == len(domain_channels):
                    for channel in domain_channels:
                        active_mask |= 1 << int(channel["channel"])
            else:
                for channel in progressing:
                    active_mask |= 1 << int(channel["channel"])

    if not domain_modes:
        stream_mode = "unknown"
    elif all(mode == "dual-wide" for mode in domain_modes):
        stream_mode = "dual-wide"
    elif all(mode == "single" for mode in domain_modes):
        stream_mode = "single"
    else:
        stream_mode = "independent"

    if any(item["status"] == "FAIL" for item in observations):
        status = "FAIL"
    elif any(item["status"] == "STARTING" for item in observations):
        status = "STARTING"
    elif any(item["status"] in {"UNKNOWN", "BLOCKED"} for item in observations):
        status = "UNKNOWN"
    else:
        status = "OK"

    return {
        "schema": 1,
        "producer": "max9296",
        "boot_id": boot_id,
        "pid": os.getpid(),
        "sequence": sequence,
        "observed_monotonic_ms": now_ms,
        "status": status,
        "stream_mode": stream_mode,
        "channel_masks": {
            "configured_channel_mask": configured_mask,
            "physical_present_mask": physical_mask,
            "stream_domain_active_mask": active_mask,
        },
        "observations": observations,
        "producer_data": {
            "abi": "max9296-health-raw-v1",
            "source_sequences": source_sequences,
            "sensor_probe": "shallow-only",
        },
    }


def atomic_write(path: Path, document: Mapping[str, Any]) -> None:
    path.parent.mkdir(parents=True, exist_ok=True)
    fd, temporary = tempfile.mkstemp(prefix=f".{path.name}.", dir=str(path.parent))
    try:
        with os.fdopen(fd, "w", encoding="utf-8") as stream:
            json.dump(document, stream, sort_keys=True, separators=(",", ":"))
            stream.write("\n")
            stream.flush()
            os.fsync(stream.fileno())
        os.chmod(temporary, 0o640)
        os.replace(temporary, path)
        directory_fd = os.open(path.parent, os.O_RDONLY | os.O_DIRECTORY)
        try:
            os.fsync(directory_fd)
        finally:
            os.close(directory_fd)
    except BaseException:
        try:
            os.unlink(temporary)
        except FileNotFoundError:
            pass
        raise


def discover_inputs(explicit: Sequence[Path]) -> List[Path]:
    if explicit:
        return list(explicit)
    return [
        Path(path)
        for path in sorted(glob.glob("/sys/bus/i2c/devices/*-0048/health_raw"))
    ]


def parse_args(argv: Optional[Sequence[str]] = None) -> argparse.Namespace:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--input", action="append", type=Path, default=[])
    parser.add_argument(
        "--output", type=Path, default=Path("/run/pim-camera/max9296.json")
    )
    parser.add_argument(
        "--boot-id-file", type=Path, default=Path("/proc/sys/kernel/random/boot_id")
    )
    parser.add_argument("--interval-ms", type=int, default=1000)
    parser.add_argument("--once", action="store_true")
    return parser.parse_args(argv)


def main(argv: Optional[Sequence[str]] = None) -> int:
    args = parse_args(argv)
    if args.interval_ms < 1:
        raise SystemExit("interval must be positive")
    boot_id = args.boot_id_file.read_text(encoding="utf-8").strip()
    if not boot_id:
        raise SystemExit("boot ID is empty")
    inputs = discover_inputs(args.input)
    if not inputs:
        raise SystemExit("no max9296 health_raw inputs found")
    stopped = False
    sequence = 0

    def stop(_signum: int, _frame: object) -> None:
        nonlocal stopped
        stopped = True

    signal.signal(signal.SIGTERM, stop)
    signal.signal(signal.SIGINT, stop)
    while not stopped:
        try:
            raw = [load_raw(path) for path in inputs]
        except SampleBusy:
            if args.once:
                return 75
            time.sleep(args.interval_ms / 1000.0)
            continue
        except ExportError as exc:
            if args.once:
                print(str(exc), file=sys.stderr)
                return 65
            time.sleep(args.interval_ms / 1000.0)
            continue
        sequence += 1
        now_ms = time.monotonic_ns() // 1_000_000
        atomic_write(args.output, convert(raw, boot_id, sequence, now_ms))
        if args.once:
            break
        time.sleep(args.interval_ms / 1000.0)
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
