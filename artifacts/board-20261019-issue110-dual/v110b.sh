#!/bin/bash
# #110 AC1·AC3 보드 측정 (192.168.214.6, 문제 카메라, dual-wide). 패키지 원본 모듈로 N회, 검출 빌드로 N회
# S/W 재부팅하고, 매 부팅 가동 75 s 뒤 dmesg·prepare·health_raw·gstApp prepare 로그·CSI IRQ 진행과
# i2ctransfer 로 읽은 ISP별 R0x2000(독립 정답)을 모은다. 끝나면(중단돼도) 처음 발견한 모듈로 되돌린다.
# 인자: <검출 빌드 .ko> <검출 빌드 srcversion> <출력 디렉터리>. N 은 환경변수(기본 15).
set -u
NEW_KO=$1
NEW_SRC=$2
OUT=$3
N=${N:-15}
HOST=root@192.168.214.6
B=(ssh -o BatchMode=yes -o ConnectTimeout=5 "$HOST")
K=/lib/modules/5.10.35-lts-5.10.y+g2fce14defc04/kernel/drivers/media/i2c/max9296.ko
PKG=/opt/pim/driver/max9296.ko
PKG_SRC=7F99CB433CAC065390A1E71
FOUND_SRC=898534AABBA3AA941B748B7
FOUND_SHA=9f197c445f9f
DIR=/root/t110b
FOUND_BACKUP=$DIR/max9296.ko.found-898534AA
SETTLE=75
SWAPPED=0
mkdir -p "$OUT"

say() { echo "$(date -u +%H:%M:%S) $*" | tee -a "$OUT/summary.txt"; }
rsh() { "${B[@]}" "$@" 2>/dev/null; }

# 재부팅하고 새 boot id 를 본 뒤 가동 시간이 SETTLE 초가 될 때까지 기다린다. 실패면 1.
reboot_wait() {
  local old new up
  old=$(rsh 'cat /proc/sys/kernel/random/boot_id')
  rsh 'sync; systemctl reboot' >/dev/null
  new=
  for _ in $(seq 1 60); do
    sleep 5
    new=$(rsh 'cat /proc/sys/kernel/random/boot_id')
    [ -n "$new" ] && [ "$new" != "$old" ] && break
    new=
  done
  [ -n "$new" ] || return 1
  for _ in $(seq 1 40); do
    up=$(rsh 'cut -d. -f1 /proc/uptime')
    [ -n "$up" ] && [ "$up" -ge "$SETTLE" ] && return 0
    sleep 3
  done
  return 1
}

collect() {  # $1 = 디렉터리 이름
  local dir="$OUT/$1"
  mkdir -p "$dir"
  # 값마다 printf 로 줄을 끝낸다 — /tmp 상태 파일은 줄바꿈 없이 끝날 수 있다.
  rsh 'for f in /proc/sys/kernel/random/boot_id /sys/module/max9296/srcversion /proc/uptime /tmp/cam_state/state /tmp/file_check;
       do printf "%s\n" "$(cat $f 2>/dev/null)"; done;
       printf "boot_wall %s\n" "$(date -u -d "@$(( $(date +%s) - $(cut -d. -f1 /proc/uptime) ))" +%Y-%m-%dT%H:%M:%S)";
       for d in 1-0048 2-0048; do echo "== $d"; printf "%s\n" "$(cat /sys/bus/i2c/devices/$d/prepare)";
       printf "%s\n" "$(cat /sys/bus/i2c/devices/$d/health_raw)"; done;
       echo "== r0x2000"; for bus in 1 2; do for a in 0x11 0x12; do
         printf "bus%s %s %s\n" "$bus" "$a" "$(i2ctransfer -f -y $bus w2@$a 0x20 0x00 r2 2>&1 | tr "\n" " ")"; done; done;
       echo "== irq0"; grep -E "csi|isi" /proc/interrupts; sleep 2; echo "== irq1"; grep -E "csi|isi" /proc/interrupts' > "$dir/state.txt"
  rsh 'dmesg' > "$dir/dmesg.txt"
  rsh 'grep "MAX9296_PREPARE" /var/log/cantops/local0.log | tail -n 30' > "$dir/prepare-log.txt"
}

install_ko() {  # $1 = 보드 원본 경로, $2 = 기대 srcversion
  rsh "cp $1 $K && sync && modinfo -F srcversion $K" | grep -qx "$2"
}

restore() {
  [ "$SWAPPED" = 1 ] || return 0
  say "restore: 처음 모듈($FOUND_SRC) 복구"
  install_ko "$FOUND_BACKUP" "$FOUND_SRC" || { say "restore: 설치 확인 실패 — 수동 확인 필요"; return 1; }
  reboot_wait || { say "restore: 재부팅 확인 실패 — 수동 확인 필요"; return 1; }
  collect restore
  local src
  src=$(sed -n 2p "$OUT/restore/state.txt")
  say "restore: srcversion=$src cam_state=$(sed -n 4p "$OUT/restore/state.txt") sha=$(rsh "sha256sum $K | cut -c1-12")"
  [ "$src" = "$FOUND_SRC" ] && SWAPPED=0
}
trap 'restore' EXIT

run_phase() {  # $1 = phase 이름, $2 = 보드 모듈 경로, $3 = 기대 srcversion
  local phase=$1 src_path=$2 want=$3 i src
  install_ko "$src_path" "$want" || { say "$phase: 설치 확인 실패 — 중단"; exit 1; }
  say "$phase: installed $want"
  for i in $(seq 1 "$N"); do
    reboot_wait || { say "$phase#$i 재부팅 실패 — 중단"; exit 1; }
    collect "$phase-$i"
    src=$(sed -n 2p "$OUT/$phase-$i/state.txt")
    say "$phase#$i boot=$(sed -n 1p "$OUT/$phase-$i/state.txt" | cut -c1-8) src=$src cam=$(sed -n 4p "$OUT/$phase-$i/state.txt") r0x2000=$(grep -E '^bus' "$OUT/$phase-$i/state.txt" | awk '{print $3$4}' | tr '\n' ' ') fail_lines=$(grep -c 'AP1302 boot check failed' "$OUT/$phase-$i/dmesg.txt")"
    [ "$src" = "$want" ] || { say "$phase#$i 중단: srcversion 불일치"; exit 1; }
  done
}

say "begin N=$N new=$NEW_SRC sha=$(sha256sum "$NEW_KO" | cut -c1-12)"
pre=$(rsh "cat /sys/module/max9296/srcversion; sha256sum $K | cut -c1-12; modinfo -F srcversion $PKG")
say "pre: $(echo "$pre" | tr '\n' ' ')"
[ "$(echo "$pre" | sed -n 1p)" = "$FOUND_SRC" ] && [ "$(echo "$pre" | sed -n 2p)" = "$FOUND_SHA" ] && [ "$(echo "$pre" | sed -n 3p)" = "$PKG_SRC" ] || { say "사전 상태가 예상과 다름 — 중단"; exit 2; }

rsh "mkdir -p $DIR && cp -p $K $FOUND_BACKUP && sha256sum $FOUND_BACKUP | cut -c1-12" | tee -a "$OUT/summary.txt"
scp -q -o BatchMode=yes "$NEW_KO" "$HOST:$DIR/max9296.ko.new" || { say "scp 실패 — 중단"; exit 1; }
[ "$(rsh "sha256sum $DIR/max9296.ko.new | cut -c1-64")" = "$(sha256sum "$NEW_KO" | cut -c1-64)" ] || { say "보드 사본 sha 불일치 — 중단"; exit 1; }
SWAPPED=1

run_phase base "$PKG" "$PKG_SRC"
run_phase new "$DIR/max9296.ko.new" "$NEW_SRC"
say "done: base $N, new $N"
