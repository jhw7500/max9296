#!/usr/bin/env bash
#
# cam_fps_watch.sh - gstApp 을 그대로 돌리면서 실제 하드웨어 레이트를 잰다.
#
# gstApp 파이프라인 안(videorate/caps 협상 하류)에서 본 fps 는 소스의 진짜
# 레이트가 아니다. 하지만 /proc/interrupts 는 앱과 무관하게 하드웨어가 실제로
# 올린 인터럽트를 센다. 스트림을 끊지 않고 두 계층을 동시에 볼 수 있다.
#
#   CSI2 인터럽트  : MIPI 수신부가 올린 이벤트. 프레임당 2회(Frame Start/End).
#                    -> /2 하면 디시리얼라이저가 실제 보낸 프레임 레이트,
#                       곧 카메라 F/W · AP1302 ISP · SERDES 의 출력 레이트.
#   ISI 인터럽트   : ISI 가 메모리에 프레임을 쓴 횟수. 프레임당 1회.
#                    -> 앱이 실제로 받는 레이트.
#
#   CSI2/2 >> ISI  -> ISI 병목 (소스는 내는데 ISI 가 못 받음)
#   CSI2/2 ≈ ISI 인데 둘 다 요청보다 낮음 -> 소스가 그 레이트밖에 못 냄
#
# 주의: 프레임당 CSI2 인터럽트 2회는 정상 동작에서 실측된 값이다(30fps 에서
#       ISI +299/10s, CSI2 +598/10s). 에러 이벤트가 섞이면 비율이 달라질 수
#       있으므로 원시 카운트도 같이 출력한다.
#
# 사용법:
#   cam_fps_watch.sh [-d 초] [-i 간격]
#     -d, --duration N   총 관측 시간(초). 기본 20
#     -i, --interval N   샘플 간격(초). 기본 5
#     -h, --help

set -u

DURATION=20
INTERVAL=5

while [ $# -gt 0 ]; do
	case "$1" in
	-d | --duration)
		DURATION="$2"
		shift
		;;
	-i | --interval)
		INTERVAL="$2"
		shift
		;;
	-h | --help)
		sed -n '2,30p' "$0" | sed 's/^# \{0,1\}//'
		exit 0
		;;
	*)
		echo "알 수 없는 옵션: $1" >&2
		exit 1
		;;
	esac
	shift
done

NCPU=$(nproc 2>/dev/null || echo 4)
irq_of() {
	awk -v d="$1" -v n="$NCPU" '$NF==d { s=0; for (i=2; i<=n+1; i++) s+=$i; print s+0; exit }' /proc/interrupts
}

# video3 = ch2/ch3 (i2c1) : ISI 32e00000, CSI2 32e40000
# video4 = ch0/ch1 (i2c2) : ISI 32e02000, CSI2 32e50000
echo "gstApp: $(pgrep -a gstApp | head -1 || echo '실행 중 아님')"
echo "관측 ${DURATION}초 / ${INTERVAL}초 간격"
echo
printf '%-8s | %-34s | %-34s\n' "" "video3  ch2/ch3" "video4  ch0/ch1"
printf '%-8s | %10s %10s %10s | %10s %10s %10s\n' \
	"경과" "소스fps" "전달fps" "손실%" "소스fps" "전달fps" "손실%"
printf '%s\n' "---------+---------------------------------------+--------------------------------------"

C3P=$(irq_of 32e40000.csi)
I3P=$(irq_of 32e00000.isi)
C4P=$(irq_of 32e50000.csi)
I4P=$(irq_of 32e02000.isi)

T=0
while [ "$T" -lt "$DURATION" ]; do
	sleep "$INTERVAL"
	T=$((T + INTERVAL))

	C3=$(irq_of 32e40000.csi)
	I3=$(irq_of 32e00000.isi)
	C4=$(irq_of 32e50000.csi)
	I4=$(irq_of 32e02000.isi)

	read -r S3 D3 L3 <<<"$(awk -v c=$((C3 - C3P)) -v i=$((I3 - I3P)) -v s="$INTERVAL" 'BEGIN{
		src=c/2.0/s; dlv=i/1.0/s;
		loss = (src>0.01) ? (src-dlv)/src*100 : 0;
		printf "%.1f %.1f %.0f", src, dlv, loss }')"
	read -r S4 D4 L4 <<<"$(awk -v c=$((C4 - C4P)) -v i=$((I4 - I4P)) -v s="$INTERVAL" 'BEGIN{
		src=c/2.0/s; dlv=i/1.0/s;
		loss = (src>0.01) ? (src-dlv)/src*100 : 0;
		printf "%.1f %.1f %.0f", src, dlv, loss }')"

	printf '%-8s | %10s %10s %10s | %10s %10s %10s\n' "${T}s" "$S3" "$D3" "$L3" "$S4" "$D4" "$L4"

	C3P=$C3
	I3P=$I3
	C4P=$C4
	I4P=$I4
done

echo
echo "해석:"
echo "  소스fps = CSI2 인터럽트/2/초  -> 카메라 F/W·ISP·SERDES 가 실제 낸 레이트"
echo "  전달fps = ISI 인터럽트/초     -> 앱이 실제로 받은 레이트"
echo "  손실%   가 크면 ISI 병목, 소스fps 자체가 낮으면 카메라 측 한계"
