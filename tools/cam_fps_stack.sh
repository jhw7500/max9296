#!/usr/bin/env bash
#
# cam_fps_stack.sh - 센서 → ISP → CSI2 → ISI 전 계층 프레임레이트 동시 측정
#
# gstApp 을 정지시키지 않고 파이프라인 네 지점을 같은 구간에서 재서, 프레임이
# 어느 구간에서 사라지는지 한 표로 보여준다.
#
#   AR0234 ──> AP1302 ──MIPI──> MAX9295 ══GMSL2══> MAX9296 ──CSI-2──> i.MX8MP
#     │          │                                              │        │
#   [센서]    [ISP출력]                                      [CSI2]    [ISI]
#   R0x00FC   R0x0002                                    /proc/interrupts
#
# | 계층 | 출처 | 의미 |
# |------|------|------|
# | 센서   | AP1302 `R0x00FC` SENSOR_TOTAL_FRAME_TIME | 1/TOTAL. AR0234 가 실제 도는 레이트 |
# | ISP    | AP1302 `R0x0002` HINF_FRAME_CNT 델타     | ISP 가 호스트 IF 로 내보낸 프레임 |
# | CSI2   | `/proc/interrupts` csi ÷ 2               | SoC MIPI 수신부가 받은 프레임 |
# | ISI    | `/proc/interrupts` isi                   | 메모리에 기록되어 앱이 받는 프레임 |
#
# AP1302 I2C 주소는 모드에 따라 다르다 (max9296.c:1968-1969):
#
#   단일 (1280x720, 1920x1080)  : ch0/ch1 둘 다 0x3c
#   듀얼 (2560x720, 3840x1080)  : ch0 = 0x11, ch1 = 0x12
#
# 이 스크립트는 0x11 응답 여부로 모드를 자동 판별하고, 듀얼이면 카메라 두 대를
# 각각 잰다. 듀얼에서 두 카메라 영상은 하나의 넓은 프레임(3840/2560)으로 합쳐져
# 전송되므로 CSI2/ISI 는 합성 프레임을 세며, 카메라별 ISP 출력과 같은 값이어야
# 한다(스케일 보정 불필요). 그래서 CSI2/ISI 열은 두 행에 같은 값이 표시된다.
#
# 주의:
#   - gstApp 자체의 fps 로는 못 잰다. 파이프라인에 videorate 와 caps 협상이 있어
#     프레임을 복제/폐기하므로 하류에서 본 값은 소스의 진짜 레이트가 아니다.
#   - i2c 읽기에 시간이 걸리므로 sleep 값이 아니라 실제 경과시간으로 나눈다.
#     (이걸 안 하면 ISP 가 센서보다 빠르게 나오는 물리적으로 불가능한 값이 뜬다)
#   - HINF_FRAME_CNT 는 8비트(256 순환)다. interval × fps < 256 이어야 하므로
#     60fps 기준 interval 은 4초 이하여야 한다. 기본 2초.
#   - CSI2 는 프레임당 2회 인터럽트(Frame Start + Frame End). 단일 1920x1080 과
#     1280x720 @30 에서 csi/isi 원시 비율 2.000 으로 실측 확인. 해상도 무관.
#   - 반면 ISI 의 "프레임당 1회" 는 조건부다. 4채널 듀얼 2560x720 @60 에서는
#     프레임 수가 59 fps 로 일정한데 ISI IRQ/frame 이 1.37~2.95 로 흔들렸다
#     (5초창 6회 실측). 정수도 고정도 아니라 나눠서 보정할 수 없다.
#     그런 구간은 ISI 값에 * 를 붙이고 판정에서 제외한다. 전달 레이트는 CSI2 열을
#     보면 된다(같은 구간에서 59 fps 로 일정했다).
#     초과분은 정상 프레임 완료가 아닌 다른 이벤트(오버플로/재시도/체인)로 보이나
#     원인 미확인. 프레임 수는 안정적이므로 영상 손실은 아니다.
#   - AP1302 는 전원 레일이 공용이라 스트리밍하지 않는 채널도 i2c 에 응답한다.
#     따라서 활성 여부는 ISI 인터럽트 증가로 판정한다.
#   - 계층별 카운터를 읽는 시점이 i2c 지연만큼 어긋나므로 샘플 하나하나는 ±2fps
#     정도 흔들린다. 평균에서 상쇄되니 판정은 평균 행과 '판정' 절을 보면 된다.
#     정밀도가 필요하면 -d 를 늘려 표본을 키운다.
#
# 사용법:
#   cam_fps_stack.sh [옵션]
#     -c, --channel ch01|ch23|both   기본 auto (실제 스트리밍 중인 디시리얼라이저)
#     -d, --duration N               총 관측 시간(초). 기본 20
#     -i, --interval N               샘플 간격(초). 기본 2
#     -D, --deep                     AR0234 레지스터를 직접 읽어 교차검증 (시작 시 1회)
#     -L, --label CASE               측정 Case 이름(추론하지 않고 결과에 그대로 기록)
#     -R, --requested-fps N           요청 FPS. 120 엄격 판정에 사용
#     -h, --help
#
# 예:
#   cam_fps_stack.sh                    # 자동, 20초
#   cam_fps_stack.sh -d 60 -i 3         # 60초, 3초 간격
#   cam_fps_stack.sh -c ch01 -D         # ch0/ch1 쪽, AR0234 교차검증 포함

set -u

CHANNEL=auto
DURATION=20
INTERVAL=2
DEEP=0
CASE_LABEL=UNLABELED
REQUESTED_FPS=0
DMA_TOOL=${CAM_FPS_STACK_DMA_TOOL:-/opt/pim/bin/cam_ap1302_dma_verify.sh}
IRQ_FILE=${CAM_FPS_STACK_IRQ_FILE:-/proc/interrupts}

while [ $# -gt 0 ]; do
	case "$1" in
	-c | --channel)
		CHANNEL="$2"
		shift
		;;
	-d | --duration)
		DURATION="$2"
		shift
		;;
	-i | --interval)
		INTERVAL="$2"
		shift
		;;
	-D | --deep) DEEP=1 ;;
	-L | --label)
		CASE_LABEL="$2"
		shift
		;;
	-R | --requested-fps)
		REQUESTED_FPS="$2"
		shift
		;;
	-h | --help)
		sed -n '2,60p' "$0" | sed 's/^# \{0,1\}//'
		exit 0
		;;
	*)
		echo "알 수 없는 옵션: $1" >&2
		exit 1
		;;
	esac
	shift
done

case "$CHANNEL" in auto | ch01 | ch23 | both) ;; *)
	echo "채널은 ch01 | ch23 | both | auto 중 하나여야 합니다: $CHANNEL" >&2
	exit 1
	;;
esac

case "$DURATION:$INTERVAL:$REQUESTED_FPS" in
*[^0-9:]*)
	echo "duration, interval, requested-fps는 음이 아닌 정수여야 합니다." >&2
	exit 1
	;;
esac
[ "$INTERVAL" -gt 0 ] || {
	echo "interval은 1 이상이어야 합니다." >&2
	exit 1
}
case "$CASE_LABEL" in
*[!A-Za-z0-9._-]* | '')
	echo "label은 영문자, 숫자, '.', '_', '-'만 사용할 수 있습니다: $CASE_LABEL" >&2
	exit 1
	;;
esac

# ch01 = i2c2(adapter 2), video4, mxc_isi.1 / mxc-mipi-csi2.1, DMA 채널 0
# ch23 = i2c1(adapter 1), video3, mxc_isi.0 / mxc-mipi-csi2.0, DMA 채널 2
bus_of() { case "$1" in ch01) echo 2 ;; ch23) echo 1 ;; esac }
csi_of() { case "$1" in ch01) echo 32e50000.csi ;; ch23) echo 32e40000.csi ;; esac }
isi_of() { case "$1" in ch01) echo 32e02000.isi ;; ch23) echo 32e00000.isi ;; esac }
node_of() { case "$1" in ch01) echo /dev/video4 ;; ch23) echo /dev/video3 ;; esac }
sub_of() { case "$1" in ch01) echo "max9296 2-0048" ;; ch23) echo "max9296 1-0048" ;; esac }
# 듀얼일 때 두 카메라의 이름
cam_lo() { case "$1" in ch01) echo ch0 ;; ch23) echo ch2 ;; esac }
cam_hi() { case "$1" in ch01) echo ch1 ;; ch23) echo ch3 ;; esac }
# --deep 용 DMA 채널 번호
dma_lo() { case "$1" in ch01) echo 0 ;; ch23) echo 2 ;; esac }
dma_hi() { case "$1" in ch01) echo 1 ;; ch23) echo 3 ;; esac }

NCPU=${CAM_FPS_STACK_NCPU:-$(nproc 2>/dev/null || echo 4)}
irq_of() {
	awk -v d="$1" -v n="$NCPU" '$NF==d { s=0; for (i=2; i<=n+1; i++) s+=$i; print s+0; exit }' "$IRQ_FILE"
}
now_ns() { date +%s%N; }

ap_rd() { # $1=bus $2=addr $3=hi $4=lo $5=len
	i2ctransfer -f -y -a "$1" "w2@$2" "$3" "$4" "r$5" 2>/dev/null | sed 's/0x//g' | tr -d ' '
}
hex2dec() { [ -n "${1:-}" ] && printf '%d' "$((16#$1))" || echo ""; }
hex16_fmt() { [ -n "${1:-}" ] && printf '0x%04x' "$((16#$1))" || echo "?"; }
fixed8_fmt() { [ -n "${1:-}" ] && awk -v raw="$((16#$1))" 'BEGIN{printf "%.3f", raw/256.0}' || echo "?"; }

streaming() {
	local a b
	a=$(irq_of "$(isi_of "$1")")
	sleep 1
	b=$(irq_of "$(isi_of "$1")")
	[ "$((b - a))" -gt 0 ]
}

fmt_of() { media-ctl -p 2>/dev/null | grep -A4 "entity.*$(sub_of "$1")" | grep -oE '[0-9]+x[0-9]+@1/[0-9]+' | head -1; }

# 듀얼 판정. 드라이버의 듀얼 모드 튜플과 같은 기준을 1차로 쓴다.
#   1280x360(채널당 640x360), 2560x720, 3840x1080
# 폭을 못 읽을 때만 0x11 응답으로 보조 판정한다. 스트리밍이 없으면 AP1302 가
# 응답하지 않아 0x11 만으로는 듀얼을 단일로 오판하기 때문이다.
is_dual() {
	local fmt
	fmt=$(fmt_of "$1")
	case "$fmt" in
	1280x360@* | 2560x720@* | 3840x1080@*) return 0 ;;
	?*) return 1 ;;
	esac
	[ -n "$(ap_rd "$(bus_of "$1")" 0x11 0x00 0x02 2)" ]
}

# --------------------------------------------------------------- 채널 결정
CHANS=""
case "$CHANNEL" in
both) CHANS="ch01 ch23" ;;
ch01 | ch23) CHANS="$CHANNEL" ;;
auto)
	for c in ch01 ch23; do streaming "$c" && CHANS="$CHANS $c"; done
	if [ -z "$CHANS" ]; then
		echo "스트리밍 중인 채널이 없습니다 (ISI 인터럽트 증가 없음)." >&2
		echo "채널을 직접 보려면 -c ch01 처럼 지정하십시오." >&2
		exit 1
	fi
	;;
esac
CHANS=$(echo "$CHANS" | xargs)

echo "gstApp: $(pgrep -a gstApp 2>/dev/null | head -1 || echo '실행 중 아님')"

# 각 디시리얼라이저의 모드와 AP1302 주소를 확정한다.
declare -A MODE ADDRS LABELS DMACH
NROWS=0
for c in $CHANS; do
	FMT=$(fmt_of "$c")
	if is_dual "$c"; then
		MODE[$c]=dual
		ADDRS[$c]="0x11 0x12"
		LABELS[$c]="$(cam_lo "$c") $(cam_hi "$c")"
		DMACH[$c]="$(dma_lo "$c") $(dma_hi "$c")"
		NROWS=$((NROWS + 2))
	else
		MODE[$c]=single
		ADDRS[$c]="0x3c"
		LABELS[$c]="$(cam_lo "$c")"
		DMACH[$c]="$(dma_lo "$c")"
		NROWS=$((NROWS + 1))
	fi
	printf '%s = %s  포맷 %s  모드 %s  AP1302 [%s]\n' \
		"$c" "$(node_of "$c")" "${FMT:-?}" "${MODE[$c]}" "${ADDRS[$c]}"
done
echo "관측 ${DURATION}초 / ${INTERVAL}초 간격"

WRAP_FPS=$REQUESTED_FPS
[ "$WRAP_FPS" -gt 0 ] || WRAP_FPS=60
WRAP_LIMIT=$((INTERVAL * WRAP_FPS))
[ "$WRAP_LIMIT" -gt 240 ] && echo "경고: interval ${INTERVAL}초는 ${WRAP_FPS}fps 에서 HINF_FRAME_CNT(8비트) 가 두 번 이상 순환할 수 있습니다. interval을 줄이십시오." >&2

# ------------------------------------------------------ --deep: AR0234 직접
if [ "$DEEP" -eq 1 ]; then
	for c in $CHANS; do
		read -r -a labs <<<"${LABELS[$c]}"
		read -r -a dchs <<<"${DMACH[$c]}"
		read -r -a adrs <<<"${ADDRS[$c]}"
		i=0
		for lab in "${labs[@]}"; do
			echo
			AP_WIDTH=$(hex2dec "$(ap_rd "$(bus_of "$c")" "${adrs[$i]}" 0x20 0x00 2)")
			AP_HEIGHT=$(hex2dec "$(ap_rd "$(bus_of "$c")" "${adrs[$i]}" 0x20 0x02 2)")
			AP_ROI_X0=$(hex2dec "$(ap_rd "$(bus_of "$c")" "${adrs[$i]}" 0x20 0x04 2)")
			AP_ROI_Y0=$(hex2dec "$(ap_rd "$(bus_of "$c")" "${adrs[$i]}" 0x20 0x06 2)")
			AP_ROI_X1=$(hex2dec "$(ap_rd "$(bus_of "$c")" "${adrs[$i]}" 0x20 0x08 2)")
			AP_ROI_Y1=$(hex2dec "$(ap_rd "$(bus_of "$c")" "${adrs[$i]}" 0x20 0x0a 2)")
			AP_ASPECT_RAW=$(ap_rd "$(bus_of "$c")" "${adrs[$i]}" 0x20 0x0c 2)
			AP_SENSOR_MODE_RAW=$(ap_rd "$(bus_of "$c")" "${adrs[$i]}" 0x20 0x14 2)
			AP_LINE_TIME_RAW=$(ap_rd "$(bus_of "$c")" "${adrs[$i]}" 0x20 0x1c 2)
			AP_MAX_FPS_RAW=$(ap_rd "$(bus_of "$c")" "${adrs[$i]}" 0x20 0x20 2)
			AP_ASPECT=$(hex16_fmt "$AP_ASPECT_RAW")
			AP_SENSOR_MODE=$(hex16_fmt "$AP_SENSOR_MODE_RAW")
			AP_LINE_TIME=$(hex16_fmt "$AP_LINE_TIME_RAW")
			AP_MAX_FPS_FMT=$(fixed8_fmt "$AP_MAX_FPS_RAW")
			echo "### [$c/$lab] AP1302 preview context (${adrs[$i]})"
			printf '  WIDTH=%s HEIGHT=%s ROI=%s/%s/%s/%s ASPECT=%s SENSOR_MODE=%s LINE_TIME=%s MAX_FPS=%s\n' \
				"${AP_WIDTH:-?}" "${AP_HEIGHT:-?}" "${AP_ROI_X0:-?}" "${AP_ROI_Y0:-?}" \
				"${AP_ROI_X1:-?}" "${AP_ROI_Y1:-?}" "$AP_ASPECT" "$AP_SENSOR_MODE" \
				"$AP_LINE_TIME" "$AP_MAX_FPS_FMT"
			printf 'AP_CONTEXT case=%s channel=%s addr=%s width=%s height=%s roi_x0=%s roi_y0=%s roi_x1=%s roi_y1=%s aspect=%s sensor_mode=%s line_time=%s max_fps=%s\n' \
				"$CASE_LABEL" "$lab" "${adrs[$i]}" "${AP_WIDTH:-?}" "${AP_HEIGHT:-?}" \
				"${AP_ROI_X0:-?}" "${AP_ROI_Y0:-?}" "${AP_ROI_X1:-?}" "${AP_ROI_Y1:-?}" \
				"$AP_ASPECT" "$AP_SENSOR_MODE" "$AP_LINE_TIME" "$AP_MAX_FPS_FMT"

			echo "### [$c/$lab] AR0234 레지스터 직접 읽기 (AP1302 DMA, 채널 ${dchs[$i]})"
			if [ ! -x "$DMA_TOOL" ]; then
				echo "  $DMA_TOOL 없음 - 건너뜀"
				i=$((i + 1))
				continue
			fi
			D=${dchs[$i]}
			rd_ar() { "$DMA_TOOL" "$D" "$1" 2>/dev/null | grep -oiE 'value=0x[0-9a-f]+' | tail -1 | cut -d= -f2; }
			ID=$(rd_ar 0x3000)
			if [ -z "$ID" ]; then
				echo "  읽기 실패 - DMA 접근 불가 (미사용 채널이거나 전원 미인가)"
				i=$((i + 1))
				continue
			fi
			YS=$(rd_ar 0x3002)
			XS=$(rd_ar 0x3004)
			YE=$(rd_ar 0x3006)
			XE=$(rd_ar 0x3008)
			FLL=$(rd_ar 0x300A)
			LLP=$(rd_ar 0x300C)
			CIT=$(rd_ar 0x3012)
			READ_MODE=$(rd_ar 0x3040)
			X_ODD_INC=$(rd_ar 0x30A2)
			Y_ODD_INC=$(rd_ar 0x30A6)
			PF=$(hex2dec "$(ap_rd "$(bus_of "$c")" "${adrs[$i]}" 0x00 0x78 4)")
			printf '  CHIP_VERSION(0x3000)            = %s %s\n' "$ID" "$([ "$ID" = "0x0a56" ] && echo '(AR0234 확인)')"
			printf '  Y_ADDR_START(0x3002)             = %s\n' "${YS:-?}"
			printf '  X_ADDR_START(0x3004)             = %s\n' "${XS:-?}"
			printf '  Y_ADDR_END(0x3006)               = %s\n' "${YE:-?}"
			printf '  X_ADDR_END(0x3008)               = %s\n' "${XE:-?}"
			printf '  FRAME_LENGTH_LINES(0x300A)      = %s\n' "${FLL:-?}"
			printf '  LINE_LENGTH_PCK(0x300C)         = %s\n' "${LLP:-?}"
			printf '  COARSE_INTEGRATION_TIME(0x3012) = %s\n' "${CIT:-?}"
			printf '  READ_MODE(0x3040)                = %s\n' "${READ_MODE:-?}"
			printf '  X_ODD_INC(0x30A2)                = %s\n' "${X_ODD_INC:-?}"
			printf '  Y_ODD_INC(0x30A6)                = %s\n' "${Y_ODD_INC:-?}"
			printf 'AR_TIMING case=%s channel=%s x_start=%s y_start=%s x_end=%s y_end=%s frame_length=%s line_length=%s x_odd_inc=%s y_odd_inc=%s read_mode=%s exposure=%s\n' \
				"$CASE_LABEL" "$lab" "${XS:-?}" "${YS:-?}" "${XE:-?}" "${YE:-?}" \
				"${FLL:-?}" "${LLP:-?}" "${X_ODD_INC:-?}" "${Y_ODD_INC:-?}" \
				"${READ_MODE:-?}" "${CIT:-?}"
			if [ -n "$FLL" ] && [ -n "$LLP" ] && [ -n "$PF" ] && [ "$PF" -gt 0 ]; then
				awk -v f="$((FLL))" -v l="$((LLP))" -v cc="$((CIT))" -v pf="$PF" 'BEGIN{
					px = pf/65536.0*1e6
					lt = l/px*1e6
					printf "  → pixel clock %.1f MHz, 라인시간 %.2f us\n", px/1e6, lt
					printf "  → 이론 프레임레이트 = %.0f / (%d x %d) = %.2f fps\n", px, f, l, px/(f*l)
					printf "  → 노출 = %d x %.2f us = %.0f us\n", cc, lt, cc*lt
				}'
			fi
			i=$((i + 1))
		done
	done
fi

# --------------------------------------------------------------- 헤더 출력
echo
LINE='---------+-------------+----------+----------+----------+----------+-------------------------'
printf '%-8s | %-11s | %-8s | %-8s | %-8s | %-8s | %s\n' "경과" "대상" "센서" "ISP출력" "CSI2" "ISI" "구간별 증감"
echo "$LINE"

# ---------------------------------------------------------------- 샘플 루프
declare -A PH PT PC PI SUM_S SUM_P SUM_C SUM_I N N_S N_P ISI_SUSPECT ISI_RATIO
for c in $CHANS; do
	PC[$c]=$(irq_of "$(csi_of "$c")")
	PI[$c]=$(irq_of "$(isi_of "$c")")
	ISI_SUSPECT[$c]=0
	ISI_RATIO[$c]=""
	for a in ${ADDRS[$c]}; do
		k="$c/$a"
		PH[$k]=$(hex2dec "$(ap_rd "$(bus_of "$c")" "$a" 0x00 0x02 2 | cut -c1-2)")
		PT[$k]=$(now_ns)
		SUM_S[$k]=0
		SUM_P[$k]=0
		SUM_C[$k]=0
		SUM_I[$k]=0
		N[$k]=0
		N_S[$k]=0
		N_P[$k]=0
	done
done

fmt() { awk -v v="$1" 'BEGIN{ if (v<0) printf "-"; else printf "%.1f", v }'; }

T=0
while [ "$T" -lt "$DURATION" ]; do
	sleep "$INTERVAL"
	T=$((T + INTERVAL))
	for c in $CHANS; do
		BUS=$(bus_of "$c")
		# 디시리얼라이저 단위 카운터는 한 번만 읽어 두 카메라 행에 공유한다.
		CC=$(irq_of "$(csi_of "$c")")
		CI=$(irq_of "$(isi_of "$c")")
		DCRAW=$((CC - PC[$c]))
		DC=$((DCRAW / 2))
		DI=$((CI - PI[$c]))
		PC[$c]=$CC
		PI[$c]=$CI

		# ISI 는 프레임당 1회, CSI2 는 2회가 정상이므로 원시 비율은 2.0 이어야 한다.
		# (단일 1920x1080/1280x720 에서 2.000 실측 확인)
		# 부하가 높은 조합(듀얼 폭 + 60fps + 4채널 동시)에서는 ISI 인터럽트가
		# 프레임당 1.37~2.95 회로 흔들린다(실측). 정수가 아니라 보정 불가 → 못 믿는다.
		if [ "$DI" -gt 0 ]; then
			R=$(awk -v c="$DCRAW" -v i="$DI" 'BEGIN{printf "%.2f", c/i}')
			ISI_RATIO[$c]=$R
			awk -v r="$R" 'BEGIN{exit !(r < 1.6 || r > 2.4)}' && ISI_SUSPECT[$c]=1
		fi

		read -r -a labs <<<"${LABELS[$c]}"
		li=0
		for a in ${ADDRS[$c]}; do
			k="$c/$a"
			TF=$(hex2dec "$(ap_rd "$BUS" "$a" 0x00 0xfc 4)")
			CH=$(hex2dec "$(ap_rd "$BUS" "$a" 0x00 0x02 2 | cut -c1-2)")
			NT=$(now_ns)
			DT=$(awk -v x="${PT[$k]}" -v y="$NT" 'BEGIN{printf "%.4f", (y-x)/1e9}')

			if [ -n "$CH" ] && [ -n "${PH[$k]}" ]; then
				DP=$(((CH - PH[$k] + 256) % 256))
			else DP=-1; fi

			read -r S P CC2 II L <<<"$(awk -v tf="${TF:-0}" -v dp="$DP" -v dc="$DC" -v di="$DI" -v dt="$DT" \
				-v sus="${ISI_SUSPECT[$c]}" 'BEGIN{
				sen = (tf>0) ? 1e6/tf : -1
				isp = (dp>=0 && dt>0) ? dp/dt : -1
				csi = (dt>0) ? dc/dt : -1
				isi = (dt>0) ? di/dt : -1
				l = ""
				if (sen>0.1 && isp>=0)  l = l sprintf("센서→ISP %+.0f%%  ", (isp-sen)/sen*100)
				if (isp>0.1 && csi>=0)  l = l sprintf("ISP→CSI2 %+.0f%%  ", (csi-isp)/isp*100)
				if (sus)                l = l "CSI2→ISI (ISI 비신뢰)"
				else if (csi>0.1 && isi>=0) l = l sprintf("CSI2→ISI %+.0f%%", (isi-csi)/csi*100)
				if (l=="") l = "-"
				printf "%.1f %.1f %.1f %.1f %s", sen, isp, csi, isi, l
			}')"

			if [ "${TF:-0}" -gt 0 ] 2>/dev/null; then
				SUM_S[$k]=$(awk -v x="${SUM_S[$k]}" -v y="$S" 'BEGIN{print x+y}')
				N_S[$k]=$((N_S[$k] + 1))
			fi
			if [ "$DP" -ge 0 ]; then
				SUM_P[$k]=$(awk -v x="${SUM_P[$k]}" -v y="$P" 'BEGIN{print x+y}')
				N_P[$k]=$((N_P[$k] + 1))
			fi
			SUM_C[$k]=$(awk -v x="${SUM_C[$k]}" -v y="$CC2" 'BEGIN{print x+y}')
			SUM_I[$k]=$(awk -v x="${SUM_I[$k]}" -v y="$II" 'BEGIN{print x+y}')
			N[$k]=$((N[$k] + 1))

			MARK=""
			[ "${ISI_SUSPECT[$c]}" = 1 ] && MARK="*"
			printf '%-8s | %-11s | %8s | %8s | %8s | %8s | %s\n' \
				"${T}s" "${labs[$li]} ($a)" "$(fmt "$S")" "$(fmt "$P")" "$(fmt "$CC2")" "$(fmt "$II")$MARK" "$L"

			PH[$k]=$CH
			PT[$k]=$NT
			li=$((li + 1))
		done
	done
done

# ------------------------------------------------------------------- 요약
echo "$LINE"
for c in $CHANS; do
	read -r -a labs <<<"${LABELS[$c]}"
	li=0
	for a in ${ADDRS[$c]}; do
		k="$c/$a"
		n=${N[$k]}
		ns=${N_S[$k]}
		np=${N_P[$k]}
		[ "$n" -eq 0 ] && {
			li=$((li + 1))
			continue
		}
		awk -v tag="${labs[$li]} ($a)" -v n="$n" -v ns="$ns" -v np="$np" \
			-v s="${SUM_S[$k]}" -v p="${SUM_P[$k]}" \
			-v cc="${SUM_C[$k]}" -v ii="${SUM_I[$k]}" 'BEGIN{
			sensor=(ns>0) ? s/ns : -1; isp=(np>0) ? p/np : -1
			printf "%-8s | %-11s | %8.1f | %8.1f | %8.1f | %8.1f |\n", "평균", tag, sensor, isp, cc/n, ii/n
		}'
		li=$((li + 1))
	done
done

for c in $CHANS; do
	read -r -a labs <<<"${LABELS[$c]}"
	li=0
	for a in ${ADDRS[$c]}; do
		k="$c/$a"
		n=${N[$k]}
		ns=${N_S[$k]}
		np=${N_P[$k]}
		[ "$n" -eq 0 ] && {
			li=$((li + 1))
			continue
		}
		echo
		echo "### $c / ${labs[$li]} (AP1302 $a) 판정"
		awk -v n="$n" -v ns="$ns" -v np="$np" -v s="${SUM_S[$k]}" -v p="${SUM_P[$k]}" -v cc="${SUM_C[$k]}" -v ii="${SUM_I[$k]}" \
			-v sus="${ISI_SUSPECT[$c]}" -v rr="${ISI_RATIO[$c]}" 'BEGIN{
			sen=(ns>0) ? s/ns : -1; isp=(np>0) ? p/np : -1; csi=cc/n; isi=ii/n
			if (csi < 0.5 && (sus || isi < 0.5)) {
				printf "  스트리밍 없음 - 프레임이 흐르지 않습니다\n"
				if (sen > 0.5) printf "  (센서는 %.1f fps 로 돌고 있으나 ISP 출력이 없습니다)\n", sen
				exit
			}
			worst=""; drop=0
			if (sen>0.1 && isp>0.1 && (sen-isp)/sen > drop) { drop=(sen-isp)/sen; worst="센서 → ISP 출력" }
			if (isp>0.1 && csi>0.1 && (isp-csi)/isp > drop) { drop=(isp-csi)/isp; worst="ISP → CSI2 (GMSL/디시리얼라이저)" }
			if (!sus && csi>0.1 && isi>0.1 && (csi-isi)/csi > drop) { drop=(csi-isi)/csi; worst="CSI2 → ISI" }
			if (drop > 0.05) printf "  최대 낙차: %s 구간에서 %.0f%% 손실\n", worst, drop*100
			else             printf "  전 구간 손실 없음 (최대 낙차 %.1f%%, 센서~CSI2 기준)\n", drop*100
			if (sen>0.1 && isp>0.1 && isp < sen*0.9)
				printf "  → ISP 가 센서 프레임을 버리고 있습니다. 트리거 분주/스킵 정책 확인 (R0x1186, R0x6112)\n"
			if (isp>0.1 && csi>0.1 && csi < isp*0.9)
				printf "  → GMSL 링크 또는 디시리얼라이저에서 손실. MAX9296 CTRL3(0x0013) 링크 락 확인\n"
			if (sus)
				printf "  ! ISI 열 제외: csi/isi 원시 비율 %s (정상 2.0). ISI 인터럽트가 프레임당 1회가 아니라\n    이 구성에서는 ISI fps 를 신뢰할 수 없습니다. 판정은 센서~CSI2 만으로 했습니다.\n", rr
			else if (csi>0.1 && isi>0.1 && isi < csi*0.9)
				printf "  → ISI 가 못 받고 있습니다. 대역/버퍼 확인\n"
		}'
		awk -v case_name="$CASE_LABEL" -v requested="$REQUESTED_FPS" \
			-v channel="${labs[$li]}" -v n="$n" -v ns="$ns" -v np="$np" -v s="${SUM_S[$k]}" \
			-v p="${SUM_P[$k]}" -v cc="${SUM_C[$k]}" -v ii="${SUM_I[$k]}" \
			-v suspect="${ISI_SUSPECT[$c]}" 'BEGIN{
			sensor=(ns>0) ? s/ns : -1; isp=(np>0) ? p/np : -1; csi=cc/n; isi=ii/n; loss=0
			if (sensor>0 && isp>=0 && (sensor-isp)/sensor > loss) loss=(sensor-isp)/sensor
			if (isp>0 && csi>=0 && (isp-csi)/isp > loss) loss=(isp-csi)/isp
			if (!suspect && csi>0 && isi>=0 && (csi-isi)/csi > loss) loss=(csi-isi)/csi
			trust=suspect ? 0 : 1
			sensor_valid=(n>0 && ns==n) ? 1 : 0
			isp_valid=(n>0 && np==n) ? 1 : 0
			pass=(requested==120 && sensor_valid && isp_valid && sensor>=118.8 && isp>=118.8 && csi>=118.8 && trust && isi>=118.8 && loss*100<=1.0) ? 1 : 0
			printf "FPS_RESULT case=%s requested=%d channel=%s sensor=%.1f isp=%.1f csi=%.1f isi=%.1f loss_pct=%.1f isi_trust=%d sensor_valid=%d isp_valid=%d pass120=%d\n", case_name, requested, channel, sensor, isp, csi, isi, loss*100, trust, sensor_valid, isp_valid, pass
		}'
		li=$((li + 1))
	done
done

echo
echo "센서=AP1302 R0x00FC 역수 / ISP=R0x0002 HINF_FRAME_CNT 델타 / CSI2=csi IRQ÷2 / ISI=isi IRQ"
echo "레이트는 sleep 값이 아니라 실제 경과시간으로 나눈 값입니다."
for c in $CHANS; do
	[ "${MODE[$c]}" = dual ] && echo "$c 는 듀얼 모드입니다. 두 카메라 영상이 하나의 넓은 프레임으로 합쳐지므로 CSI2/ISI 열은 두 행이 같은 값을 공유합니다."
done
for c in $CHANS; do
	[ "${ISI_SUSPECT[$c]}" = 1 ] && cat <<EOF

! $c : ISI 열(*) 신뢰 불가 — csi/isi 원시 비율 ${ISI_RATIO[$c]} (정상 2.0)
  ISI 는 프레임당 인터럽트 1회가 정상이나 이 구성에서는 그보다 많이 올린다.
  실측: 단일 1920x1080/1280x720 @30 → 비율 2.000 (프레임당 1회, 정확)
        4채널 듀얼 2560x720 @60     → 프레임 59 fps 로 일정한데
                                       ISI IRQ/frame 이 1.37~2.95 로 흔들림
  정수도 고정도 아니라 나눠서 보정할 수 없다. CSI2 열(csi IRQ÷2)은 같은 구간에서
  59 fps 로 일정하므로, 전달 레이트는 ISI 대신 CSI2 열을 보면 된다.
  초과분은 정상 프레임 완료가 아닌 다른 이벤트로 보이나 원인 미확인이며,
  프레임 수 자체는 안정적이므로 영상 손실은 아니다.
EOF
done

exit 0
