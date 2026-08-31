#!/usr/bin/env bash
set -euo pipefail

usage() {
  cat <<'EOF'
Usage: run_360p_readout_compare.sh --module-dir DIR --output DIR [--duration SEC]

Target-only qualification runner for 640x360@120 sensor readout comparison.
It tests KEEP, AP1302 sensor mode 1, and sensor mode 2, then restores the
installed MAX9296 module and edgeconf even when a case fails.
EOF
}

die() {
  echo "ERROR: $*" >&2
  exit 2
}

module_dir=
output_dir=
duration=20

while [ "$#" -gt 0 ]; do
  case "$1" in
    --module-dir)
      [ "$#" -ge 2 ] || die "--module-dir requires a directory"
      module_dir=$2
      shift 2
      ;;
    --output)
      [ "$#" -ge 2 ] || die "--output requires a directory"
      output_dir=$2
      shift 2
      ;;
    --duration)
      [ "$#" -ge 2 ] || die "--duration requires seconds"
      duration=$2
      shift 2
      ;;
    -h|--help)
      usage
      exit 0
      ;;
    *) die "unknown option: $1" ;;
  esac
done

[ -n "$module_dir" ] || die "--module-dir is required"
[ -n "$output_dir" ] || die "--output is required"
case "$duration" in
  ''|*[!0-9]*) die "duration must be a positive integer: $duration" ;;
esac
[ "$duration" -gt 0 ] || die "duration must be greater than zero"
[ "$output_dir" != / ] || die "output must not be the filesystem root"
[ ! -e "$output_dir" ] || die "output already exists: $output_dir"

if [ "$(id -u)" -ne 0 ] && [ "${MAX9296_COMPARE_ALLOW_NON_ROOT:-0}" != 1 ]; then
  die "this target runner must execute as root"
fi

module_path=${MAX9296_COMPARE_MODULE_PATH:-}
if [ -z "$module_path" ]; then
  module_path=$(modinfo -n max9296) || die "cannot resolve installed max9296 module"
fi
edgeconf_path=${MAX9296_COMPARE_EDGECONF_PATH:-/root/shared_v/edgeconf_pim.json}
hard_reset=${MAX9296_COMPARE_HARD_RESET:-/root/camtest/cam_hard_reset.sh}
fps_tool=${MAX9296_COMPARE_FPS_TOOL:-/root/camtest/max9296-360p-run-20260828T120000Z/cam_fps_stack.sh}
resource_tool=${MAX9296_COMPARE_RESOURCE_TOOL:-/root/camtest/max9296-360p-run-20260828T120000Z/cam_360p_resource.sh}
dma_tool=${MAX9296_COMPARE_DMA_TOOL:-/opt/pim/bin/cam_ap1302_dma_verify.sh}
systemctl_bin=${MAX9296_COMPARE_SYSTEMCTL:-systemctl}
depmod_bin=${MAX9296_COMPARE_DEPMOD:-depmod}

for candidate in max9296-keep.ko max9296-sm01.ko max9296-sm02.ko; do
  [ -f "$module_dir/$candidate" ] || die "missing candidate: $module_dir/$candidate"
done
[ -f "$module_path" ] || die "installed module not found: $module_path"
[ -f "$edgeconf_path" ] || die "edgeconf not found: $edgeconf_path"
for tool in "$hard_reset" "$fps_tool" "$resource_tool"; do
  [ -x "$tool" ] || die "required executable not found: $tool"
done
dma_available=0
[ -x "$dma_tool" ] && dma_available=1
command -v "$systemctl_bin" >/dev/null 2>&1 || die "systemctl command not found: $systemctl_bin"
command -v "$depmod_bin" >/dev/null 2>&1 || die "depmod command not found: $depmod_bin"
command -v jq >/dev/null 2>&1 || die "jq is required"

jq -e '
  (.VHL_CAM | type == "object") and
  (.VHL_CAM.i2c2 | type == "object") and
  (.VHL_CAM.i2c2.ch0 | type == "object") and
  (.VHL_CAM.i2c2.ch1 | type == "object")
' "$edgeconf_path" >/dev/null || die "edgeconf camera structure is invalid"

mkdir -p "$(dirname "$output_dir")"
mkdir "$output_dir"
backup_dir="$output_dir/backup"
mkdir "$backup_dir"

module_mode=$(stat -c '%a' "$module_path")
edgeconf_mode=$(stat -c '%a' "$edgeconf_path")
original_service=$($systemctl_bin is-active cam-operate.service 2>/dev/null || true)
case "$original_service" in
  active|inactive) ;;
  *) die "cam-operate service state must be active or inactive: ${original_service:-unknown}" ;;
esac
cp -a "$module_path" "$backup_dir/max9296.ko"
cp -a "$edgeconf_path" "$backup_dir/edgeconf_pim.json"
sha256sum "$backup_dir/max9296.ko" "$backup_dir/edgeconf_pim.json" \
  >"$backup_dir/sha256.txt"
if [ -f "$module_dir/manifest.tsv" ]; then
  cp -a "$module_dir/manifest.tsv" "$output_dir/candidate-manifest.tsv"
fi

backup_ready=1
restore_state() {
  local original_rc=$?
  local restore_failed=0
  local reset_state=FAIL
  local service_state=unknown
  local expected_module expected_edgeconf actual_module actual_edgeconf

  trap - EXIT HUP INT TERM
  set +e
  if [ "${backup_ready:-0}" -eq 1 ]; then
    "$systemctl_bin" stop cam-operate.service >/dev/null 2>&1 || restore_failed=1
    install -m "$module_mode" "$backup_dir/max9296.ko" "$module_path" || restore_failed=1
    install -m "$edgeconf_mode" "$backup_dir/edgeconf_pim.json" "$edgeconf_path" || restore_failed=1
    "$depmod_bin" -a >>"$output_dir/restore-reset.txt" 2>&1 || restore_failed=1
    if [ "$original_service" = active ]; then
      "$hard_reset" -s -S >>"$output_dir/restore-reset.txt" 2>&1 && reset_state=PASS || restore_failed=1
    else
      "$hard_reset" -s >>"$output_dir/restore-reset.txt" 2>&1 && reset_state=PASS || restore_failed=1
    fi

    expected_module=$(sha256sum "$backup_dir/max9296.ko" | awk '{print $1}')
    expected_edgeconf=$(sha256sum "$backup_dir/edgeconf_pim.json" | awk '{print $1}')
    actual_module=$(sha256sum "$module_path" 2>/dev/null | awk '{print $1}')
    actual_edgeconf=$(sha256sum "$edgeconf_path" 2>/dev/null | awk '{print $1}')
    service_state=$($systemctl_bin is-active cam-operate.service 2>/dev/null || true)

    module_state=FAIL
    edgeconf_state=FAIL
    [ "$actual_module" = "$expected_module" ] && module_state=PASS || restore_failed=1
    [ "$actual_edgeconf" = "$expected_edgeconf" ] && edgeconf_state=PASS || restore_failed=1
    [ "$service_state" = "$original_service" ] || restore_failed=1
    printf 'RESTORE_RESULT module=%s edgeconf=%s reset=%s service=%s\n' \
      "$module_state" "$edgeconf_state" "$reset_state" "${service_state:-inactive}" \
      >"$output_dir/restore-status.txt"
  fi
  set -e

  if [ "$restore_failed" -ne 0 ]; then
    echo "ERROR: production restore failed; inspect $output_dir/restore-reset.txt" >&2
    exit 90
  fi
  exit "$original_rc"
}
trap restore_state EXIT
trap 'exit 130' HUP INT
trap 'exit 143' TERM

jq '
  .VHL_CAM.cam_width = 640 |
  .VHL_CAM.cam_height = 360 |
  .VHL_CAM.fps = 120 |
  .VHL_CAM.i2c2.crop_enable = false |
  .VHL_CAM.i2c2.dz = 100 |
  .VHL_CAM.i2c2.ch0.ae_on = true |
  .VHL_CAM.i2c2.ch1.ae_on = true |
  .VHL_CAM.i2c2.ch0.dz_x = 32768 |
  .VHL_CAM.i2c2.ch0.dz_y = 32768 |
  .VHL_CAM.i2c2.ch1.dz_x = 32768 |
  .VHL_CAM.i2c2.ch1.dz_y = 32768
' "$backup_dir/edgeconf_pim.json" >"$output_dir/edgeconf-640x360-120.json"
jq -e '
  .VHL_CAM.cam_width == 640 and
  .VHL_CAM.cam_height == 360 and
  .VHL_CAM.fps == 120 and
  .VHL_CAM.i2c2.crop_enable == false and
  .VHL_CAM.i2c2.dz == 100 and
  .VHL_CAM.i2c2.ch0.ae_on == true and
  .VHL_CAM.i2c2.ch1.ae_on == true
' "$output_dir/edgeconf-640x360-120.json" >/dev/null

run_case() {
  local slug=$1
  local label=$2
  local candidate=$3
  local candidate_path="$module_dir/$candidate"

  "$systemctl_bin" stop cam-operate.service
  install -m "$edgeconf_mode" "$output_dir/edgeconf-640x360-120.json" "$edgeconf_path"
  install -m "$module_mode" "$candidate_path" "$module_path"
  "$depmod_bin" -a

  {
    printf 'CASE=%s\n' "$label"
    sha256sum "$candidate_path" "$module_path" "$edgeconf_path"
  } >"$output_dir/$slug-state.txt"

  "$hard_reset" -s -S >"$output_dir/$slug-reset.txt" 2>&1
  [ "$($systemctl_bin is-active cam-operate.service 2>/dev/null || true)" = active ] || {
    echo "ERROR: cam-operate is not active after $label reset" >&2
    return 1
  }

  "$fps_tool" -c ch01 -D -d "$duration" -i 1 -L "$label" -R 120 \
    >"$output_dir/$slug-fps.txt" 2>&1
  MAX9296_COMPARE_CASE="$slug" "$resource_tool" -d "$duration" -v /dev/video4 \
    >"$output_dir/$slug-resource.txt" 2>&1

  if [ "$dma_available" -ne 1 ]; then
    printf 'AR30B0_RESULT status=SKIP reason=unavailable\n' \
      >"$output_dir/$slug-ar30b0.txt"
  else
    dma_state=PASS
    {
      if ! MAX9296_COMPARE_CASE="$slug" "$dma_tool" 0 0x30b0; then
        dma_state=FAIL
      fi
      if ! MAX9296_COMPARE_CASE="$slug" "$dma_tool" 1 0x30b0; then
        dma_state=FAIL
      fi
      printf 'AR30B0_RESULT status=%s\n' "$dma_state"
    } >"$output_dir/$slug-ar30b0.txt" 2>&1
  fi
}

run_case keep KEEP_120 max9296-keep.ko
run_case sm01 SM01_120 max9296-sm01.ko
run_case sm02 SM02_120 max9296-sm02.ko

echo "Qualification cases completed; production restore follows via EXIT trap."
