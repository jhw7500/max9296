#!/bin/bash
set -eu

ROOT=$(cd "$(dirname "$0")/.." && pwd)
SCRIPT="$ROOT/tools/cam_360p_resource.sh"
FIXTURE_BIN="$ROOT/tests/fixtures/cam_360p_resource"
TMP_ROOT=$(mktemp -d)
trap 'rm -rf "$TMP_ROOT"' EXIT

fail() {
    echo "FAIL: $*" >&2
    exit 1
}

prepare_tree() {
    local root=$1
    mkdir -p "$root/proc/4242" "$root/sys/class/thermal/thermal_zone0"
    printf '%s\n' 'VmRSS:       10000 kB' >"$root/proc/4242/status"
    printf '%s\n' '4242 (gstApp) S 1 1 1 0 0 0 0 0 0 0 100 20 0 0 0 0' >"$root/proc/4242/stat"
    printf '%s\n' 'cpu 100 0 50 850 0 0 0 0 0 0' >"$root/proc/stat"
    printf '%s\n' 45000 >"$root/sys/class/thermal/thermal_zone0/temp"
    printf '%s\n' cpu-thermal >"$root/sys/class/thermal/thermal_zone0/type"
}

run_resource() {
    local name=$1
    local supported=$2
    local root="$TMP_ROOT/$name"
    prepare_tree "$root"
    mkdir -p "$root/state"
    if [ "$supported" -eq 1 ]; then
        mkdir -p "$root/sys/bus/event_source/devices/imx8_ddr0/events"
        : >"$root/sys/bus/event_source/devices/imx8_ddr0/events/cycles"
        : >"$root/sys/bus/event_source/devices/imx8_ddr0/events/read-cycles"
        : >"$root/sys/bus/event_source/devices/imx8_ddr0/events/write-cycles"
    fi
    env CAM_360P_RESOURCE_PROC_ROOT="$root/proc" \
        CAM_360P_RESOURCE_SYS_ROOT="$root/sys" \
        CAM_360P_RESOURCE_TEST_STATE="$root/state" \
        PATH="$FIXTURE_BIN:$PATH" \
        bash "$SCRIPT" -d 1 -p 4242 -v /dev/video4
}

echo "=== resource collection without DDR PMU ==="
output=$(run_resource unsupported 0) || fail "unsupported DDR run failed"
for expected in \
    'V4L2_FORMAT node=/dev/video4 width=640 height=360 pixelformat=UYVY bytesperline=1280 sizeimage=460800' \
    'DDR_RESULT ddr_supported=0 cycles=na read_cycles=na write_cycles=na' \
    'RESOURCE_RESULT pid=4242 duration_ns=' \
    'vmrss_kb_before=10000 vmrss_kb_after=11000 process_ticks_delta=40 system_cpu_pct=30.0' \
    'DMESG_RESULT overflow=1 crc=1 ecc=1 lost_frame=1 timeout=1 green=1'
do
    case "$output" in *"$expected"*) ;; *) fail "missing: $expected" ;; esac
done

echo "=== resource collection with DDR PMU ==="
output=$(run_resource supported 1) || fail "supported DDR run failed"
case "$output" in
    *'DDR_RESULT ddr_supported=1 cycles=1000 read_cycles=2000 write_cycles=3000'*) ;;
    *) fail "supported DDR counters were not collected" ;;
esac

echo "PASS: cam_360p_resource capability and metric contracts"
