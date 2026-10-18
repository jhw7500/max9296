#!/bin/bash
# #110 보드 검증 (AC2·AC5, .4 pim). 원본 모듈로 N회, 새 모듈로 N회 재부팅하고 매 부팅 직후
# dmesg·prepare·health_raw·cam_state 를 모은다. 끝나면(중단돼도) 원본 모듈로 복구하고 재부팅해 확인한다.
# 인자: <새 .ko 경로> <새 srcversion> <출력 디렉터리>. N 은 환경변수(기본 10).
set -u
NEW_KO=$1
NEW_SRC=$2
OUT=$3
N=${N:-10}
HOST=root@192.168.214.4
B=(ssh -o BatchMode=yes -o ConnectTimeout=5 "$HOST")
K=/lib/modules/5.10.35-lts-5.10.y+g2fce14defc04/kernel/drivers/media/i2c/max9296.ko
ORIG_SRC=7F99CB433CAC065390A1E71
ORIG_SHA=e6decd0e0f93
BACKUP=/root/t110/max9296.ko.orig-7F99CB43
SWAPPED=0
mkdir -p "$OUT"

say() { echo "$(date -u +%H:%M:%S) $*" | tee -a "$OUT/summary.txt"; }
rsh() { "${B[@]}" "$@" 2>/dev/null; }

# 재부팅하고 새 boot id 가 보인 뒤 cam_state 가 healthy 가 되기를 최대 180 s 기다린다.
# stdout: 새 boot id. 실패면 1.
reboot_wait() {
  local old new st t
  old=$(rsh 'cat /proc/sys/kernel/random/boot_id')
  rsh 'sync; systemctl reboot' >/dev/null
  new=
  for t in $(seq 1 60); do
    sleep 5
    new=$(rsh 'cat /proc/sys/kernel/random/boot_id')
    [ -n "$new" ] && [ "$new" != "$old" ] && break
    new=
  done
  [ -n "$new" ] || return 1
  for t in $(seq 1 36); do
    st=$(rsh 'cat /tmp/cam_state/state')
    [ "$st" = healthy ] && break
    sleep 5
  done
  echo "$new"
}

collect() {  # $1 = 디렉터리 이름
  local dir="$OUT/$1"
  mkdir -p "$dir"
  # 값마다 printf 로 줄을 끝낸다 — cam_state 파일은 줄바꿈 없이 끝나 다음 줄과 붙는다.
  rsh 'for f in /proc/sys/kernel/random/boot_id /sys/module/max9296/srcversion /tmp/cam_state/state;
       do printf "%s\n" "$(cat $f)"; done;
       for d in 1-0048 2-0048; do echo "== $d"; printf "%s\n" "$(cat /sys/bus/i2c/devices/$d/prepare)";
       printf "%s\n" "$(cat /sys/bus/i2c/devices/$d/health_raw)"; done' > "$dir/state.txt"
  rsh 'dmesg' > "$dir/dmesg.txt"
}

restore() {
  [ "$SWAPPED" = 1 ] || return 0
  say "restore: 원본 모듈 복구 시작"
  rsh "cp $BACKUP $K && sync && sha256sum $K | cut -c1-12" | tee -a "$OUT/summary.txt"
  local id
  id=$(reboot_wait) || { say "restore: 재부팅 확인 실패 — 수동 확인 필요"; return 1; }
  collect restore
  local src st
  src=$(sed -n 2p "$OUT/restore/state.txt")
  st=$(sed -n 3p "$OUT/restore/state.txt")
  say "restore: boot=${id:0:8} srcversion=$src cam_state=$st"
  [ "$src" = "$ORIG_SRC" ] && [ "$st" = healthy ] && SWAPPED=0
}
trap 'restore' EXIT

# 한 번의 부팅을 판정한다. $1=phase(orig|new) $2=회차. 0 이면 계속.
judge() {
  local phase=$1 i=$2 dir="$OUT/$1-$2" src st passed failed okjson
  src=$(sed -n 2p "$dir/state.txt")
  st=$(sed -n 3p "$dir/state.txt")
  passed=$(grep -c 'AP1302 boot check passed' "$dir/dmesg.txt")
  failed=$(grep -c 'AP1302 boot check failed' "$dir/dmesg.txt")
  okjson=$(grep -c '"isp_boot":{"status":"OK","epoch_current":true' "$dir/state.txt")
  say "$phase#$i boot=$(sed -n 1p "$dir/state.txt" | cut -c1-8) src=$src cam_state=$st pass_lines=$passed fail_lines=$failed isp_boot_ok=$okjson"
  local want=$ORIG_SRC
  [ "$phase" = new ] && want=$NEW_SRC
  [ "$src" = "$want" ] || { say "$phase#$i 중단: srcversion 불일치"; return 1; }
  [ "$st" = healthy ] || { say "$phase#$i 중단: cam_state != healthy"; return 1; }
  if [ "$phase" = new ]; then
    [ "$failed" = 0 ] || { say "$phase#$i 중단: boot check failed 줄"; return 1; }
    [ "$passed" -ge 2 ] && [ "$okjson" = 2 ] || { say "$phase#$i 중단: 통과 줄 또는 isp_boot OK 부족"; return 1; }
  fi
  return 0
}

say "begin N=$N new=$NEW_SRC sha=$(sha256sum "$NEW_KO" | cut -c1-12)"
pre=$(rsh "cat /sys/module/max9296/srcversion; sha256sum $K | cut -c1-12; cat /tmp/cam_state/state")
say "pre: $(echo "$pre" | tr '\n' ' ')"
[ "$(echo "$pre" | sed -n 1p)" = "$ORIG_SRC" ] && [ "$(echo "$pre" | sed -n 2p)" = "$ORIG_SHA" ] || { say "사전 상태가 원본이 아님 — 중단"; exit 2; }

for i in $(seq 1 "$N"); do
  reboot_wait >/dev/null || { say "orig#$i 재부팅 실패 — 중단"; exit 1; }
  collect "orig-$i"
  judge orig "$i" || exit 1
done

rsh "mkdir -p /root/t110 && cp -p $K $BACKUP && sha256sum $BACKUP | cut -c1-12" | tee -a "$OUT/summary.txt"
scp -q -o BatchMode=yes "$NEW_KO" "$HOST:/root/t110/max9296.ko.new" || { say "scp 실패 — 중단"; exit 1; }
rsha=$(rsh 'sha256sum /root/t110/max9296.ko.new | cut -c1-64')
[ "$rsha" = "$(sha256sum "$NEW_KO" | cut -c1-64)" ] || { say "보드 사본 sha 불일치 — 중단"; exit 1; }
SWAPPED=1
rsh "cp /root/t110/max9296.ko.new $K && sync" || { say "설치 실패 — 복구"; exit 1; }
say "swap: $(rsh "modinfo -F srcversion $K")"

for i in $(seq 1 "$N"); do
  reboot_wait >/dev/null || { say "new#$i 재부팅 실패 — 중단"; exit 1; }
  collect "new-$i"
  judge new "$i" || exit 1
done
say "done: orig $N, new $N"
