#!/usr/bin/env python3
"""Contract tests for 360p modes, exposure fencing, and AP1302 zoom controls."""

from __future__ import annotations

import re
import sys
from pathlib import Path


ROOT = Path(__file__).resolve().parents[1]
SOURCE = ROOT / "max9296.c"


def function(source: str, name: str) -> str:
    """Return one C function body using brace matching."""
    match = re.search(rf"\b{name}\s*\([^;]*?\)\s*\{{", source, re.S)
    if not match:
        return ""

    start = match.start()
    brace = source.find("{", match.start())
    depth = 0
    for index in range(brace, len(source)):
        if source[index] == "{":
            depth += 1
        elif source[index] == "}":
            depth -= 1
            if depth == 0:
                return source[start : index + 1]
    return ""


def dz_percent_to_fixed8(percent: int) -> int:
    if not 100 <= percent <= 300:
        raise ValueError("digital zoom percent")
    return (percent * 0x100 + 50) // 100


def center_u16_to_fixed8(position: int) -> int:
    if not 0 <= position <= 0xFFFF:
        raise ValueError("normalized center")
    return (position * 0x100 + 0x7FFF) // 0xFFFF


def exposure_allowed(fps: int, safe_max_fps: int) -> bool:
    return 1 <= fps <= safe_max_fps


def exposure_restore_addresses(dual: bool) -> tuple[int, ...]:
    return (0x11, 0x12) if dual else (0x3C,)


def zoom_restore_slots(dual: bool, enable: int) -> tuple[str, ...]:
    if dual:
        return ("ch0", "ch1")
    return ("ch1",) if enable == 2 else ("ch0",)


def control_write_target(programmed_dual: bool, topology_pending: bool) -> str:
    if topology_pending:
        return "cache-only"
    return "per-channel" if programmed_dual else "global"


def main() -> int:
    source = SOURCE.read_text(encoding="utf-8")
    failures: list[str] = []

    # Public-control conversion contracts. The center ABI stays 0..65535,
    # while AP1302 DZ_CENTER_X/Y use normalized s7.8 (0x0000..0x0100).
    expected_conversions = {
        "dz 100%": (dz_percent_to_fixed8(100), 0x0100),
        "dz 125%": (dz_percent_to_fixed8(125), 0x0140),
        "dz 300%": (dz_percent_to_fixed8(300), 0x0300),
        "center start": (center_u16_to_fixed8(0), 0x0000),
        "center midpoint": (center_u16_to_fixed8(0x8000), 0x0080),
        "center end": (center_u16_to_fixed8(0xFFFF), 0x0100),
    }
    for label, (actual, expected) in expected_conversions.items():
        if actual != expected:
            failures.append(f"{label} conversion: got 0x{actual:x}, expected 0x{expected:x}")

    if not exposure_allowed(30, 30):
        failures.append("30fps must remain inside the conservative exposure-write policy")
    if exposure_allowed(31, 30):
        failures.append("31fps must be fenced by a 30fps exposure-write policy")

    if exposure_restore_addresses(True) != (0x11, 0x12):
        failures.append("dual cached exposure must restore through channel addresses")
    if zoom_restore_slots(False, 2) != ("ch1",):
        failures.append("single-right zoom must restore the active ch1 cache")
    if control_write_target(False, True) != "cache-only":
        failures.append("pending topology changes must not receive live control I2C")

    # 640x360 is the single-camera tuple; 1280x360 is two 640-wide channels.
    for token in (
        "MAX9296_MODE_1280x360",
        "MAX9296_MODE_640x360",
        "1280,\n        360",
        "640,\n        360",
    ):
        if token not in source:
            failures.append(f"missing 360p mode contract: {token!r}")

    parse_prepare = function(source, "max9296_parse_prepare_command")
    if "command->width == 1280 && command->height == 360" not in parse_prepare:
        failures.append("prepare ABI does not accept the 1280x360 dual tuple")
    if "command->width == 640 && command->height == 360" not in parse_prepare:
        failures.append("prepare ABI does not accept the 640x360 single tuple")

    # max_fps remains the ordinary negotiation limit. Exposure safety is a
    # separate per-mode policy and every direct 0x500c write passes its gate.
    mode_struct = source[source.find("struct max9296_mode_info") :]
    mode_struct = mode_struct[: mode_struct.find("};") + 2]
    if "u32 max_fps;" not in mode_struct:
        failures.append("mode info lost ordinary max_fps")
    if "u32 exposure_safe_max_fps;" not in mode_struct:
        failures.append("mode info lacks separate exposure_safe_max_fps")
    if "#define MAX9296_EXPOSURE_SAFE_MAX_FPS 30" not in source:
        failures.append("conservative 30fps exposure safety policy is missing")
    if '#include "max9296_360p_policy.h"' not in source:
        failures.append("driver does not consume the tested 360p policy")
    if "#define MAX9296_360P_MAX_FPS 120" not in source:
        failures.append("360p ordinary 120fps limit is missing")
    if source.count("MAX9296_360P_MAX_FPS,") < 3:
        failures.append("single, dual, and right-hand 360p modes do not share 120fps")
    if "#define MAX9296_DEFAULT_MAX_FPS 30" not in source:
        failures.append("HD/FHD ordinary max_fps policy is missing")
    if source.count("MAX9296_DEFAULT_MAX_FPS,") < 7:
        failures.append("one or more HD/FHD mode tables still advertise over 30fps")

    enum_interval = function(source, "max9296_enum_frame_interval")
    if "max9296_find_mode(sensor, fie->width, fie->height, false)" not in enum_interval:
        failures.append("frame-interval enumeration does not resolve the requested mode")
    if "fie->index >= mode->max_fps" not in enum_interval:
        failures.append("frame-interval enumeration still uses a global FPS limit")

    set_interval = function(source, "max9296_s_frame_interval")
    mode_limit = set_interval.find("fps > sensor->current_mode->max_fps")
    fsync_write = set_interval.find("max9296_update_shared_fsync_locked")
    if mode_limit < 0 or fsync_write < 0 or mode_limit > fsync_write:
        failures.append("s_frame_interval does not reject mode-invalid FPS before FSYNC")

    set_format_decl = source.find("static int max9296_set_fmt(")
    set_format = (
        function(source[set_format_decl:], "max9296_set_fmt")
        if set_format_decl >= 0
        else ""
    )
    if "READ_ONCE(sensor->fps) > new_mode->max_fps" not in set_format:
        failures.append("S_FMT can select a mode below the current FPS")

    normalize = function(source, "max9296_normalize_fingerprint_locked")
    if "fps > mode->max_fps" not in normalize:
        failures.append("hardware fingerprints accept a mode-invalid current FPS")

    preview_registers = (
        "#define AP1302_REG_ATOMIC 0x1184",
        "#define AP1302_REG_PREVIEW_WIDTH 0x2000",
        "#define AP1302_REG_PREVIEW_HEIGHT 0x2002",
        "#define AP1302_REG_PREVIEW_ROI_X0 0x2004",
        "#define AP1302_REG_PREVIEW_ROI_Y0 0x2006",
        "#define AP1302_REG_PREVIEW_ROI_X1 0x2008",
        "#define AP1302_REG_PREVIEW_ROI_Y1 0x200a",
        "#define AP1302_REG_PREVIEW_ASPECT 0x200c",
        "#define AP1302_REG_PREVIEW_SENSOR_MODE 0x2014",
        "#define AP1302_REG_PREVIEW_LINE_TIME 0x201c",
        "#define AP1302_REG_PREVIEW_MAX_FPS 0x2020",
        "#define AP1302_REG_TRIGGER_MAX_MISMATCH 0x6112",
    )
    for register in preview_registers:
        if register not in source:
            failures.append(f"missing AP1302 preview register contract: {register}")

    context_writer = function(source, "max9296_program_preview_context_channel")
    if not context_writer:
        failures.append("per-AP1302 preview context writer is missing")
    else:
        ordered_tokens = (
            "AP1302_ATOMIC_BEGIN",
            "AP1302_REG_PREVIEW_WIDTH",
            "AP1302_REG_PREVIEW_HEIGHT",
            "AP1302_REG_PREVIEW_ROI_X0",
            "AP1302_REG_PREVIEW_ROI_Y0",
            "AP1302_REG_PREVIEW_ROI_X1",
            "AP1302_REG_PREVIEW_ROI_Y1",
            "AP1302_REG_PREVIEW_ASPECT",
            "AP1302_REG_PREVIEW_SENSOR_MODE",
            "AP1302_REG_PREVIEW_MAX_FPS",
            "AP1302_REG_TRIGGER_MAX_MISMATCH",
            "AP1302_ATOMIC_FINISH",
        )
        positions = [context_writer.find(token) for token in ordered_tokens]
        if any(position < 0 for position in positions) or positions != sorted(positions):
            failures.append("preview context is not programmed in atomic policy order")
        if not re.search(
            r"max9296_preview_sensor_mode\(current_value,\s*2U,\s*sensor_mode\)",
            context_writer,
        ):
            failures.append("PREVIEW_SENSOR_MODE does not preserve unowned bits")
        if "sensor_mode != MAX9296_360P_SENSOR_MODE_KEEP" not in context_writer:
            failures.append("KEEP artifact can still write PREVIEW_SENSOR_MODE")
        if re.search(
            r"maxim_ops_i2c_write\s*\([^;]*AP1302_REG_PREVIEW_LINE_TIME",
            context_writer,
            re.S,
        ):
            failures.append("qualification build must leave PREVIEW_LINE_TIME automatic")

    exposure_write = function(source, "max9296_write_exposure")
    if not exposure_write:
        failures.append("central max9296_write_exposure gate is missing")
    else:
        gate = exposure_write.find("exposure_safe_max_fps")
        write = exposure_write.find("maxim_ops_i2c_write")
        if gate < 0 or write < 0 or gate > write:
            failures.append("exposure FPS policy is not checked before I2C")
        for field in ("channel", "mode", "fps", "exposure", "safe_max_fps"):
            if field not in exposure_write:
                failures.append(f"exposure rejection log lacks {field}")

    direct_exposure_writes = re.findall(
        r"maxim_ops_i2c_write\s*\([^;]*?AP1302_REG_EXP_TIME", source, re.S
    )
    if len(direct_exposure_writes) != 1:
        failures.append(
            "all AP1302 0x500c writes must funnel through one guarded helper "
            f"(found {len(direct_exposure_writes)})"
        )

    apply_channel_controls = function(source, "max9296_apply_channel_controls")
    if "max9296_write_exposure(sensor, i2c_addr" not in apply_channel_controls:
        failures.append("cached dual exposure restore does not use its channel address")

    # Bench-verified AP1302 register meanings. 0x1012 is only the immediate
    # transition selector; 0x1014 is optical zoom and must not be an X/Y target.
    register_contracts = (
        "#define AP1302_REG_DZ_TGT_FCT 0x1010",
        "#define AP1302_REG_DZ_STEP_FCT 0x1012",
        "#define AP1302_REG_DZ_CENTER_X 0x118c",
        "#define AP1302_REG_DZ_CENTER_Y 0x118e",
        "#define AP1302_DZ_STEP_IMMEDIATE 0x8000",
        "#define MAX9296_DZ_DEFAULT 100",
        "#define MAX9296_DZ_CENTER_DEFAULT 0x8000",
    )
    for contract in register_contracts:
        if contract not in source:
            failures.append(f"missing zoom register contract: {contract}")

    for cid in (
        "V4L2_CID_DZ",
        "V4L2_CID_DZ_X",
        "V4L2_CID_DZ_Y",
        "V4L2_CID_DZ_X_CH0",
        "V4L2_CID_DZ_X_CH1",
        "V4L2_CID_DZ_Y_CH0",
        "V4L2_CID_DZ_Y_CH1",
    ):
        if f"#define {cid} " not in source:
            failures.append(f"missing V4L2 zoom control: {cid}")

    # A dual MAX9296 stream concatenates both AP1302 outputs and therefore
    # requires one common zoom factor. Per-channel factor controls would allow
    # different sensor readout heights and corrupt the combined frame.
    for unsafe_cid in ("V4L2_CID_DZ_CH0", "V4L2_CID_DZ_CH1"):
        if f"#define {unsafe_cid} " in source:
            failures.append(f"unsafe per-channel zoom factor remains: {unsafe_cid}")

    channel_cache = source[source.find("struct max9296_channel_ctrl") :]
    channel_cache = channel_cache[: channel_cache.find("};") + 2]
    for field in ("int dz_x;", "int dz_y;"):
        if field not in channel_cache:
            failures.append(f"zoom state is not restart-persistent: {field}")
    if "int dz;" in channel_cache:
        failures.append("zoom factor must be common, not cached per channel")

    shared_cache = source[source.find("struct max9296_ctrl_cache") :]
    shared_cache = shared_cache[: shared_cache.find("};") + 2]
    if "int dz;" not in shared_cache:
        failures.append("common zoom factor is not restart-persistent")

    post_firmware = function(source, "max9296_post_firmware_program_locked")
    if "max9296_apply_cached_zoom" not in post_firmware:
        failures.append("firmware reload does not restore cached zoom before stream commit")

    stream_on = function(source, "max9296_s_stream")
    zoom_replay = stream_on.find("ret = max9296_apply_cached_zoom(sensor)")
    stream_commit = stream_on.find("max9296_stream_commit_locked(sensor)")
    if zoom_replay < 0 or stream_commit < 0 or zoom_replay > stream_commit:
        failures.append(
            "controls cached after prepare are not replayed before output commit"
        )
    elif "if (ret)" not in stream_on[zoom_replay:stream_commit] or (
        "max9296_drop_fsync_contract_locked(sensor)"
        not in stream_on[zoom_replay:stream_commit]
    ):
        failures.append("pre-output zoom failure does not abort STREAMON cleanly")

    enable_decl = source.find("static int max9296_enable(void *data)")
    enable_worker = (
        function(source[enable_decl:], "max9296_enable") if enable_decl >= 0 else ""
    )
    latest_zoom = enable_worker.find("zoom_ret = max9296_apply_cached_zoom(sensor)")
    output_enable = enable_worker.find("max9296_enable_output_locked(sensor)")
    if latest_zoom < 0 or output_enable < 0 or latest_zoom > output_enable:
        failures.append(
            "enable worker does not replay the latest zoom before CSI output"
        )
    elif "if (zoom_ret)" not in enable_worker[latest_zoom:output_enable] or (
        "else if" not in enable_worker[latest_zoom:output_enable]
    ):
        failures.append("enable worker can consume output request after zoom failure")

    apply_zoom = function(source, "max9296_apply_cached_zoom")
    if "sensor->enable == 0x02" not in apply_zoom:
        failures.append("single-right firmware reload does not select the active ch1 zoom cache")
    if "max9296_hw_is_dual(sensor)" not in apply_zoom:
        failures.append("zoom restore does not use the programmed hardware topology")
    if apply_zoom.count("sensor->ctrl_cache.dz") < 3:
        failures.append("single and both dual AP1302 writes do not share one cached zoom factor")

    write_zoom = function(source, "max9296_write_zoom_channel")
    if "u32 dz_percent" not in write_zoom:
        failures.append("zoom writer does not receive the common factor explicitly")
    if "max9296_dz_percent_to_fixed8(dz_percent)" not in write_zoom:
        failures.append("zoom writer still derives the factor from per-channel state")

    set_ctrl = function(source, "max9296_s_ctrl")
    cache_pos = set_ctrl.find("max9296_cache_ctrl(sensor, ctrl)")
    pending_pos = set_ctrl.find("sensor->pending_mode_change")
    if cache_pos < 0 or pending_pos < cache_pos:
        failures.append("pending topology controls are not cached before live-I2C suppression")
    if "sensor->pending_fmt_change" not in set_ctrl or "return 0;" not in set_ctrl[pending_pos:]:
        failures.append("pending format/topology changes do not suppress live control I2C")
    if "max9296_hw_is_dual(sensor)" not in set_ctrl:
        failures.append("live control addressing does not use programmed hardware topology")

    stream_commit = function(source, "max9296_stream_commit_locked")
    for flag in ("pending_mode_change", "pending_fmt_change"):
        clear = f"sensor->{flag} = false;"
        if clear not in stream_commit:
            failures.append(
                f"legacy S_FMT -> STREAMON does not clear {flag} before live controls"
            )

    if re.search(r"0x510a", source, re.I):
        failures.append("unsafe AP1302 0x510A manual-WB register was introduced")

    if failures:
        for failure in failures:
            print(f"FAIL: {failure}", file=sys.stderr)
        return 1

    print("PASS: 360p modes, exposure fence, and common zoom-factor contracts")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
