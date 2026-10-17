#!/bin/bash
# #111 수정 보드 검증 (cam-operate·ord-operate 를 멈춘 상태에서 보드에서 실행).
# v4l2-ctl 로 두 디시리얼라이저의 시작·정지 순서를 직접 정한다.
#   video3 = 1-0048 (I2C:1, CSI0 32e40000), video4 = 2-0048 (I2C:2, CSI1 32e50000)
# 단계: A  두 노드 동시 스트리밍 후 정지 순서 2종 x10 (1..10: video3 먼저, 11..20: video4 먼저)
#       B  1-0048 을 잠깐 돌렸다 멈춘 뒤 2-0048 단독 90 프레임 x3
#       G  한쪽을 계속 돌리면서 다른 쪽을 3회 정지·재시작, CSI IRQ 를 50 ms 간격으로 기록
# 정지는 SIGTERM — 비대화형 셸의 백그라운드 작업은 SIGINT 를 무시한다.
set -u
OUT=/root/t111/v111-$(date -u +%H%M%S)
mkdir -p "$OUT"
say() { echo "$(date -u +%FT%T.%N | cut -c1-23) $*" | tee -a "$OUT/summary.txt"; }
start() { # $1 노드 번호, $2 태그, [$3 프레임 수]
  v4l2-ctl -d "/dev/video$1" --stream-mmap --stream-count="${3:-1000000}" \
    --stream-to=/dev/null > "$OUT/v$1-$2.txt" 2>&1 &
  echo $!
}
stop() {
  kill -TERM "$1" 2>/dev/null
  for _ in 1 2 3 4 5 6; do kill -0 "$1" 2>/dev/null || return 0; sleep 0.5; done
  kill -9 "$1" 2>/dev/null
  echo "stop: pid $1 needed SIGKILL" >> "$OUT/summary.txt"
}
csi() {
  awk '/32e40000\.csi/{s=0;for(i=2;i<=NF;i++){if($i~/^[0-9]+$/)s+=$i;else break};a=s}
       /32e50000\.csi/{s=0;for(i=2;i<=NF;i++){if($i~/^[0-9]+$/)s+=$i;else break};b=s}
       END{print a+0, b+0}' /proc/interrupts
}
fps() { grep -oE '[0-9]+\.[0-9]+ fps' "$1" 2>/dev/null | tail -1; }
health() {
  for b in 1 2; do
    echo "== $1 $b-0048/health_raw"
    cat "/sys/bus/i2c/devices/$b-0048/health_raw" 2>&1
  done >> "$OUT/health.txt"
}

say "begin srcversion=$(cat /sys/module/max9296/srcversion) boot=$(cut -c1-8 /proc/sys/kernel/random/boot_id)"
health start

say "A begin"
for i in $(seq 1 20); do
  c0=$(csi)
  p3=$(start 3 "a$i"); p4=$(start 4 "a$i")
  sleep 8
  c1=$(csi)
  if [ "$i" -le 10 ]; then
    say "A$i stop video3"; stop "$p3"; sleep 1; say "A$i stop video4"; stop "$p4"
  else
    say "A$i stop video4"; stop "$p4"; sleep 1; say "A$i stop video3"; stop "$p3"
  fi
  say "A$i csi_delta_8s=$(echo "$c0 $c1" | awk '{print $3-$1"/"$4-$2}') fps3=$(fps "$OUT/v3-a$i.txt") fps4=$(fps "$OUT/v4-a$i.txt")"
  sleep 2
done

say "B begin"
for j in 1 2 3; do
  p3=$(start 3 "b$j"); sleep 6; say "B$j stop video3"; stop "$p3"; sleep 1
  c0=$(csi)
  say "B$j start video4 alone"
  timeout 25 v4l2-ctl -d /dev/video4 --stream-mmap --stream-count=90 \
    --stream-to=/dev/null > "$OUT/v4-b$j.txt" 2>&1
  rc=$?
  c1=$(csi)
  say "B$j rc=$rc csi_delta=$(echo "$c0 $c1" | awk '{print $3-$1"/"$4-$2}') fps4=$(fps "$OUT/v4-b$j.txt") frames=$(grep -o '<' "$OUT/v4-b$j.txt" | wc -l)"
  sleep 2
done
health after-B

gap_run() { # $1 계속 도는 노드, $2 정지·재시작하는 노드
  ( while :; do echo "$(date +%s.%N) $(csi)"; sleep 0.05; done ) > "$OUT/irq-g$1.txt" &
  local sp=$! ps pt k
  ps=$(start "$1" "g$1"); sleep 6
  say "G$1 start video$2 (t0)"; pt=$(start "$2" "g$1-t0"); sleep 6
  for k in 1 2 3; do
    say "G$1 k$k stop video$2"; stop "$pt"; sleep 4
    say "G$1 k$k start video$2"; pt=$(start "$2" "g$1-t$k"); sleep 6
  done
  stop "$pt"; sleep 1; stop "$ps"; kill "$sp" 2>/dev/null
  say "G$1 done fps$1=$(fps "$OUT/v$1-g$1.txt")"
}
say "G begin"
gap_run 4 3
sleep 2
gap_run 3 4
health end
say "ALL DONE out=$OUT"
