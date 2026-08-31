#!/usr/bin/env python3
import subprocess
import tempfile
from pathlib import Path


ROOT = Path(__file__).resolve().parents[1]
SCRIPT = ROOT / "tools" / "rgb565_frame_check.py"


def rgb565(red, green, blue):
    word = ((red * 31 // 255) << 11) | ((green * 63 // 255) << 5) | (
        blue * 31 // 255)
    return bytes((word & 0xFF, word >> 8))


def frame(width, height, stride, colors):
    rows = bytearray()
    for row_index in range(height):
        row = bytearray()
        for column in range(width):
            row.extend(colors[(row_index * width + column) % len(colors)])
        row.extend(b"\xA5" * (stride - width * 2))
        rows.extend(row)
    return bytes(rows)


def run(path, width, height, stride):
    return subprocess.run(
        ["python3", str(SCRIPT), "--width", str(width), "--height",
         str(height), "--bytesperline", str(stride), str(path)],
        text=True, capture_output=True, check=False)


with tempfile.TemporaryDirectory() as directory:
    temp = Path(directory)
    width = 16
    height = 8
    stride = width * 2 + 8

    bars = [
        rgb565(255, 255, 255),
        rgb565(255, 0, 0),
        rgb565(0, 0, 255),
        rgb565(255, 255, 0),
        rgb565(0, 255, 255),
        rgb565(255, 0, 255),
        rgb565(64, 64, 64),
        rgb565(0, 0, 0),
    ]
    valid = temp / "bars.rgb565"
    valid.write_bytes(frame(width, height, stride, bars))
    result = run(valid, width, height, stride)
    assert result.returncode == 0, result.stdout + result.stderr
    assert "RGB565_RESULT" in result.stdout
    assert "pass=1" in result.stdout
    assert "mostly_green=0" in result.stdout

    green_colors = [
        rgb565(0, 96, 0),
        rgb565(0, 128, 0),
        rgb565(0, 160, 0),
        rgb565(0, 192, 0),
        rgb565(0, 224, 0),
    ]
    mostly_green = temp / "mostly-green.rgb565"
    mostly_green.write_bytes(frame(width, height, stride, green_colors))
    result = run(mostly_green, width, height, stride)
    assert result.returncode != 0, result.stdout
    assert "mostly_green=1" in result.stdout
    ratio = float(result.stdout.split("green_ratio=")[1].split()[0])
    assert ratio > 0.80, result.stdout

    result = run(valid, width, height, width * 2 - 2)
    assert result.returncode != 0
    assert "stride_too_short" in result.stdout

    truncated = temp / "truncated.rgb565"
    truncated.write_bytes(valid.read_bytes()[:-1])
    result = run(truncated, width, height, stride)
    assert result.returncode != 0
    assert "size_mismatch" in result.stdout

print("rgb565 frame check: valid/green/stride/truncation contracts PASSED")
