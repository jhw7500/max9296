#!/bin/bash
# max9296.ko 을 pim-package 배포 트리에 복사하고 매니페스트를 맞춘다.
#
# 예전에는 복사만 했다. 그러면 pim-package 쪽 .github/binary-manifest.json 을 손으로
# 고쳐야 하고, 잊으면 Binary Verify 가 경고만 남긴 채 어긋난 상태로 머지된다.
# 이제 복사 직후 매니페스트까지 맞춘다. 예전 동작은 --no-manifest 로 남겨 두었다.

set -eu

HERE="$(cd "$(dirname "$0")" && pwd)"
ARTIFACT="$HERE/max9296.ko"
DEST_REL="dist/pim/opt/pim/driver/max9296.ko"
BUILD_HINT="./make-for-imx8"

# 기본 대상은 이 저장소와 나란히 있는 pim-package-jhw. 다른 곳에 두었으면
# --pim-dir 이나 PIM_PACKAGE_DIR 로 지정한다.
PIM_PACKAGE_DIR="${PIM_PACKAGE_DIR:-$(dirname "$HERE")/pim-package-jhw}"
update_manifest=1

die() {
    echo "update_bin: $*" >&2
    exit 1
}

usage() {
    cat <<'USAGE'
update_bin.sh — max9296.ko 을 pim-package 배포 트리에 복사하고 매니페스트를 맞춘다.

  ./update_bin.sh                          복사 + 매니페스트 갱신
  ./update_bin.sh --no-manifest            복사만 (예전 동작)
  ./update_bin.sh --pim-dir <경로>         대상 트리 지정
  PIM_PACKAGE_DIR=<경로> ./update_bin.sh   같은 것을 환경변수로

매니페스트 갱신은 pim-package 의 tools/verify_binaries.py --update 를 불러
sha256/size/mode/arch 를 실측 기입하고 source.commit 을 이 저장소 HEAD 로 맞춘다.
module_version 과 required_strings 는 자동으로 바뀌지 않는다 - 손으로 고친다.
USAGE
}

while [ $# -gt 0 ]; do
    case "$1" in
        --pim-dir)
            shift
            [ $# -gt 0 ] || die "--pim-dir 뒤에 경로가 필요하다"
            PIM_PACKAGE_DIR="$1"
            ;;
        --pim-dir=*) PIM_PACKAGE_DIR="${1#*=}" ;;
        --no-manifest) update_manifest=0 ;;
        -h|--help) usage; exit 0 ;;
        *) die "모르는 인자: $1 (--help 참고)" ;;
    esac
    shift
done

[ -f "$ARTIFACT" ] || die "빌드 산출물이 없다: $ARTIFACT — 먼저 $BUILD_HINT"
[ -d "$PIM_PACKAGE_DIR" ] || die "pim-package 트리가 없다: $PIM_PACKAGE_DIR"
PIM_PACKAGE_DIR="$(cd "$PIM_PACKAGE_DIR" && pwd)"

dest="$PIM_PACKAGE_DIR/$DEST_REL"
dest_dir="$(dirname "$dest")"
[ -d "$dest_dir" ] || die "배포 경로가 없다: $dest_dir — pim-package 트리가 맞는지 확인한다"

# 내용이 그대로면 출처 커밋도 건드리지 않는다. 재빌드 없이 돌리기만 해도
# source.commit 이 밀려나면 의미 없는 diff 만 쌓인다.
changed=1
if [ -f "$dest" ] && cmp -s "$ARTIFACT" "$dest"; then
    changed=0
fi

cp "$ARTIFACT" "$dest"
echo "복사: $ARTIFACT"
echo "  -> $dest"
[ "$changed" -eq 1 ] || echo "  (내용 동일 — 출처 커밋은 그대로 둔다)"

[ "$update_manifest" -eq 1 ] || exit 0

verifier="$PIM_PACKAGE_DIR/tools/verify_binaries.py"
[ -f "$PIM_PACKAGE_DIR/.github/binary-manifest.json" ] \
    || die "매니페스트가 없다: $PIM_PACKAGE_DIR/.github/binary-manifest.json (건너뛰려면 --no-manifest)"
[ -f "$verifier" ] || die "대조기가 없다: $verifier (건너뛰려면 --no-manifest)"
command -v python3 >/dev/null 2>&1 || die "python3 가 없다 (건너뛰려면 --no-manifest)"

verify_args=(--update "$DEST_REL")

if [ "$changed" -eq 1 ]; then
    commit="$(git -C "$HERE" rev-parse --short=7 HEAD)"
    verify_args+=(--set-commit "$commit")

    # 작업트리가 더러우면 산출물이 HEAD 와 다를 수 있다. 막지는 않되 무엇이
    # 그 커밋에 빠져 있는지 보여준다.
    dirty="$(git -C "$HERE" status --porcelain)"
    if [ -n "$dirty" ]; then
        {
            echo "경고: 작업트리가 깨끗하지 않다. 기록할 커밋은 $commit 인데"
            echo "      아래 변경은 그 커밋에 들어 있지 않다:"
            while IFS= read -r line; do echo "        $line"; done <<<"$dirty"
        } >&2
    fi
fi

echo
python3 "$verifier" "${verify_args[@]}"
