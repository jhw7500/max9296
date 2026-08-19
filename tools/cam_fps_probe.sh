#!/usr/bin/env bash
#
# cam_fps_probe.sh - ISP/SERDES 출력 레이트와 ISI 통과 레이트를 갈라서 측정한다.
#
# 왜 gstApp 으로는 못 재는가:
#   gstApp 파이프라인에는 videorate 와 caps 협상이 들어 있다. videorate 는
#   프레임을 복제/폐기해 목표 caps 에 맞추므로 하류에서 관측한 fps 는 소스의
#   진짜 레이트가 아니다. 협상된 caps 역시 "합의값"이지 실측값이 아니다.
#
# 어떻게 가르는가 - 두 계층을 따로 센다:
#   (a) CSI2 "Frame Start/End events"  = 디시리얼라이저가 MIPI 로 실제 보낸
#       프레임 수. 곧 카메라 F/W + AP1302 ISP + SERDES 가 낸 출력 레이트.
#       (imx8-mipi-csi2-sam 이 STREAMON 에서 카운터를 0 으로 지우고
#        STREAMOFF 에서 debug>0 이면 로그로 뱉는다)
#   (b) ISI 인터럽트 수 / v4l2 DQBUF 레이트 = ISI 를 통과해 메모리에 쓰인 수.
#
#   판정:
#     (a) 높고 (b) 낮으면            -> ISI 병목
#     (a) 가 요청 fps 에 못 미치면   -> 카메라 F/W · ISP · SERDES 가 못 냄
#
# 사용법:
#   cam_fps_probe.sh [옵션]
#     -n, --nodes "4"     측정할 video 노드 번호. 공백으로 여러 개 = 동시 측정
#     -w, --width  N      기본 3840
#     -H, --height N      기본 1080
#     -f, --fps    N      요청 fps. 기본 30
#     -c, --count  N      캡처할 프레임 수. 기본 300
#     -t, --timeout N     v4l2-ctl 타임아웃(초). 기본 40
#     -r, --reset         측정 전 cam_hard_reset.sh 실행
#     -h, --help
#
# 예:
#   cam_fps_probe.sh -r -n 4 -w 3840 -H 1080 -f 60      # ch0/1 듀얼 60fps
#   cam_fps_probe.sh -r -n "3 4" -w 1920 -H 1080 -f 60  # ch0/3 동시 단일

set -u

NODES="4"
WIDTH=3840
HEIGHT=1080
FPS=30
COUNT=300
TIMEOUT=40
DO_RESET=0
PIXFMT=RGBP
SELF_DIR=$(cd "$(dirname "$0")" && pwd)

while [ $# -gt 0 ]; do
	case "$1" in
	-n | --nodes)
		NODES="$2"
		shift
		;;
	-w | --width)
		WIDTH="$2"
		shift
		;;
	-H | --height)
		HEIGHT="$2"
		shift
		;;
	-f | --fps)
		FPS="$2"
		shift
		;;
	-c | --count)
		COUNT="$2"
		shift
		;;
	-t | --timeout)
		TIMEOUT="$2"
		shift
		;;
	-r | --reset) DO_RESET=1 ;;
	-h | --help)
		sed -n '2,40p' "$0" | sed 's/^# \{0,1\}//'
		exit 0
		;;
	*)
		echo "알 수 없는 옵션: $1" >&2
		exit 1
		;;
	esac
	shift
done

# video 노드 -> (max9296 subdev, CSI2 subdev 이름, CSI2 irq, ISI irq)
#   video3 = mxc_isi.0 <- mxc-mipi-csi2.0 <- max9296 1-0048 (i2c1, ch2/ch3)
#   video4 = mxc_isi.1 <- mxc-mipi-csi2.1 <- max9296 2-0048 (i2c2, ch0/ch1)
map_subdev() { case "$1" in 3) echo "max9296 1-0048" ;; 4) echo "max9296 2-0048" ;; *) echo "" ;; esac }
map_csiname() { case "$1" in 3) echo "mxc-mipi-csi2.0" ;; 4) echo "mxc-mipi-csi2.1" ;; *) echo "" ;; esac }
map_isiirq() { case "$1" in 3) echo "32e00000.isi" ;; 4) echo "32e02000.isi" ;; *) echo "" ;; esac }
map_label() { case "$1" in 3) echo "ch2/ch3 (i2c1)" ;; 4) echo "ch0/ch1 (i2c2)" ;; *) echo "?" ;; esac }

NCPU=$(nproc 2>/dev/null || echo 4)
irq_of() { # $1 = /proc/interrupts 마지막 필드 이름
	awk -v d="$1" -v n="$NCPU" '$NF==d { s=0; for (i=2; i<=n+1; i++) s+=$i; print s+0; exit }' /proc/interrupts
}

for N in $NODES; do
	if [ -z "$(map_subdev "$N")" ]; then
		echo "지원하지 않는 노드: video$N (3 또는 4)" >&2
		exit 1
	fi
	if [ ! -e "/dev/video$N" ]; then
		echo "/dev/video$N 없음 - 먼저 -r 로 리셋하거나 모듈을 올리십시오" >&2
		exit 1
	fi
done

if [ "$DO_RESET" -eq 1 ]; then
	echo "### 하드 리셋"
	"$SELF_DIR/cam_hard_reset.sh" -q || exit 1
	sleep 2
fi

# CSI2 카운터 로그를 켠다 (STREAMOFF 에서 이벤트 카운터를 찍게 하는 스위치)
DBG=/sys/module/imx8_mipi_csi2_sam/parameters/debug
DBG_OLD=""
if [ -w "$DBG" ]; then
	DBG_OLD=$(cat "$DBG")
	echo 1 >"$DBG"
else
	echo "경고: $DBG 없음 - CSI2 이벤트 카운터를 못 읽습니다 (ISI 측만 측정)" >&2
fi

echo "### 측정: ${WIDTH}x${HEIGHT} @ ${FPS}fps, 노드=[$NODES], 프레임=$COUNT"

# max9296 이 i2c 로그를 대량으로 뿜어 링버퍼가 금방 감긴다. 줄 오프셋으로
# "이번 스트림 구간"을 잡으면 버퍼가 감길 때 카운터 줄을 통째로 놓치므로,
# 직전 내용을 파일로 보존한 뒤 버퍼를 비우고 시작한다.
dmesg >/tmp/fps_probe_dmesg_prev.txt 2>/dev/null
dmesg -C 2>/dev/null

for N in $NODES; do
	media-ctl -V "\"$(map_subdev "$N")\":0 [fmt:UYVY8_2X8/${WIDTH}x${HEIGHT}@1/${FPS}]" 2>/dev/null
done

declare -A ISI0
for N in $NODES; do
	ISI0[$N]=$(irq_of "$(map_isiirq "$N")")
done

T0=$(date +%s%N)
PIDS=""
for N in $NODES; do
	timeout "$TIMEOUT" v4l2-ctl -d "/dev/video$N" \
		--set-fmt-video=width=${WIDTH},height=${HEIGHT},pixelformat=${PIXFMT} \
		--stream-mmap --stream-count="$COUNT" >"/tmp/fps_probe_v$N.txt" 2>&1 &
	PIDS="$PIDS $!"
done
for p in $PIDS; do wait "$p" 2>/dev/null; done
T1=$(date +%s%N)
ELAPSED=$(awk -v a="$T0" -v b="$T1" 'BEGIN{printf "%.2f", (b-a)/1e9}')

sleep 1 # STREAMOFF 로그가 dmesg 에 실릴 시간

declare -A ISI1
for N in $NODES; do
	ISI1[$N]=$(irq_of "$(map_isiirq "$N")")
done

dmesg >/tmp/fps_probe_dmesg.txt 2>/dev/null
NEWLOG=$(cat /tmp/fps_probe_dmesg.txt)

echo
printf '%-16s %-14s %10s %10s %10s %10s %10s\n' \
	"노드" "채널" "요청fps" "전달fps" "CSI2프레임" "ISI프레임" "소스fps"
printf '%s\n' "--------------------------------------------------------------------------------------------"

for N in $NODES; do
	CSINAME=$(map_csiname "$N")

	# (b) v4l2 가 실제로 뽑아준 레이트. v4l2-ctl 이 스트리밍 구간에서 직접 계산한 값.
	DELIV=$(grep -oE '[0-9]+\.[0-9]+ fps' "/tmp/fps_probe_v$N.txt" 2>/dev/null | tail -1 | awk '{print $1}')
	[ -z "$DELIV" ] && DELIV=0

	# (a) 디시리얼라이저가 MIPI 로 보낸 프레임 수
	CSI_FRAMES=$(printf '%s\n' "$NEWLOG" | grep -F "$CSINAME: Frame Start events:" | tail -1 |
		grep -oE '[0-9]+$')
	[ -z "$CSI_FRAMES" ] && CSI_FRAMES=$(printf '%s\n' "$NEWLOG" |
		grep -F "$CSINAME: Frame End events:" | tail -1 | grep -oE '[0-9]+$')
	[ -z "$CSI_FRAMES" ] && CSI_FRAMES=-1

	ISI_FRAMES=$((${ISI1[$N]} - ${ISI0[$N]}))

	# 소스 fps: 두 카운터가 같은 구간을 덮으므로 비율로 환산한다. 이렇게 하면
	# 펌웨어 로드 같은 스트림 전 대기시간이 결과를 희석하지 않는다.
	if [ "$CSI_FRAMES" -gt 0 ] && [ "$ISI_FRAMES" -gt 0 ]; then
		SRC=$(awk -v d="$DELIV" -v c="$CSI_FRAMES" -v i="$ISI_FRAMES" \
			'BEGIN{printf "%.2f", d*c/i}')
	else
		SRC="-"
	fi

	printf '%-16s %-14s %10s %10s %10s %10s %10s\n' \
		"/dev/video$N" "$(map_label "$N")" "$FPS" "$DELIV" "$CSI_FRAMES" "$ISI_FRAMES" "$SRC"
done

echo
echo "경과 ${ELAPSED}s (스트림 준비시간 포함)"
echo "주의: 전달fps 는 v4l2-ctl 이 캡처 전 구간(초기 램프 포함)으로 계산한 값이라"
echo "      정상상태보다 2~3fps 낮게 나온다. 정확한 정상상태 값은 cam_fps_watch.sh."

# ------------------------------------------------------------------- 판정
echo
echo "### 판정"
for N in $NODES; do
	CSINAME=$(map_csiname "$N")
	CSI_FRAMES=$(printf '%s\n' "$NEWLOG" | grep -F "$CSINAME: Frame Start events:" | tail -1 | grep -oE '[0-9]+$')
	[ -z "$CSI_FRAMES" ] && CSI_FRAMES=-1
	ISI_FRAMES=$((${ISI1[$N]} - ${ISI0[$N]}))
	DELIV=$(grep -oE '[0-9]+\.[0-9]+ fps' "/tmp/fps_probe_v$N.txt" 2>/dev/null | tail -1 | awk '{print $1}')
	[ -z "$DELIV" ] && DELIV=0

	printf '  /dev/video%s: ' "$N"
	if [ "$ISI_FRAMES" -le 1 ]; then
		echo "프레임 없음 - 파이프라인이 물렸거나 링크 이상 (cam_hard_reset.sh 후 재시도)"
	elif [ "$CSI_FRAMES" -lt 0 ]; then
		echo "전달 ${DELIV}fps. CSI2 카운터를 못 읽어 계층 구분 불가"
	elif [ "$CSI_FRAMES" -gt $((ISI_FRAMES * 3 / 2)) ]; then
		echo "ISI 병목 - 디시리얼라이저는 ${CSI_FRAMES}프레임 보냈는데 ISI 통과 ${ISI_FRAMES}프레임"
	elif awk -v d="$DELIV" -v f="$FPS" 'BEGIN{exit !(d < f*0.8)}'; then
		echo "카메라 F/W·ISP·SERDES 측 한계 - 소스가 요청 ${FPS}fps 를 못 냄 (전달 ${DELIV}fps, CSI2 ${CSI_FRAMES} ≈ ISI ${ISI_FRAMES}, ISI 손실 없음)"
	else
		echo "정상 범위 - 요청 ${FPS}fps 대비 전달 ${DELIV}fps"
	fi
done

# CSI2 에러 카운터가 잡혔으면 같이 보여준다
ERRS=$(printf '%s\n' "$NEWLOG" | grep -E "mxc-mipi-csi2\.[01]: (CRC|ECC|FIFO|Lost|Unknown)" |
	grep -vE "events: 0$")
if [ -n "$ERRS" ]; then
	echo
	echo "### CSI2 에러 이벤트"
	printf '%s\n' "$ERRS" | sed 's/^/  /'
fi

[ -n "$DBG_OLD" ] && echo "$DBG_OLD" >"$DBG"
exit 0
