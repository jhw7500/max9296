#!/bin/bash
set -eu

usage() {
  cat <<'EOF'
Usage: tools/build_360p_candidates.sh [options]

Clean-build isolated MAX9296 modules for AP1302 sensor-mode qualification.

Options:
  --output DIR     Unique output directory. The default is timestamped under
                   artifacts/360p-candidates/.
  --allow-dirty    Permit modified max9296.c or max9296_360p_policy.h.
  --dry-run        Print all 17 clean-build commands and artifact names only.
  -h, --help       Show this help.

Artifacts:
  max9296-keep.ko  Compile-time KEEP policy (0xff)
  max9296-sm00.ko ... max9296-sm15.ko
EOF
}

output_dir=
allow_dirty=0
dry_run=0

while [ "$#" -gt 0 ]; do
  case "$1" in
    --output)
      if [ "$#" -lt 2 ] || [ -z "$2" ]; then
        echo "ERROR: --output requires a directory" >&2
        exit 2
      fi
      output_dir=$2
      shift 2
      ;;
    --allow-dirty)
      allow_dirty=1
      shift
      ;;
    --dry-run)
      dry_run=1
      shift
      ;;
    -h|--help)
      usage
      exit 0
      ;;
    *)
      echo "ERROR: unknown option: $1" >&2
      usage >&2
      exit 2
      ;;
  esac
done

repo_root=$(git rev-parse --show-toplevel)
cd "$repo_root"
git_head=$(git rev-parse HEAD)

if [ -z "$output_dir" ]; then
  timestamp=$(date +%Y%m%d-%H%M%S)
  short_head=$(git rev-parse --short=12 HEAD)
  output_dir="artifacts/360p-candidates/${timestamp}-${short_head}"
fi

if [ -e "$output_dir" ]; then
  echo "ERROR: output directory already exists: $output_dir" >&2
  exit 1
fi

source_status=$(git status --porcelain -- max9296.c max9296_360p_policy.h)
if [ -n "$source_status" ] && [ "$allow_dirty" -ne 1 ]; then
  echo "ERROR: candidate inputs are dirty; commit them or pass --allow-dirty" >&2
  printf '%s\n' "$source_status" >&2
  exit 1
fi

worktree_diff=$(git diff --stat | tr '\n' ';')
staged_diff=$(git diff --cached --stat | tr '\n' ';')
[ -n "$worktree_diff" ] || worktree_diff=clean
[ -n "$staged_diff" ] || staged_diff=clean

build_one() {
  label=$1
  sensor_mode=$2
  artifact=$3
  compiler_mode="-DMAX9296_360P_SENSOR_MODE=${sensor_mode}"

  printf '+ ./make-for-imx8 clean\n'
  printf '+ ./make-for-imx8 KCFLAGS=%s\n' "$compiler_mode"
  printf 'artifact\t%s\t%s\n' "$artifact" "$label"

  if [ "$dry_run" -eq 1 ]; then
    return 0
  fi

  ./make-for-imx8 clean
  ./make-for-imx8 "KCFLAGS=${compiler_mode}"
  if [ ! -f max9296.ko ]; then
    echo "ERROR: build succeeded without max9296.ko for $label" >&2
    exit 1
  fi

  cp max9296.ko "$output_dir/$artifact"
  artifact_sha=$(sha256sum "$output_dir/$artifact" | awk '{print $1}')
  printf '%s\t%s\t%s\t%s\t%s\t%s\t%s\n' \
    "$artifact" "$label" "$git_head" "$worktree_diff" "$staged_diff" \
    "$compiler_mode" "$artifact_sha" >>"$output_dir/manifest.tsv"
}

if [ "$dry_run" -eq 1 ]; then
  printf 'output\t%s\n' "$output_dir"
  printf 'git_head\t%s\n' "$git_head"
else
  mkdir -p "$(dirname "$output_dir")"
  mkdir "$output_dir"
  printf 'artifact\tsensor_mode\tgit_head\tgit_diff_stat\tgit_cached_diff_stat\tcompiler_mode\tsha256\n' \
    >"$output_dir/manifest.tsv"
fi

build_one KEEP 255 max9296-keep.ko
mode=0
while [ "$mode" -le 15 ]; do
  artifact=$(printf 'max9296-sm%02d.ko' "$mode")
  build_one "$mode" "$mode" "$artifact"
  mode=$((mode + 1))
done

if [ "$dry_run" -ne 1 ]; then
  printf 'Built 17 candidate modules in %s\n' "$output_dir"
  printf 'Manifest: %s/manifest.tsv\n' "$output_dir"
fi
