#!/usr/bin/env bash
set -euo pipefail

runner=tools/run_360p_readout_compare.sh
if [ ! -x "$runner" ]; then
  echo "FAIL: missing executable target runner: $runner" >&2
  exit 1
fi

tmp_dir=$(mktemp -d /tmp/max9296-readout-compare-test.XXXXXX)
trap 'rm -rf "$tmp_dir"' EXIT

make_fixture() {
  local root=$1
  mkdir -p "$root/modules" "$root/bin"
  printf 'production-module\n' >"$root/installed.ko"
  printf 'keep-module\n' >"$root/modules/max9296-keep.ko"
  printf 'mode1-module\n' >"$root/modules/max9296-sm01.ko"
  printf 'mode2-module\n' >"$root/modules/max9296-sm02.ko"
  printf '%s\n' '{"VHL_CAM":{"cam_width":640,"cam_height":360,"fps":30,"i2c2":{"crop_enable":false,"dz":100,"exp_time":2000,"ch0":{"ae_on":false,"dz_x":32768,"dz_y":32768},"ch1":{"ae_on":true,"dz_x":32768,"dz_y":32768}}}}' >"$root/edgeconf.json"
  printf 'active\n' >"$root/service.state"
  printf '0\n' >"$root/reset.count"

  cat >"$root/bin/systemctl" <<'EOF'
#!/usr/bin/env bash
set -eu
printf 'systemctl %s\n' "$*" >>"$TEST_EVENTS"
case "${1:-}" in
  is-active)
    state=$(cat "$TEST_SERVICE_STATE_FILE")
    printf '%s\n' "$state"
    [ "$state" = active ]
    ;;
  stop)
    printf 'inactive\n' >"$TEST_SERVICE_STATE_FILE"
    ;;
esac
EOF
  cat >"$root/bin/depmod" <<'EOF'
#!/usr/bin/env bash
set -eu
printf 'depmod %s\n' "$*" >>"$TEST_EVENTS"
EOF
  cat >"$root/bin/hard-reset" <<'EOF'
#!/usr/bin/env bash
set -eu
count=$(($(cat "$TEST_RESET_COUNT_FILE") + 1))
printf '%s\n' "$count" >"$TEST_RESET_COUNT_FILE"
start_service=0
for arg in "$@"; do
  [ "$arg" = -S ] && start_service=1
done
if [ "$start_service" -eq 1 ] && \
   [ "${TEST_SKIP_SERVICE_START_ON_RESET:-0}" != "$count" ]; then
  printf 'active\n' >"$TEST_SERVICE_STATE_FILE"
fi
printf 'reset module=%s fps=%s\n' \
  "$(sed -n '1p' "$MAX9296_COMPARE_MODULE_PATH")" \
  "$(jq -r '.VHL_CAM.fps' "$MAX9296_COMPARE_EDGECONF_PATH")" >>"$TEST_EVENTS"
EOF
  cat >"$root/bin/fps-tool" <<'EOF'
#!/usr/bin/env bash
set -eu
label=
while [ "$#" -gt 0 ]; do
  case "$1" in
    -L|--label) label=$2; shift 2 ;;
    *) shift ;;
  esac
done
printf 'fps label=%s module=%s fps=%s ae0=%s ae1=%s\n' \
  "$label" "$(sed -n '1p' "$MAX9296_COMPARE_MODULE_PATH")" \
  "$(jq -r '.VHL_CAM.fps' "$MAX9296_COMPARE_EDGECONF_PATH")" \
  "$(jq -r '.VHL_CAM.i2c2.ch0.ae_on' "$MAX9296_COMPARE_EDGECONF_PATH")" \
  "$(jq -r '.VHL_CAM.i2c2.ch1.ae_on' "$MAX9296_COMPARE_EDGECONF_PATH")" >>"$TEST_EVENTS"
if [ "${TEST_FAIL_CASE:-}" = "$label" ]; then
  echo "injected FPS failure: $label" >&2
  exit 42
fi
printf 'FPS_RESULT case=%s requested=120 pass120=1\n' "$label"
EOF
  cat >"$root/bin/resource-tool" <<'EOF'
#!/usr/bin/env bash
set -eu
printf 'resource case=%s\n' "${MAX9296_COMPARE_CASE:?}" >>"$TEST_EVENTS"
printf 'RESOURCE_RESULT case=%s\n' "$MAX9296_COMPARE_CASE"
EOF
cat >"$root/bin/dma-tool" <<'EOF'
#!/usr/bin/env bash
set -eu
printf 'dma case=%s\n' "${MAX9296_COMPARE_CASE:-unknown}" >>"$TEST_EVENTS"
if [ "${TEST_FAIL_DMA:-0}" = 1 ]; then
  echo "injected supplemental DMA failure" >&2
  exit 43
fi
printf 'DMA_RESULT channel=%s reg=%s module=%s\n' "$1" "$2" \
  "$(sed -n '1p' "$MAX9296_COMPARE_MODULE_PATH")"
EOF
  chmod +x "$root/bin/"*
  : >"$root/events.log"
}

run_fixture() {
  local root=$1
  local output=$2
  TEST_EVENTS="$root/events.log" \
  TEST_SERVICE_STATE_FILE="$root/service.state" \
  TEST_RESET_COUNT_FILE="$root/reset.count" \
  MAX9296_COMPARE_ALLOW_NON_ROOT=1 \
  MAX9296_COMPARE_MODULE_PATH="$root/installed.ko" \
  MAX9296_COMPARE_EDGECONF_PATH="$root/edgeconf.json" \
  MAX9296_COMPARE_HARD_RESET="$root/bin/hard-reset" \
  MAX9296_COMPARE_FPS_TOOL="$root/bin/fps-tool" \
  MAX9296_COMPARE_RESOURCE_TOOL="$root/bin/resource-tool" \
  MAX9296_COMPARE_DMA_TOOL="$root/bin/dma-tool" \
  MAX9296_COMPARE_SYSTEMCTL="$root/bin/systemctl" \
  MAX9296_COMPARE_DEPMOD="$root/bin/depmod" \
  "$runner" --module-dir "$root/modules" --output "$output" --duration 1
}

success_root="$tmp_dir/success"
make_fixture "$success_root"
run_fixture "$success_root" "$success_root/results"

test "$(sed -n '1p' "$success_root/installed.ko")" = production-module
test "$(jq -r '.VHL_CAM.fps' "$success_root/edgeconf.json")" = 30
test "$(grep -c '^reset ' "$success_root/events.log")" -eq 4
grep -q '^fps label=KEEP_120 module=keep-module fps=120 ae0=true ae1=true$' "$success_root/events.log"
grep -q '^fps label=SM01_120 module=mode1-module fps=120 ae0=true ae1=true$' "$success_root/events.log"
grep -q '^fps label=SM02_120 module=mode2-module fps=120 ae0=true ae1=true$' "$success_root/events.log"
keep_fps_line=$(grep -n '^fps label=KEEP_120 ' "$success_root/events.log" | cut -d: -f1)
keep_resource_line=$(grep -n '^resource case=keep$' "$success_root/events.log" | cut -d: -f1)
keep_dma_line=$(grep -n '^dma case=keep$' "$success_root/events.log" | head -1 | cut -d: -f1)
test "$keep_fps_line" -lt "$keep_resource_line"
test "$keep_resource_line" -lt "$keep_dma_line"
for label in keep sm01 sm02; do
  test -s "$success_root/results/$label-fps.txt"
  test -s "$success_root/results/$label-resource.txt"
  test -s "$success_root/results/$label-ar30b0.txt"
done
grep -q '^RESTORE_RESULT module=PASS edgeconf=PASS reset=PASS service=active$' \
  "$success_root/results/restore-status.txt"

inactive_root="$tmp_dir/inactive"
make_fixture "$inactive_root"
printf 'inactive\n' >"$inactive_root/service.state"
run_fixture "$inactive_root" "$inactive_root/results"
test "$(cat "$inactive_root/service.state")" = inactive
grep -q '^RESTORE_RESULT module=PASS edgeconf=PASS reset=PASS service=inactive$' \
  "$inactive_root/results/restore-status.txt"

service_mismatch_root="$tmp_dir/service-mismatch"
make_fixture "$service_mismatch_root"
set +e
TEST_SKIP_SERVICE_START_ON_RESET=4 \
  run_fixture "$service_mismatch_root" "$service_mismatch_root/results"
service_mismatch_rc=$?
set -e
test "$service_mismatch_rc" -eq 90
grep -q '^RESTORE_RESULT module=PASS edgeconf=PASS reset=PASS service=inactive$' \
  "$service_mismatch_root/results/restore-status.txt"

dma_failure_root="$tmp_dir/dma-failure"
make_fixture "$dma_failure_root"
TEST_FAIL_DMA=1 run_fixture "$dma_failure_root" "$dma_failure_root/results"
test "$(grep -c '^fps label=' "$dma_failure_root/events.log")" -eq 3
test "$(grep -c '^resource case=' "$dma_failure_root/events.log")" -eq 3
for label in keep sm01 sm02; do
  grep -q '^AR30B0_RESULT status=FAIL$' \
    "$dma_failure_root/results/$label-ar30b0.txt"
done
grep -q '^RESTORE_RESULT module=PASS edgeconf=PASS reset=PASS service=active$' \
  "$dma_failure_root/results/restore-status.txt"

dma_unavailable_root="$tmp_dir/dma-unavailable"
make_fixture "$dma_unavailable_root"
chmod -x "$dma_unavailable_root/bin/dma-tool"
run_fixture "$dma_unavailable_root" "$dma_unavailable_root/results"
test "$(grep -c '^fps label=' "$dma_unavailable_root/events.log")" -eq 3
test "$(grep -c '^resource case=' "$dma_unavailable_root/events.log")" -eq 3
for label in keep sm01 sm02; do
  grep -q '^AR30B0_RESULT status=SKIP reason=unavailable$' \
    "$dma_unavailable_root/results/$label-ar30b0.txt"
done

failure_root="$tmp_dir/failure"
make_fixture "$failure_root"
set +e
TEST_FAIL_CASE=SM01_120 run_fixture "$failure_root" "$failure_root/results"
failure_rc=$?
set -e
test "$failure_rc" -eq 42
test "$(sed -n '1p' "$failure_root/installed.ko")" = production-module
test "$(jq -r '.VHL_CAM.fps' "$failure_root/edgeconf.json")" = 30
test ! -e "$failure_root/results/sm02-fps.txt"
grep -q '^RESTORE_RESULT module=PASS edgeconf=PASS reset=PASS service=active$' \
  "$failure_root/results/restore-status.txt"

echo "PASS: 360p readout compare runner restores production state"
