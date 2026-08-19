#!/usr/bin/env bash
#
# cam_fps_matrix.sh - 채널 구성 4가지에 대해 fps 를 계층별로 측정한다.
#
# 목적: 60fps 가 안 나오는 이유가 ISI 때문인지, 카메라 F/W · AP1302 ISP ·
#       SERDES 설정 때문인지 가른다.
#
#   케이스 A  ch0/ch1 듀얼   video4 3840x1080   디시리얼라이저 1개가 2채널
#   케이스 B  ch2/ch3 듀얼   video3 3840x1080   반대쪽 디시리얼라이저 1개가 2채널
#   케이스 C  ch0 + ch3      video4 + video3 각 1920x1080 동시
#                                              디시리얼라이저 2개가 각 1채널
#   케이스 D  단일 1채널     video4 1920x1080   디시리얼라이저 1개가 1채널
#
#   읽는 법:
#     A·B 는 낮고 C·D 는 요청대로 나오면  -> 듀얼(3840 폭) 경로의 한계.
#                                            ISI 또는 듀얼 레지스터 테이블.
#     D 도 60 이 안 나오면                -> 카메라 F/W·ISP 가 애초에 60 을 못 냄.
#     C 는 되는데 A 가 안 되면            -> 한 디시리얼라이저에 2채널을 묶는
#                                            구성(대역폭/테이블)의 문제.
#     CSI2 프레임수 >> ISI 프레임수       -> ISI 병목 (구성 무관하게 확인 가능)
#
# 사용법:
#   cam_fps_matrix.sh [옵션]
#     -f, --fps   "60"       측정할 fps 목록. 여러 개면 순차 측정. 기본 "30 60"
#     -c, --count N          케이스당 캡처 프레임 수. 기본 300
#     -t, --timeout N        v4l2-ctl 타임아웃(초). 기본 40
#     -k, --cases "A B C D"  실행할 케이스. 기본 전부
#     -h, --help
#
# 주의: cam-operate 를 정지시킨 상태에서 돌린다. 끝나면 원래 상태로 돌리지
#       않으므로 필요하면 `systemctl start cam-operate` 를 직접 하십시오.

set -u

FPS_LIST="30 60"
COUNT=300
TIMEOUT=40
CASES="A B C D"
SELF_DIR=$(cd "$(dirname "$0")" && pwd)
PROBE="$SELF_DIR/cam_fps_probe.sh"
RESET="$SELF_DIR/cam_hard_reset.sh"

while [ $# -gt 0 ]; do
	case "$1" in
	-f | --fps)
		FPS_LIST="$2"
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
	-k | --cases)
		CASES="$2"
		shift
		;;
	-h | --help)
		sed -n '2,36p' "$0" | sed 's/^# \{0,1\}//'
		exit 0
		;;
	*)
		echo "알 수 없는 옵션: $1" >&2
		exit 1
		;;
	esac
	shift
done

[ -x "$PROBE" ] || {
	echo "$PROBE 없음/실행권한 없음" >&2
	exit 1
}
[ -x "$RESET" ] || {
	echo "$RESET 없음/실행권한 없음" >&2
	exit 1
}

case_nodes() { case "$1" in A) echo "4" ;; B) echo "3" ;; C) echo "3 4" ;; D) echo "4" ;; esac }
case_wh() { case "$1" in A | B) echo "3840 1080" ;; C | D) echo "1920 1080" ;; esac }
case_desc() {
	case "$1" in
	A) echo "ch0/ch1 듀얼 (video4, 3840x1080)" ;;
	B) echo "ch2/ch3 듀얼 (video3, 3840x1080)" ;;
	C) echo "ch0 + ch3 동시 (video4+video3, 각 1920x1080)" ;;
	D) echo "단일 1채널 (video4, 1920x1080)" ;;
	esac
}

echo "========================================================================"
echo " cam_fps_matrix  fps=[$FPS_LIST]  케이스=[$CASES]  프레임=$COUNT"
echo "========================================================================"

# gstApp 이 노드를 잡고 있으면 측정이 안 된다.
if systemctl is-active --quiet cam-operate.service 2>/dev/null || pgrep gstApp >/dev/null 2>&1; then
	echo "cam-operate/gstApp 이 동작 중입니다. 정지시킵니다."
	systemctl stop cam-operate.service 2>/dev/null
	pkill -9 gstApp 2>/dev/null
	sleep 3
fi

for F in $FPS_LIST; do
	for K in $CASES; do
		NODES=$(case_nodes "$K")
		read -r W H <<<"$(case_wh "$K")"
		echo
		echo "------------------------------------------------------------------------"
		echo " [케이스 $K] $(case_desc "$K")   요청 ${F}fps"
		echo "------------------------------------------------------------------------"
		# 케이스마다 하드 리셋해서 직전 시험의 잔여 상태를 배제한다.
		"$RESET" -q || {
			echo "  리셋 실패 - 이 케이스 건너뜀"
			continue
		}
		sleep 2
		"$PROBE" -n "$NODES" -w "$W" -H "$H" -f "$F" -c "$COUNT" -t "$TIMEOUT"
	done
done

echo
echo "========================================================================"
echo " 완료. cam-operate 는 정지 상태입니다 (systemctl start cam-operate)."
echo "========================================================================"
