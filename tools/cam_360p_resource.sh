#!/usr/bin/env bash
# Collect one fixed-window gstApp/system/DDR/thermal/transport resource sample.

set -u

DURATION=20
GST_PID=
VIDEO_NODE=/dev/video4
OUTPUT_FILE=
PROC_ROOT=${CAM_360P_RESOURCE_PROC_ROOT:-/proc}
SYS_ROOT=${CAM_360P_RESOURCE_SYS_ROOT:-/sys}

usage() {
    cat <<'EOF'
Usage: cam_360p_resource.sh [options]
  -d, --duration SEC   fixed measurement duration (default: 20)
  -p, --pid PID        gstApp PID (default: first exact-name gstApp process)
  -v, --video NODE     V4L2 capture node (default: /dev/video4)
  -o, --output FILE    also save the complete report with tee
EOF
}

while [ $# -gt 0 ]; do
    case "$1" in
        -d|--duration) DURATION=$2; shift 2 ;;
        -p|--pid) GST_PID=$2; shift 2 ;;
        -v|--video) VIDEO_NODE=$2; shift 2 ;;
        -o|--output) OUTPUT_FILE=$2; shift 2 ;;
        -h|--help) usage; exit 0 ;;
        *) echo "Unknown option: $1" >&2; usage >&2; exit 2 ;;
    esac
done

case "$DURATION" in
    ''|*[!0-9]*) echo "duration must be a positive integer: $DURATION" >&2; exit 2 ;;
esac
[ "$DURATION" -gt 0 ] || {
    echo "duration must be greater than zero" >&2
    exit 2
}

if [ -z "$GST_PID" ]; then
    GST_PID=$(pgrep -x gstApp 2>/dev/null | head -1)
fi
case "$GST_PID" in
    ''|*[!0-9]*) echo "gstApp PID not found or invalid: ${GST_PID:-none}" >&2; exit 1 ;;
esac
[ -r "$PROC_ROOT/$GST_PID/status" ] || {
    echo "cannot read $PROC_ROOT/$GST_PID/status" >&2
    exit 1
}

if [ -n "$OUTPUT_FILE" ]; then
    mkdir -p "$(dirname "$OUTPUT_FILE")"
    exec > >(tee "$OUTPUT_FILE") 2>&1
fi

TMP_DIR=$(mktemp -d)
trap 'rm -rf "$TMP_DIR"' EXIT

read_vmrss_kb() {
    awk '$1 == "VmRSS:" {print $2; found=1; exit} END{if (!found) print "na"}' \
        "$PROC_ROOT/$GST_PID/status" 2>/dev/null
}

read_process_ticks() {
    awk '{print $14 + $15; found=1} END{if (!found) print "na"}' \
        "$PROC_ROOT/$GST_PID/stat" 2>/dev/null
}

read_cpu_totals() {
    awk '/^cpu / {
        busy=$2+$3+$4+$7+$8+$9
        total=busy+$5+$6
        print busy, total
        found=1
        exit
    } END{if (!found) print "na na"}' "$PROC_ROOT/stat" 2>/dev/null
}

emit_thermal() {
    local phase=$1
    local found=0
    local temp zone type
    for temp in "$SYS_ROOT"/class/thermal/thermal_zone*/temp; do
        [ -r "$temp" ] || continue
        found=1
        zone=$(basename "$(dirname "$temp")")
        type=unknown
        [ -r "$(dirname "$temp")/type" ] && type=$(cat "$(dirname "$temp")/type")
        printf 'THERMAL phase=%s zone=%s type=%s temp_millic=%s\n' \
            "$phase" "$zone" "$type" "$(cat "$temp")"
    done
    [ "$found" -eq 1 ] || printf 'THERMAL phase=%s supported=0\n' "$phase"
}

capture_dmesg() {
    dmesg --color=never 2>/dev/null || true
}

derive_dmesg_delta() {
    local before=$1
    local after=$2
    local delta=$3
    local marker marker_line

    if [ ! -s "$before" ]; then
        cp "$after" "$delta"
        return
    fi

    marker=$(tail -n 1 "$before")
    marker_line=
    if [ -n "$marker" ]; then
        marker_line=$(grep -nF -x -- "$marker" "$after" 2>/dev/null |
            head -n 1 | cut -d: -f1)
    fi

    if [ -n "$marker_line" ]; then
        tail -n +$((marker_line + 1)) "$after" >"$delta"
    else
        # The old tail was evicted (or the log was cleared). Treat the complete
        # current snapshot as new so qualification errors cannot be hidden.
        cp "$after" "$delta"
    fi
}

count_pattern() {
    local pattern=$1
    local file=$2
    grep -Eic "$pattern" "$file" 2>/dev/null || true
}

emit_media_and_format() {
    local format width height pixelformat bytesperline sizeimage
    echo "MEDIA_GRAPH_BEGIN"
    media-ctl -p 2>&1 || true
    echo "MEDIA_GRAPH_END"

    format=$(v4l2-ctl -d "$VIDEO_NODE" --get-fmt-video 2>&1 || true)
    printf '%s\n' "$format"
    width=$(printf '%s\n' "$format" | awk -F: '/Width\/Height/ {gsub(/[[:space:]]/, "", $2); split($2, a, "/"); print a[1]; exit}')
    height=$(printf '%s\n' "$format" | awk -F: '/Width\/Height/ {gsub(/[[:space:]]/, "", $2); split($2, a, "/"); print a[2]; exit}')
    pixelformat=$(printf '%s\n' "$format" | awk -F: '/Pixel Format/ {gsub(/[[:space:]'"'"']/, "", $2); print $2; exit}')
    bytesperline=$(printf '%s\n' "$format" | awk -F: '/Bytes per Line/ {gsub(/[[:space:]]/, "", $2); print $2; exit}')
    sizeimage=$(printf '%s\n' "$format" | awk -F: '/Size Image/ {gsub(/[[:space:]]/, "", $2); print $2; exit}')
    printf 'V4L2_FORMAT node=%s width=%s height=%s pixelformat=%s bytesperline=%s sizeimage=%s\n' \
        "$VIDEO_NODE" "${width:-unknown}" "${height:-unknown}" \
        "${pixelformat:-unknown}" "${bytesperline:-unknown}" \
        "${sizeimage:-unknown}"
}

extract_perf_value() {
    local event=$1
    local file=$2
    awk -F, -v wanted="imx8_ddr0/$event/" '
        $3 == wanted {
            gsub(/[[:space:]]/, "", $1)
            if ($1 ~ /^[0-9]+$/) print $1; else print "na"
            found=1
            exit
        }
        END{if (!found) print "na"}
    ' "$file" 2>/dev/null
}

VMRSS_BEFORE=$(read_vmrss_kb)
TICKS_BEFORE=$(read_process_ticks)
read -r CPU_BUSY_BEFORE CPU_TOTAL_BEFORE <<EOF
$(read_cpu_totals)
EOF
capture_dmesg >"$TMP_DIR/dmesg-before"
emit_thermal before
emit_media_and_format

START_NS=$(date +%s%N)
DDR_DIR="$SYS_ROOT/bus/event_source/devices/imx8_ddr0/events"
DDR_SUPPORTED=0
DDR_CYCLES=na
DDR_READ_CYCLES=na
DDR_WRITE_CYCLES=na
if [ -r "$DDR_DIR/cycles" ] && [ -r "$DDR_DIR/read-cycles" ] && \
   [ -r "$DDR_DIR/write-cycles" ] && command -v perf >/dev/null 2>&1; then
    DDR_SUPPORTED=1
    perf stat -x, -o "$TMP_DIR/perf.csv" -a \
        -e imx8_ddr0/cycles/,imx8_ddr0/read-cycles/,imx8_ddr0/write-cycles/ \
        -- sleep "$DURATION" >/dev/null 2>&1 || true
    if [ -r "$TMP_DIR/perf.csv" ]; then
        DDR_CYCLES=$(extract_perf_value cycles "$TMP_DIR/perf.csv")
        DDR_READ_CYCLES=$(extract_perf_value read-cycles "$TMP_DIR/perf.csv")
        DDR_WRITE_CYCLES=$(extract_perf_value write-cycles "$TMP_DIR/perf.csv")
    fi
else
    sleep "$DURATION"
fi
END_NS=$(date +%s%N)

VMRSS_AFTER=$(read_vmrss_kb)
TICKS_AFTER=$(read_process_ticks)
read -r CPU_BUSY_AFTER CPU_TOTAL_AFTER <<EOF
$(read_cpu_totals)
EOF
capture_dmesg >"$TMP_DIR/dmesg-after"
derive_dmesg_delta "$TMP_DIR/dmesg-before" "$TMP_DIR/dmesg-after" \
    "$TMP_DIR/dmesg-delta"

DURATION_NS=$((END_NS - START_NS))
PROCESS_TICKS_DELTA=na
case "$TICKS_BEFORE:$TICKS_AFTER" in
    *[!0-9:]*) ;;
    *) PROCESS_TICKS_DELTA=$((TICKS_AFTER - TICKS_BEFORE)) ;;
esac
SYSTEM_CPU_PCT=na
case "$CPU_BUSY_BEFORE:$CPU_TOTAL_BEFORE:$CPU_BUSY_AFTER:$CPU_TOTAL_AFTER" in
    *[!0-9:]*) ;;
    *)
        SYSTEM_CPU_PCT=$(awk -v b0="$CPU_BUSY_BEFORE" -v t0="$CPU_TOTAL_BEFORE" \
            -v b1="$CPU_BUSY_AFTER" -v t1="$CPU_TOTAL_AFTER" \
            'BEGIN{dt=t1-t0; if (dt>0) printf "%.1f", (b1-b0)*100/dt; else print "na"}')
        ;;
esac

emit_thermal after
printf 'DDR_RESULT ddr_supported=%d cycles=%s read_cycles=%s write_cycles=%s\n' \
    "$DDR_SUPPORTED" "$DDR_CYCLES" "$DDR_READ_CYCLES" "$DDR_WRITE_CYCLES"
printf 'RESOURCE_RESULT pid=%s duration_ns=%s vmrss_kb_before=%s vmrss_kb_after=%s process_ticks_delta=%s system_cpu_pct=%s\n' \
    "$GST_PID" "$DURATION_NS" "$VMRSS_BEFORE" "$VMRSS_AFTER" \
    "$PROCESS_TICKS_DELTA" "$SYSTEM_CPU_PCT"

OVERFLOW_COUNT=$(count_pattern 'overflow' "$TMP_DIR/dmesg-delta")
CRC_COUNT=$(count_pattern 'crc' "$TMP_DIR/dmesg-delta")
ECC_COUNT=$(count_pattern 'ecc' "$TMP_DIR/dmesg-delta")
LOST_COUNT=$(count_pattern 'lost[ _-]*frame' "$TMP_DIR/dmesg-delta")
TIMEOUT_COUNT=$(count_pattern 'timeout' "$TMP_DIR/dmesg-delta")
GREEN_COUNT=$(count_pattern 'green' "$TMP_DIR/dmesg-delta")
printf 'DMESG_RESULT overflow=%s crc=%s ecc=%s lost_frame=%s timeout=%s green=%s\n' \
    "$OVERFLOW_COUNT" "$CRC_COUNT" "$ECC_COUNT" "$LOST_COUNT" \
    "$TIMEOUT_COUNT" "$GREEN_COUNT"
echo "DMESG_DELTA_BEGIN"
cat "$TMP_DIR/dmesg-delta"
echo "DMESG_DELTA_END"
