#!/bin/bash
set -eu

ROOT=$(cd "$(dirname "$0")/.." && pwd)
SCRIPT="$ROOT/tools/cam_fps_stack.sh"
FIXTURE_BIN="$ROOT/tests/fixtures/cam_fps_stack"
TMP_ROOT=$(mktemp -d)
trap 'rm -rf "$TMP_ROOT"' EXIT

fail() {
    echo "FAIL: $*" >&2
    exit 1
}

run_for_format() {
    env CAM_FPS_STACK_TEST_FMT="$1" PATH="$FIXTURE_BIN:$PATH" \
        bash "$SCRIPT" -c ch01 -d 0 -i 2
}

run_deep() {
    local name=$1
    local format=$2
    local label=$3
    local addr_count=$4
    local csi_fps=${5:-120}
    local isi_fps=${6:-120}
    local state="$TMP_ROOT/$name"
    mkdir -p "$state"
    printf '%s\n' \
        '10: 100 32e50000.csi' \
        '11: 50 32e02000.isi' >"$state/interrupts"

    env CAM_FPS_STACK_TEST_FMT="$format" \
        CAM_FPS_STACK_TEST_STATE="$state" \
        CAM_FPS_STACK_IRQ_FILE="$state/interrupts" \
        CAM_FPS_STACK_NCPU=1 \
        CAM_FPS_STACK_DMA_TOOL="$FIXTURE_BIN/dma-tool" \
        CAM_FPS_STACK_TEST_REQUESTED=120 \
        CAM_FPS_STACK_TEST_CSI_FPS="$csi_fps" \
        CAM_FPS_STACK_TEST_ISI_FPS="$isi_fps" \
        CAM_FPS_STACK_TEST_ADDR_COUNT="$addr_count" \
        PATH="$FIXTURE_BIN:$PATH" \
        bash "$SCRIPT" -c ch01 -d 1 -i 1 -D -L "$label" -R 120
}

echo "=== cam_fps_stack mode classification ==="

output=$(run_for_format '1280x360@1/120') ||
    fail "1280x360 classification command failed"
case "$output" in
    *'포맷 1280x360@1/120  모드 dual  AP1302 [0x11 0x12]'*) ;;
    *) fail "1280x360 dual stream was not mapped to both AP1302 addresses" ;;
esac

output=$(run_for_format '640x360@1/120') ||
    fail "640x360 classification command failed"
case "$output" in
    *'포맷 640x360@1/120  모드 single  AP1302 [0x3c]'*) ;;
    *) fail "640x360 single stream was not mapped to the global AP1302 address" ;;
esac

echo "=== deep readback and strict 120fps result ==="

output=$(run_deep single '640x360@1/120' SENSOR-640 1) ||
    fail "single 640x360 deep measurement failed"
case "$output" in
    *'AP_CONTEXT case=SENSOR-640 channel=ch0 addr=0x3c width=640 height=360 roi_x0=0 roi_y0=0 roi_x1=65535 roi_y1=65535 aspect=0x0090 sensor_mode=0x0005 line_time=0x1234 max_fps=120.000'*) ;;
    *) fail "single AP1302 context evidence is incomplete" ;;
esac
case "$output" in
    *'AR_TIMING case=SENSOR-640 channel=ch0 x_start=0x0000 y_start=0x0000 x_end=0x077f y_end=0x0437 frame_length=0x0450 line_length=0x0898 x_odd_inc=0x0003 y_odd_inc=0x0007 read_mode=0x0041 exposure=0x0100'*) ;;
    *) fail "single AR0234 timing evidence is incomplete" ;;
esac
case "$output" in
    *'FPS_RESULT case=SENSOR-640 requested=120 channel=ch0 sensor=120.0 isp=120.0 csi=120.0 isi=120.0 loss_pct=0.0 isi_trust=1 pass120=1'*) ;;
    *) fail "single wrap-around measurement did not strictly pass 120fps" ;;
esac

output=$(run_deep dual '1280x360@1/120' HD-ISP 2) ||
    fail "dual 1280x360 deep measurement failed"
for expected in \
    'AP_CONTEXT case=HD-ISP channel=ch0 addr=0x11 width=640 height=360' \
    'AP_CONTEXT case=HD-ISP channel=ch1 addr=0x12 width=640 height=360' \
    'AR_TIMING case=HD-ISP channel=ch0' \
    'AR_TIMING case=HD-ISP channel=ch1' \
    'FPS_RESULT case=HD-ISP requested=120 channel=ch0 sensor=120.0 isp=120.0 csi=120.0 isi=120.0 loss_pct=0.0 isi_trust=1 pass120=1' \
    'FPS_RESULT case=HD-ISP requested=120 channel=ch1 sensor=120.0 isp=120.0 csi=120.0 isi=120.0 loss_pct=0.0 isi_trust=1 pass120=1'
do
    case "$output" in
        *"$expected"*) ;;
        *) fail "dual evidence missing: $expected" ;;
    esac
done

output=$(run_deep untrusted '640x360@1/120' SENSOR-640 1 120 60) ||
    fail "untrusted ISI measurement command failed"
case "$output" in
    *'FPS_RESULT case=SENSOR-640 requested=120 channel=ch0 sensor=120.0 isp=120.0 csi=120.0 isi=60.0 loss_pct=0.0 isi_trust=0 pass120=0'*) ;;
    *) fail "untrusted ISI must make the strict 120fps result fail" ;;
esac

output=$(run_deep below-threshold '640x360@1/120' SENSOR-640 1 115 115) ||
    fail "below-threshold measurement command failed"
case "$output" in
    *'FPS_RESULT case=SENSOR-640 requested=120 channel=ch0 sensor=120.0 isp=120.0 csi=115.0 isi=115.0 loss_pct=4.2 isi_trust=1 pass120=0'*) ;;
    *) fail "115fps must not be rounded into a 120fps pass" ;;
esac

echo "PASS: cam_fps_stack mode/readback/120fps contracts"
