#!/usr/bin/env python3
import subprocess
import tempfile
from pathlib import Path


ROOT = Path(__file__).resolve().parents[1]
SCRIPT = ROOT / "tools" / "uyvy_frame_check.py"


def uyvy_row(width, pairs):
    row = bytearray()
    for index in range(width // 2):
        y0, u, y1, v = pairs[index % len(pairs)]
        row.extend((u, y0, v, y1))
    return bytes(row)


def run(path, width, height, stride):
    return subprocess.run(
        ["python3", str(SCRIPT), "--width", str(width), "--height",
         str(height), "--bytesperline", str(stride), str(path)],
        text=True, capture_output=True, check=False)


with tempfile.TemporaryDirectory() as directory:
    temp = Path(directory)
    width = 16
    height = 8
    stride = width * 2

    bars = [
        (235, 128, 235, 128),
        (210, 16, 210, 146),
        (170, 166, 170, 16),
        (145, 54, 145, 34),
        (106, 202, 106, 222),
        (81, 90, 81, 240),
        (41, 240, 41, 110),
        (16, 128, 16, 128),
    ]
    valid = temp / "bars.uyvy"
    valid.write_bytes(uyvy_row(width, bars) * height)
    result = run(valid, width, height, stride)
    assert result.returncode == 0, result.stdout + result.stderr
    assert "pass=1" in result.stdout
    assert "mostly_green=0" in result.stdout

    green_pairs = [
        (120, 54, 140, 34),
        (150, 54, 170, 34),
        (100, 54, 180, 34),
        (130, 54, 200, 34),
        (81, 90, 81, 240),
    ]
    mostly_green = temp / "mostly-green.uyvy"
    mostly_green.write_bytes(uyvy_row(width, green_pairs) * height)
    result = run(mostly_green, width, height, stride)
    assert result.returncode != 0, result.stdout
    assert "mostly_green=1" in result.stdout
    ratio = float(result.stdout.split("green_ratio=")[1].split()[0])
    assert ratio > 0.80, result.stdout

    result = run(valid, width, height, stride - 2)
    assert result.returncode != 0
    assert "stride_too_short" in result.stdout

    truncated = temp / "truncated.uyvy"
    truncated.write_bytes(valid.read_bytes()[:-1])
    result = run(truncated, width, height, stride)
    assert result.returncode != 0
    assert "size_mismatch" in result.stdout

print("uyvy frame check: valid/green/stride/truncation contracts PASSED")
