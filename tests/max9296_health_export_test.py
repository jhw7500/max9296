#!/usr/bin/env python3
"""Offline tests for MAX9296 health_raw to camera-health-v1 export."""

from __future__ import annotations

import importlib.util
import json
import subprocess
import sys
import tempfile
from pathlib import Path
from typing import Any, Dict, List, Optional


ROOT = Path(__file__).resolve().parents[1]
MODULE_PATH = ROOT / "tools/max9296_health_export.py"

spec = importlib.util.spec_from_file_location("max9296_health_export", MODULE_PATH)
if spec is None or spec.loader is None:
    raise SystemExit("cannot load max9296_health_export.py")
exporter = importlib.util.module_from_spec(spec)
spec.loader.exec_module(exporter)


def raw_channel(
    channel: int,
    *,
    enabled: bool,
    link: str = "OK",
    serializer: str = "OK",
    isp: str = "OK",
    progress: str = "YES",
    phy: str = "A",
) -> Dict[str, Any]:
    blocked = link != "OK"
    return {
        "channel": channel,
        "enabled": enabled,
        "phy": phy if enabled else "NONE",
        "link": {"status": link if enabled else "N/A", "up": enabled and link == "OK"},
        "control_tunnel": (
            "N/A"
            if not enabled
            else "BLOCKED"
            if blocked
            else "AMBIGUOUS"
            if serializer == "UNKNOWN" and isp == "UNKNOWN"
            else "PARTIAL"
            if serializer == "FAIL" or isp == "FAIL"
            else "OK"
        ),
        "serializer": {
            "status": "N/A" if not enabled else "BLOCKED" if blocked else serializer,
            "errno": 0 if serializer == "OK" else -121,
            "device_id": 145 if serializer == "OK" else None,
        },
        "isp": {
            "status": "N/A" if not enabled else "BLOCKED" if blocked else isp,
            "errno": 0 if isp in {"OK", "STARTING"} else -121,
            "hinf_count": 10 if isp in {"OK", "STARTING"} else None,
            "hinf_progress": "NOT_EXPECTED"
            if not enabled
            else "NOT_AVAILABLE"
            if blocked
            else progress,
        },
        "sensor": {
            "status": "N/A"
            if not enabled
            else "BLOCKED"
            if blocked or isp == "FAIL"
            else "UNKNOWN",
            "probe": "DEEP_NOT_RUN",
        },
    }


def raw_device(
    adapter: int,
    base: int,
    channels: List[Dict[str, Any]],
    *,
    mode: str,
    streaming: bool = True,
    des_status: str = "OK",
) -> Dict[str, Any]:
    local_mask = sum(
        1 << (item["channel"] - base) for item in channels if item["enabled"]
    )
    global_mask = sum(1 << item["channel"] for item in channels if item["enabled"])
    return {
        "schema": 1,
        "adapter": adapter,
        "sequence": 7,
        "observed_monotonic_ms": 1000,
        "busy": False,
        "mode": mode,
        "streaming": streaming,
        "configured_local_mask": local_mask,
        "configured_global_mask": global_mask,
        "deserializer": {
            "status": des_status,
            "errno": 0 if des_status == "OK" else -121,
            "device_id": 150 if des_status == "OK" else None,
            "ctrl3_errno": 0 if des_status == "OK" else -6,
            "ctrl3": 250 if des_status == "OK" else None,
            "rx3_errno": 0 if des_status == "OK" else -6,
            "rx3": 102 if des_status == "OK" else None,
            "link_a_up": des_status == "OK",
            "link_b_up": des_status == "OK",
        },
        "channels": channels,
    }


def by_block(
    document: Dict[str, Any], block: str, channel: Optional[int] = None
) -> List[Dict[str, Any]]:
    result = [item for item in document["observations"] if item["block"] == block]
    if channel is not None:
        result = [item for item in result if item["scope"].get("channels") == [channel]]
    return result


class Tests:
    def __init__(self) -> None:
        self.passed = 0
        self.failed = 0

    def check(self, condition: bool, label: str) -> None:
        if condition:
            self.passed += 1
            print(f"  OK   {label}")
        else:
            self.failed += 1
            print(f"  FAIL {label}", file=sys.stderr)

    def run(self) -> int:
        print("=== max9296 health exporter ===")
        with tempfile.TemporaryDirectory(prefix="max9296-health-test.") as temporary:
            self.conversion_tests(Path(temporary))
            self.validation_tests(Path(temporary))
            self.cli_tests(Path(temporary))
        print()
        print(f"max9296 health exporter: {self.passed} passed / {self.failed} failed")
        return 1 if self.failed else 0

    def fixtures(self) -> List[Dict[str, Any]]:
        return [
            raw_device(
                2,
                0,
                [
                    raw_channel(0, enabled=True, phy="B"),
                    raw_channel(1, enabled=True, phy="A"),
                ],
                mode="dual-wide",
            ),
            raw_device(
                1,
                2,
                [
                    raw_channel(2, enabled=False),
                    raw_channel(3, enabled=False),
                ],
                mode="single",
                streaming=False,
            ),
        ]

    def conversion_tests(self, root: Path) -> None:
        result = exporter.convert(self.fixtures(), "boot-a", 1, 1100)
        self.check(
            result["channel_masks"]
            == {
                "configured_channel_mask": 3,
                "physical_present_mask": 3,
                "stream_domain_active_mask": 3,
            },
            "configured, physical, and active masks remain distinct fields",
        )
        self.check(
            result["stream_mode"] == "dual-wide",
            "configured dual pair exports dual-wide mode",
        )
        self.check(
            by_block(result, "sensor", 0)[0]["status"] == "UNKNOWN"
            and by_block(result, "sensor", 0)[0]["code"] == "PRODUCER_STALE",
            "shallow ABI does not fabricate AR0234 presence",
        )

        one_down = self.fixtures()
        one_down[0]["channels"][0] = raw_channel(0, enabled=True, link="DOWN", phy="B")
        result = exporter.convert(one_down, "boot-a", 2, 1200)
        link0 = by_block(result, "gmsl_link", 0)[0]
        self.check(
            result["channel_masks"]["physical_present_mask"] == 2
            and result["channel_masks"]["stream_domain_active_mask"] == 0,
            "one dual-wide link loss preserves peer physical identity but blocks shared activity",
        )
        self.check(
            link0["status"] == "FAIL"
            and link0["code"] == "GMSL_LINK_DOWN"
            and link0["action_scope"] == "camera-domain",
            "physical origin remains link-scoped while recovery scope is camera-domain",
        )

        ser_fail = self.fixtures()
        ser_fail[0]["channels"][0] = raw_channel(
            0, enabled=True, serializer="FAIL", isp="OK", phy="B"
        )
        result = exporter.convert(ser_fail, "boot-a", 3, 1300)
        ser0 = by_block(result, "serializer", 0)[0]
        self.check(
            ser0["status"] == "FAIL"
            and ser0["code"] == "SER_DEVICE_ID_FAIL"
            and "AP1302 ACK" in ser0["reason_detail"],
            "serializer FAIL requires independent AP1302 tunnel evidence",
        )
        self.check(
            by_block(result, "isp", 0)[0]["status"] == "OK",
            "SER management failure preserves ISP evidence",
        )

        ambiguous = self.fixtures()
        ambiguous[0]["channels"][0] = raw_channel(
            0,
            enabled=True,
            serializer="UNKNOWN",
            isp="UNKNOWN",
            progress="NOT_AVAILABLE",
            phy="B",
        )
        result = exporter.convert(ambiguous, "boot-a", 4, 1400)
        self.check(
            by_block(result, "serializer", 0)[0]["status"] == "UNKNOWN"
            and by_block(result, "isp", 0)[0]["status"] == "UNKNOWN"
            and not any(
                item["status"] == "FAIL"
                for item in by_block(result, "serializer", 0)
                + by_block(result, "isp", 0)
            ),
            "all-remote NAK stays ambiguous instead of blaming serializer",
        )

        contradictory = self.fixtures()
        contradictory[0]["channels"][0] = raw_channel(
            0,
            enabled=True,
            serializer="FAIL",
            isp="FAIL",
            progress="NOT_AVAILABLE",
            phy="B",
        )
        result = exporter.convert(contradictory, "boot-a", 4, 1450)
        self.check(
            by_block(result, "serializer", 0)[0]["status"] == "UNKNOWN"
            and by_block(result, "isp", 0)[0]["status"] == "UNKNOWN",
            "contradictory raw failures cannot bypass independent endpoint evidence",
        )

        isp_stall = self.fixtures()
        isp_stall[0]["channels"][0] = raw_channel(
            0, enabled=True, serializer="OK", isp="UNKNOWN", progress="NO", phy="B"
        )
        result = exporter.convert(isp_stall, "boot-a", 5, 1500)
        self.check(
            by_block(result, "isp", 0)[0]["code"] == "AMBIGUOUS_SENSOR_ISP_STALL",
            "stopped HINF remains ambiguous between sensor and ISP",
        )

        des_fail = self.fixtures()
        des_fail[0] = raw_device(
            2,
            0,
            [
                raw_channel(0, enabled=True, link="BLOCKED_BY_DES", phy="B"),
                raw_channel(1, enabled=True, link="BLOCKED_BY_DES", phy="A"),
            ],
            mode="dual-wide",
            des_status="FAIL",
        )
        result = exporter.convert(des_fail, "boot-a", 6, 1600)
        self.check(
            by_block(result, "deserializer")[1]["status"] == "FAIL"
            and by_block(result, "gmsl_link", 0)[0]["status"] == "BLOCKED",
            "DES control failure blocks remote probes without fabricating link failure",
        )

    def validation_tests(self, root: Path) -> None:
        path = root / "raw.json"
        document = self.fixtures()[0]
        path.write_text(json.dumps(document), encoding="utf-8")
        self.check(
            exporter.load_raw(path)["adapter"] == 2, "valid driver raw snapshot parses"
        )

        busy = {"schema": 1, "busy": True}
        path.write_text(json.dumps(busy), encoding="utf-8")
        try:
            exporter.load_raw(path)
        except exporter.SampleBusy:
            self.check(
                True, "busy driver sample is skipped, not published as hardware failure"
            )
        else:
            self.check(
                False,
                "busy driver sample is skipped, not published as hardware failure",
            )

        malformed = self.fixtures()[0]
        malformed["configured_global_mask"] = 1
        path.write_text(json.dumps(malformed), encoding="utf-8")
        try:
            exporter.load_raw(path)
        except exporter.ExportError:
            self.check(True, "inconsistent configured mask is rejected")
        else:
            self.check(False, "inconsistent configured mask is rejected")

        malformed = self.fixtures()[0]
        malformed["configured_local_mask"] = 1
        path.write_text(json.dumps(malformed), encoding="utf-8")
        try:
            exporter.load_raw(path)
        except exporter.ExportError:
            self.check(True, "inconsistent local configured mask is rejected")
        else:
            self.check(False, "inconsistent local configured mask is rejected")

    def cli_tests(self, root: Path) -> None:
        raw_paths = [root / "raw-0.json", root / "raw-1.json"]
        for path, document in zip(raw_paths, self.fixtures()):
            path.write_text(json.dumps(document), encoding="utf-8")
        boot_id = root / "boot_id"
        boot_id.write_text("boot-cli\n", encoding="utf-8")
        output = root / "max9296.json"
        command = [
            sys.executable,
            str(MODULE_PATH),
            "--once",
            "--boot-id-file",
            str(boot_id),
            "--output",
            str(output),
        ]
        for path in raw_paths:
            command.extend(("--input", str(path)))
        subprocess.run(command, check=True)
        result = json.loads(output.read_text(encoding="utf-8"))
        self.check(
            result["producer"] == "max9296" and result["boot_id"] == "boot-cli",
            "CLI publishes max9296 producer identity",
        )
        self.check(
            oct(output.stat().st_mode & 0o777) == "0o640", "CLI output mode is 0640"
        )
        self.check(
            not list(root.glob(".max9296.json.*")),
            "CLI atomic write leaves no temporary",
        )

        schema_path = (
            ROOT.parent / "pim-package-jhw/docs/camera-health/health-v1.schema.json"
        )
        registry_path = (
            ROOT.parent / "pim-package-jhw/docs/camera-health/error-codes-v1.json"
        )
        if schema_path.is_file() and registry_path.is_file():
            from jsonschema import Draft202012Validator

            schema = json.loads(schema_path.read_text(encoding="utf-8"))
            codes = {
                item["code"]
                for item in json.loads(registry_path.read_text(encoding="utf-8"))[
                    "codes"
                ]
            }
            self.check(
                not list(Draft202012Validator(schema).iter_errors(result)),
                "CLI output validates against sibling health-v1 schema",
            )
            self.check(
                all(item["code"] in codes for item in result["observations"]),
                "CLI uses only registered cross-repository codes",
            )

        sentinel = output.read_bytes()
        raw_paths[0].write_text(
            json.dumps({"schema": 1, "busy": True}), encoding="utf-8"
        )
        busy_run = subprocess.run(
            command, stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL
        )
        self.check(
            busy_run.returncode == 75, "CLI reports temporary busy with EX_TEMPFAIL"
        )
        self.check(
            output.read_bytes() == sentinel,
            "busy sampling preserves the previous atomic snapshot",
        )

        raw_paths[0].write_text("{not-json\n", encoding="utf-8")
        invalid_run = subprocess.run(
            command, stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL
        )
        self.check(
            invalid_run.returncode == 65, "CLI reports invalid raw input as data error"
        )
        self.check(
            output.read_bytes() == sentinel,
            "invalid sampling preserves the previous atomic snapshot",
        )


if __name__ == "__main__":
    raise SystemExit(Tests().run())
