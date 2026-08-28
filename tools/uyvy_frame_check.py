#!/usr/bin/env python3
"""Validate one raw UYVY frame and reject truncated/constant/green images."""

from __future__ import annotations

import argparse
import sys
from pathlib import Path


GREEN_RATIO_LIMIT = 0.80
LUMA_VARIANCE_MIN = 1.0
DEFAULT_MAX_SAMPLES = 65536


def clamp(value: int) -> int:
    return max(0, min(255, value))


def uyv_to_rgb(y: int, u: int, v: int) -> tuple[int, int, int]:
    c = max(0, y - 16)
    d = u - 128
    e = v - 128
    red = clamp((298 * c + 409 * e + 128) >> 8)
    green = clamp((298 * c - 100 * d - 208 * e + 128) >> 8)
    blue = clamp((298 * c + 516 * d + 128) >> 8)
    return red, green, blue


def emit_error(args: argparse.Namespace, actual: int, expected: int,
               reason: str) -> int:
    print(
        "UYVY_RESULT"
        f" path={args.raw} width={args.width} height={args.height}"
        f" bytesperline={args.bytesperline} expected_bytes={expected}"
        f" actual_bytes={actual} error={reason} pass=0"
    )
    return 2


def inspect(args: argparse.Namespace) -> int:
    raw_path = Path(args.raw)
    if args.width <= 0 or args.height <= 0 or args.width % 2:
        return emit_error(args, 0, 0, "invalid_geometry")

    minimum_stride = args.width * 2
    expected = args.bytesperline * args.height
    actual = raw_path.stat().st_size if raw_path.is_file() else 0
    if args.bytesperline < minimum_stride:
        return emit_error(args, actual, expected, "stride_too_short")
    if actual != expected:
        return emit_error(args, actual, expected, "size_mismatch")

    frame = raw_path.read_bytes()
    total_pixels = args.width * args.height
    sample_step = max(1, (total_pixels + args.max_samples - 1) //
                      args.max_samples)
    sampled = 0
    green_dominant = 0
    luma_sum = 0.0
    luma_square_sum = 0.0
    pixel_index = 0

    for row_index in range(args.height):
        row_start = row_index * args.bytesperline
        active = memoryview(frame)[row_start:row_start + minimum_stride]
        for pair_start in range(0, minimum_stride, 4):
            u = active[pair_start]
            y_values = (active[pair_start + 1], active[pair_start + 3])
            v = active[pair_start + 2]
            for y_value in y_values:
                if pixel_index % sample_step == 0:
                    red, green, blue = uyv_to_rgb(y_value, u, v)
                    sampled += 1
                    luma_sum += y_value
                    luma_square_sum += y_value * y_value
                    if (green >= 80 and green > red * 1.20 and
                            green > blue * 1.20):
                        green_dominant += 1
                pixel_index += 1

    luma_mean = luma_sum / sampled
    luma_variance = max(0.0, luma_square_sum / sampled - luma_mean * luma_mean)
    green_ratio = green_dominant / sampled
    constant = int(luma_variance < LUMA_VARIANCE_MIN)
    mostly_green = int(green_ratio > GREEN_RATIO_LIMIT)
    passed = int(not constant and not mostly_green)

    print(
        "UYVY_RESULT"
        f" path={raw_path} width={args.width} height={args.height}"
        f" bytesperline={args.bytesperline} expected_bytes={expected}"
        f" actual_bytes={actual} sampled_pixels={sampled}"
        f" luma_mean={luma_mean:.3f} luma_variance={luma_variance:.3f}"
        f" green_ratio={green_ratio:.6f} constant={constant}"
        f" mostly_green={mostly_green} pass={passed}"
    )
    return 0 if passed else 1


def parse_args() -> argparse.Namespace:
    parser = argparse.ArgumentParser(
        description="Validate one exact-size raw UYVY frame")
    parser.add_argument("raw", help="path to one raw UYVY frame")
    parser.add_argument("--width", type=int, required=True)
    parser.add_argument("--height", type=int, required=True)
    parser.add_argument("--bytesperline", type=int, required=True)
    parser.add_argument("--max-samples", type=int,
                        default=DEFAULT_MAX_SAMPLES)
    args = parser.parse_args()
    if args.max_samples <= 0:
        parser.error("--max-samples must be greater than zero")
    return args


if __name__ == "__main__":
    try:
        raise SystemExit(inspect(parse_args()))
    except OSError as error:
        print(f"UYVY_RESULT error=io_error detail={error} pass=0")
        raise SystemExit(2) from error
