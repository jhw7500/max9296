#!/bin/bash
set -eu

cd "$(dirname "$0")/.."

script=tools/build_360p_candidates.sh
tmp_dir=$(mktemp -d)
trap 'rm -rf "$tmp_dir"' EXIT

if [ ! -x "$script" ]; then
  echo "FAIL: $script is missing or not executable" >&2
  exit 1
fi

help_text=$("$script" --help)
for token in --output --allow-dirty --dry-run max9296-keep.ko max9296-sm15.ko; do
  if ! printf '%s\n' "$help_text" | rg -F -- "$token" >/dev/null; then
    echo "FAIL: --help omits $token" >&2
    exit 1
  fi
done

plan=$("$script" --dry-run --allow-dirty --output "$tmp_dir/candidates")
build_count=$(printf '%s\n' "$plan" | rg -c '^\+ \./make-for-imx8 KCFLAGS=-DMAX9296_360P_SENSOR_MODE=')
artifact_count=$(printf '%s\n' "$plan" | rg -c '^artifact[[:space:]]')
if [ "$build_count" -ne 17 ] || [ "$artifact_count" -ne 17 ]; then
  echo "FAIL: expected 17 build plans and 17 artifacts, got $build_count/$artifact_count" >&2
  exit 1
fi

if ! printf '%s\n' "$plan" | rg -F -- \
    '+ ./make-for-imx8 KCFLAGS=-DMAX9296_360P_SENSOR_MODE=255 -DMAX9296_360P_MAX_FPS=120' >/dev/null; then
  echo "FAIL: KEEP qualification build lacks explicit mode/FPS policy" >&2
  exit 1
fi
if ! printf '%s\n' "$plan" | rg -F -- $'artifact\tmax9296-keep.ko\tKEEP' >/dev/null; then
  echo "FAIL: KEEP artifact is missing" >&2
  exit 1
fi

for mode in $(seq 0 15); do
  artifact=$(printf 'max9296-sm%02d.ko' "$mode")
  if ! printf '%s\n' "$plan" | rg -F -- \
      "+ ./make-for-imx8 KCFLAGS=-DMAX9296_360P_SENSOR_MODE=$mode -DMAX9296_360P_MAX_FPS=120" >/dev/null; then
    echo "FAIL: sensor mode $mode qualification flags are missing" >&2
    exit 1
  fi
  if ! printf '%s\n' "$plan" | rg -F -- $'artifact\t'"$artifact"$'\t'"$mode" >/dev/null; then
    echo "FAIL: artifact $artifact is missing" >&2
    exit 1
  fi
done

mkdir "$tmp_dir/existing"
if "$script" --dry-run --allow-dirty --output "$tmp_dir/existing" >/dev/null 2>&1; then
  echo "FAIL: existing output directory was accepted" >&2
  exit 1
fi

for contract in \
    'git status --porcelain -- max9296.c max9296_360p_policy.h' \
    'git rev-parse HEAD' \
    'git diff --stat' \
    'sha256sum' \
    'manifest.tsv'; do
  if ! rg -F -- "$contract" "$script" >/dev/null; then
    echo "FAIL: builder contract missing: $contract" >&2
    exit 1
  fi
done

# Reproduce the external-module clean behaviour: an output directory below the
# source tree must not lose artifacts from earlier candidate builds.
mock_repo="$tmp_dir/mock-repo"
mkdir -p "$mock_repo/tools"
cp "$script" "$mock_repo/tools/build_360p_candidates.sh"
touch "$mock_repo/max9296.c" "$mock_repo/max9296_360p_policy.h"
cat >"$mock_repo/make-for-imx8" <<'EOF'
#!/bin/bash
set -eu
if [ "${1:-}" = clean ]; then
  find . -type f -name '*.ko' -delete
  exit 0
fi
if [ -n "${MAX9296_TEST_FAIL_MODE:-}" ] &&
   printf '%s\n' "$*" | grep -q "MAX9296_360P_SENSOR_MODE=${MAX9296_TEST_FAIL_MODE}"; then
  exit 37
fi
printf '%s\n' "$*" >max9296.ko
EOF
chmod +x "$mock_repo/make-for-imx8"
git -C "$mock_repo" init -q
git -C "$mock_repo" -c user.name=test -c user.email=test@example.invalid \
  add tools/build_360p_candidates.sh make-for-imx8 max9296.c max9296_360p_policy.h
git -C "$mock_repo" -c user.name=test -c user.email=test@example.invalid \
  commit -q -m fixture

(
  cd "$mock_repo"
  tools/build_360p_candidates.sh --output artifacts/candidates >/dev/null
)
preserved_count=$(find "$mock_repo/artifacts/candidates" -maxdepth 1 \
  -type f -name '*.ko' | wc -l)
if [ "$preserved_count" -ne 17 ]; then
  echo "FAIL: clean builds preserved $preserved_count/17 candidate modules" >&2
  exit 1
fi
if [ -e "$mock_repo/max9296.ko" ]; then
  echo "FAIL: qualification module escaped into the generic build path" >&2
  exit 1
fi

# A production module that existed before qualification must survive byte for
# byte, including when a candidate build fails part way through.
printf 'known-production-module\n' >"$mock_repo/max9296.ko"
production_sha=$(sha256sum "$mock_repo/max9296.ko" | awk '{print $1}')
(
  cd "$mock_repo"
  tools/build_360p_candidates.sh --output artifacts/candidates-existing >/dev/null
)
restored_sha=$(sha256sum "$mock_repo/max9296.ko" | awk '{print $1}')
if [ "$restored_sha" != "$production_sha" ]; then
  echo "FAIL: successful qualification replaced the generic production module" >&2
  exit 1
fi

if (
  cd "$mock_repo"
  MAX9296_TEST_FAIL_MODE=2 tools/build_360p_candidates.sh \
    --output artifacts/candidates-failing >/dev/null 2>&1
); then
  echo "FAIL: injected candidate build failure unexpectedly succeeded" >&2
  exit 1
fi
restored_sha=$(sha256sum "$mock_repo/max9296.ko" | awk '{print $1}')
if [ "$restored_sha" != "$production_sha" ]; then
  echo "FAIL: failed qualification replaced the generic production module" >&2
  exit 1
fi

echo "PASS: 17 isolated 360p candidate build plans"
