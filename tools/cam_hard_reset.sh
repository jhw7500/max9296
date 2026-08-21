#!/usr/bin/env bash
#
# cam_hard_reset.sh - 재부팅 없이 카메라 파이프라인 전체를 초기화한다.
#
# 왜 모듈 리셋으로 부족한가:
#   cam_disable.sh 가 하는 `rmmod/modprobe max9296 + imx8-media-dev` 는 i2c
#   드라이버와 미디어 디바이스만 되돌린다. SoC 쪽 MIPI CSI-2 수신부
#   (mxc-mipi-csi2-sam) 와 ISI 는 커널에 빌트인이라 모듈 리셋의 영향을 전혀
#   받지 않는다. 이 둘이 한 번 물리면 STREAMON 은 성공하는데 CSI2 이벤트
#   카운터가 전부 0 인 - 즉 D-PHY 가 락을 못 잡은 - 상태로 남는다.
#
#   실측(2026-08-11, pim-camera-v016):
#     max9296 + imx8-media-dev 완전 리로드      -> 복구 실패
#     32e00000.isi 만 unbind/bind               -> 복구 실패
#     32e40000.csi 만 unbind/bind               -> 복구 성공 (28.91 fps)
#
#   그래서 이 스크립트는 CSI2 까지 내렸다 올린다.
#
# 사용법:
#   cam_hard_reset.sh [옵션]
#     -s, --stop-service   cam-operate.service 를 정지시킨 뒤 리셋 (기본: 정지 안 함)
#     -S, --start-service  리셋 후 cam-operate.service 를 기동
#     -q, --quiet          단계별 로그 생략, 결과만 출력
#     -h, --help
#
# 종료코드: 0 = 정상 (video 노드 재생성 확인), 1 = 실패, 2 = 복구 불가(모듈 refcnt 음수 — 재부팅 필요)

set -u

CSI_DRV=/sys/bus/platform/drivers/mxc-mipi-csi2-sam
ISI_DRV=/sys/bus/platform/drivers/mxc-isi
CAP_DRV=/sys/bus/platform/drivers/isi-capture
M2M_DRV=/sys/bus/platform/drivers/isi-m2m

CSI_DEVS="32e40000.csi 32e50000.csi"
ISI_DEVS="32e00000.isi 32e02000.isi"
CAP_DEVS="32e00000.isi:cap_device 32e02000.isi:cap_device"
M2M_DEVS="32e00000.isi:m2m_device"

STOP_SVC=0
START_SVC=0
QUIET=0
SKIP_UNBIND=0

while [ $# -gt 0 ]; do
	case "$1" in
	-s | --stop-service) STOP_SVC=1 ;;
	-S | --start-service) START_SVC=1 ;;
	-q | --quiet) QUIET=1 ;;
	-h | --help)
		sed -n '2,28p' "$0" | sed 's/^# \{0,1\}//'
		exit 0
		;;
	*)
		echo "알 수 없는 옵션: $1" >&2
		exit 1
		;;
	esac
	shift
done

log() { [ "$QUIET" -eq 1 ] || echo "$@"; }

# sysfs bind/unbind 는 "이미 그 상태" 일 때 EINVAL/ENODEV 를 낸다. 정상이므로
# 실패를 삼키되 무엇이 일어났는지는 남긴다.
sysfs_write() { # $1=파일 $2=값 $3=라벨
	if [ ! -w "$1" ]; then
		log "    - $3: $1 없음 (건너뜀)"
		return 0
	fi
	if echo "$2" >"$1" 2>/dev/null; then
		log "    - $3: $2 OK"
	else
		log "    - $3: $2 (이미 해당 상태 / 대상 없음)"
	fi
}

is_bound() { # $1=드라이버경로 $2=디바이스명
	[ -e "$1/$2" ]
}

# ---------------------------------------------------------------- 0) 앱 정지
if [ "$STOP_SVC" -eq 1 ]; then
	log "[0] cam-operate 정지"
	systemctl stop cam-operate.service 2>/dev/null
	sleep 2
fi
if pgrep gstApp >/dev/null 2>&1; then
	log "[0] gstApp 정지"
	pkill -9 gstApp 2>/dev/null
	sleep 2
fi

# --------------------------------------------------------------- 1) 모듈 해제
#
# rmmod 실패를 무시하고 진행하면 안 된다. 사용 중인 모듈을 강제로 내리려다
# 실패하면 참조 카운트가 -1 로 남고, 그 상태의 모듈은 제거도 재사용도 안 되며
# /dev/videoN 이 사라진 채 재부팅 외에는 복구되지 않는다. (2026-08-11 실측)
log "[1] 모듈 해제"
for m in imx8-media-dev max9296; do
	mod=$(echo "$m" | tr '-' '_')
	lsmod | grep -qE "^$mod " || {
		log "    - $m: 이미 미적재"
		continue
	}
	if rmmod "$m" 2>/dev/null; then
		log "    - $m rmmod OK"
	else
		log "    ! $m rmmod 실패 (사용 중: $(lsmod | awk -v M="$mod" '$1==M{print $3}'))"
	fi
	sleep 1
done

# 참조 카운트가 음수면 커널이 모듈을 정리 중 실패한 것 - 여기서 멈춘다.
STUCK=$(lsmod | awk '$1=="max9296" || $1=="imx8_media_dev" { if ($3+0 < 0) print $1"(refcnt="$3")" }')
if [ -n "$STUCK" ]; then
	echo "치명적: 모듈이 복구 불가 상태입니다 - $STUCK" >&2
	echo "        참조 카운트가 음수인 모듈은 재적재도 제거도 되지 않습니다." >&2
	echo "        재부팅이 필요합니다: reboot" >&2
	exit 2
fi
if lsmod | grep -qE '^(max9296|imx8_media_dev) '; then
	log "    ! 모듈이 남아있음: $(lsmod | grep -E '^(max9296|imx8_media_dev) ' | awk '{print $1"(used="$3")"}' | tr '\n' ' ')"
	log "      → SoC 드라이버 unbind 를 건너뜁니다 (미디어 디바이스가 참조 중)"
	SKIP_UNBIND=1
fi

# ------------------------------------------------------- 2) SoC 드라이버 unbind
# 부모(mxc-isi)를 먼저 내리면 자식 플랫폼 디바이스가 사라지므로 자식부터 내린다.
#
# 모듈이 안 내려간 상태에서 unbind 하면 미디어 디바이스가 살아있는 subdev 를
# 참조한 채 남아 커널이 불안정해진다. 그런 경우 아예 시도하지 않는다.
if [ "$SKIP_UNBIND" -eq 1 ]; then
	echo "모듈을 내리지 못해 SoC 드라이버 재바인드를 건너뜁니다." >&2
	echo "사용 중인 프로세스를 정지한 뒤 다시 실행하거나, 재부팅하십시오." >&2
	exit 1
fi
log "[2] SoC 드라이버 unbind (capture -> m2m -> isi -> csi2)"
for d in $CAP_DEVS; do sysfs_write "$CAP_DRV/unbind" "$d" "isi-capture"; done
sleep 1
for d in $M2M_DEVS; do sysfs_write "$M2M_DRV/unbind" "$d" "isi-m2m"; done
sleep 1
for d in $ISI_DEVS; do sysfs_write "$ISI_DRV/unbind" "$d" "mxc-isi"; done
sleep 1
for d in $CSI_DEVS; do sysfs_write "$CSI_DRV/unbind" "$d" "mxc-mipi-csi2-sam"; done
sleep 2

# --------------------------------------------------------- 3) SoC 드라이버 bind
# csi2 를 먼저 올린다. mxc-isi 의 probe 가 cap/m2m 자식 디바이스를 다시 만들고
# 커널이 자동으로 바인드하므로, 자식은 여전히 unbound 일 때만 손으로 붙인다.
log "[3] SoC 드라이버 bind (csi2 -> isi -> 자식 자동)"
for d in $CSI_DEVS; do sysfs_write "$CSI_DRV/bind" "$d" "mxc-mipi-csi2-sam"; done
sleep 1
for d in $ISI_DEVS; do sysfs_write "$ISI_DRV/bind" "$d" "mxc-isi"; done
sleep 2
for d in $CAP_DEVS; do
	is_bound "$CAP_DRV" "$d" || sysfs_write "$CAP_DRV/bind" "$d" "isi-capture(수동)"
done
for d in $M2M_DEVS; do
	is_bound "$M2M_DRV" "$d" || sysfs_write "$M2M_DRV/bind" "$d" "isi-m2m(수동)"
done
sleep 1

# --------------------------------------------------------------- 4) 모듈 재적재
log "[4] 모듈 재적재"
modprobe max9296 2>&1 | sed 's/^/    /'
sleep 3
modprobe imx8-media-dev 2>&1 | sed 's/^/    /'
sleep 5

# -------------------------------------------------------------------- 5) 검증
log "[5] 검증"
BOUND_OK=1
for d in $CSI_DEVS; do
	is_bound "$CSI_DRV" "$d" || {
		BOUND_OK=0
		log "    ! $d unbound"
	}
done
for d in $ISI_DEVS; do
	is_bound "$ISI_DRV" "$d" || {
		BOUND_OK=0
		log "    ! $d unbound"
	}
done

NODES=""
for i in 0 1 2 3 4 5; do
	[ -e "/dev/video$i" ] && NODES="$NODES video$i"
done
log "    video 노드:$NODES"
log "    모듈: $(lsmod | grep -E '^(max9296|imx8_media_dev) ' | awk '{print $1}' | tr '\n' ' ')"

if [ "$START_SVC" -eq 1 ]; then
	log "[6] cam-operate 기동"
	systemctl start cam-operate.service
	sleep 8
	log "    cam-operate: $(systemctl is-active cam-operate.service), gstApp: $(pgrep gstApp || echo 없음)"
fi

# video3/video4 가 다시 생겼고 드라이버가 붙어 있으면 성공으로 본다.
if [ "$BOUND_OK" -eq 1 ] && [ -e /dev/video3 ] && [ -e /dev/video4 ]; then
	echo "하드 리셋 완료 (CSI2 + ISI 재바인드 포함)"
	exit 0
fi
echo "하드 리셋 실패 - 위 로그 확인 필요" >&2
exit 1
