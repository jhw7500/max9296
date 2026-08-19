#!/usr/bin/env bash
#
# cam_prepare_gate.sh - MAX9296 병렬 prepare 보드 게이트 실측
#
# docs/superpowers/plans/2026-08-13-max9296-parallel-prepare.md 의 Task 5 Step 4
# 가 요구하는 네 가지를 실측한다. 전부 통과해야 이 경로를 기동 스크립트에
# 활성화할 수 있다.
#
#   G1  두 CSI 도메인의 prepare 구간이 실제로 겹치는가 (직렬화되지 않는가)
#   G2  GStreamer STREAMON 에서 2차 펌웨어 다운로드가 없는가
#   G3  expiry / cancel / 잘못된 요청의 정리가 계약대로인가
#   G4  single / dual 반복 사이클 soak (기본 100회)
#
# 계약 근거는 docs/parallel-prepare-v1.md 이다.
#
# 주의: 이 시험은 파괴적이다. cam-operate.service 를 내리고 하드 리셋을 건다.
#       종료 시 trap 이 서비스를 원복한다.
#
# 사용법:
#   cam_prepare_gate.sh [옵션]
#     -c, --cycles N     G4 사이클 수 (기본 100)
#     -g, --gates LIST   실행할 게이트, 쉼표 구분 (기본 G1,G2,G3,G4)
#     -k, --keep-service 종료 시 cam-operate.service 를 다시 켜지 않는다
#     -h, --help
#
# 종료코드: 0 = 전 게이트 PASS, 1 = 하나라도 FAIL, 2 = 사전조건 미충족

set -u

# CSI 도메인 <-> i2c 어댑터 <-> video 노드 매핑.
# imx8mp.dtsi 가 i2c1=&i2c2, i2c2=&i2c3 로 선언하므로 어댑터 번호는
# device-tree 노드명보다 1 작다. docs/parallel-prepare-v1.md 의 표와 동일하다.
PREP_A=/sys/bus/i2c/devices/1-0048/prepare   # max9296_0, ch2/ch3, /dev/video3
PREP_B=/sys/bus/i2c/devices/2-0048/prepare   # max9296_1, ch0/ch1, /dev/video4
VID_A=/dev/video3
VID_B=/dev/video4

# 게이트에서 쓰는 하드웨어 튜플. docs/parallel-prepare-v1.md 의 지원 목록에서 고른다.
DUAL_W=3840
DUAL_H=1080
DUAL_EN=3
SINGLE_W=1920
SINGLE_H=1080
SINGLE_EN=1
FPS=30
# ISI capture 노드는 UYVY 를 받지 않는다. cam_fps_probe.sh 와 같은 RGB565 를 쓴다.
PIXFMT=RGBP

# 펌웨어 구간 마커. max9296.c 가 다운로드의 시작과 끝을 모두 찍는다.
#   [   20.672225] [I2C:2][max9296.c:3513] start_fw_load
#   [   24.359943] [I2C:2][max9296.c:3537] end_fw_load
# 커널 타임스탬프라 사용자 공간 시계보다 정확하고, 인스턴스별로 태그가 붙어
# 두 도메인의 구간 겹침을 직접 계산할 수 있다.
FW_MARK='start_fw_load|end_fw_load'

RESET=/root/camtest/cam_hard_reset.sh
[ -x "$RESET" ] || RESET="$(dirname "$0")/cam_hard_reset.sh"

CYCLES=100
GATES=G1,G2,G3,G4
KEEP_SERVICE=0
WORK=/tmp/prepare_gate.$$
FAILED=0

while [ $# -gt 0 ]; do
	case "$1" in
	-c | --cycles)
		CYCLES="$2"
		shift 2
		;;
	-g | --gates)
		GATES="$2"
		shift 2
		;;
	-k | --keep-service)
		KEEP_SERVICE=1
		shift
		;;
	-h | --help)
		sed -n '2,30p' "$0"
		exit 0
		;;
	*)
		echo "알 수 없는 옵션: $1" >&2
		exit 2
		;;
	esac
done

mkdir -p "$WORK"

cleanup() {
	rm -rf "$WORK"
	if [ "$KEEP_SERVICE" -eq 0 ]; then
		echo "[정리] cam-operate.service 재기동"
		systemctl start cam-operate.service 2>/dev/null
	fi
}
trap cleanup EXIT

say() { printf '%s\n' "$*"; }
hr() { printf '%s\n' "------------------------------------------------------------"; }

pass() {
	say "  PASS  $*"
}

fail() {
	say "  FAIL  $*"
	FAILED=1
}

wants() { case ",$GATES," in *",$1,"*) return 0 ;; *) return 1 ;; esac; }

# prepare 상태줄에서 키 하나를 뽑는다. 없으면 빈 문자열.
stat_of() {
	local node="$1" key="$2"
	tr ' ' '\n' <"$node" 2>/dev/null | sed -n "s/^${key}=//p" | head -1
}

# mark(uptime 초) 이후 커널이 시작한 펌웨어 다운로드 건수.
# 이 드라이버는 모든 i2c write 를 KERN_NOTICE 로 찍어 링버퍼가 빨리 감긴다.
# 전체 버퍼를 세면 이전 구간이 잘려나가 음수 델타가 나오므로 반드시 창으로 센다.
# 버퍼가 이미 mark 이전을 잃었으면 -1 을 돌려 호출자가 판정을 보류하게 한다.
fw_count_since() {
	dmesg | awk -F'[][]' -v mark="$1" '
		NR == 1 { first = $2 + 0 }
		($2 + 0) > mark && /start_fw_load/ { c++ }
		END { if (first > mark) print -1; else print c + 0 }'
}

# 현재 uptime(초). dmesg 타임스탬프와 같은 시간축이다.
now_uptime() { awk '{print $1}' /proc/uptime; }

# since 이후 구간에서 인스턴스별 펌웨어 구간을 뽑아 겹침을 계산한다.
# 출력: "<instA> <durA_ms> <instB> <durB_ms> <overlap_ms> <span_ms>"
# 인스턴스가 2개 미만이면 "insufficient <n>".
fw_overlap_since() {
	dmesg | grep -E "$FW_MARK" | awk -F'[][]' -v since="$1" '
		{
			ts = $2 + 0
			if (ts <= since) next
			inst = $4
			mark = $7
			gsub(/ /, "", mark)
			if (mark == "start_fw_load") {
				st[inst] = ts
				if (!(inst in seen)) { seen[inst] = 1; order[++n] = inst }
			} else if (mark == "end_fw_load") {
				en[inst] = ts
			}
		}
		END {
			if (n < 2) { printf "insufficient %d\n", n; exit }
			a = order[1]; b = order[2]
			if (!(a in en) || !(b in en)) { printf "incomplete\n"; exit }
			lo = (st[a] > st[b]) ? st[a] : st[b]
			hi = (en[a] < en[b]) ? en[a] : en[b]
			first = (st[a] < st[b]) ? st[a] : st[b]
			last  = (en[a] > en[b]) ? en[a] : en[b]
			printf "%s %.1f %s %.1f %.1f %.1f\n", \
				a, (en[a]-st[a])*1000, b, (en[b]-st[b])*1000, (hi-lo)*1000, (last-first)*1000
		}'
}

# 비어 있지 않은 generation 을 만든다. 0 은 계약상 거부된다.
gen_new() { date +%s%N; }

# 한 도메인에 prepare 를 쓴다. 시작/종료 시각(ns)과 종료코드를 파일로 남긴다.
# 블로킹 write 이므로 이 구간이 곧 그 도메인의 prepare 구간이다.
prepare_bg() {
	local node="$1" gen="$2" w="$3" h="$4" en="$5" out="$6"
	(
		start=$(date +%s%N)
		printf '1 %s %s %s %s %s\n' "$gen" "$w" "$h" "$FPS" "$en" >"$node" 2>"$out.err"
		rc=$?
		end=$(date +%s%N)
		printf '%s %s %s\n' "$start" "$end" "$rc" >"$out"
	) &
}

# 하드 리셋으로 cold 상태를 만든다. 레거시 앱이 남긴 power_count 잔류가
# prepare 를 영구 EBUSY 로 막기 때문에 매 게이트 앞에 필요하다.
go_cold() {
	systemctl stop cam-operate.service 2>/dev/null
	pkill -9 gstApp 2>/dev/null
	"$RESET" -q -s >/dev/null 2>&1 || {
		fail "하드 리셋 실패 - cold 상태를 만들 수 없다"
		return 1
	}
	local st
	st=$(stat_of "$PREP_A" state)
	[ "$st" = "IDLE" ] || {
		fail "cold 확보 실패: $PREP_A state=$st (IDLE 이어야 한다)"
		return 1
	}
	return 0
}

# ---------------------------------------------------------------- 사전조건
say "MAX9296 병렬 prepare 보드 게이트"
hr
for n in "$PREP_A" "$PREP_B"; do
	[ -e "$n" ] || {
		say "prepare ABI 없음: $n"
		say "이 커널 모듈에는 병렬 prepare 가 없다. 배포본을 확인하라."
		exit 2
	}
done
say "드라이버: $(cat /sys/module/max9296/srcversion 2>/dev/null) / $(cat /sys/module/max9296/version 2>/dev/null)"
say "게이트: $GATES   soak 사이클: $CYCLES"
hr

# ---------------------------------------------------------------- G1 겹침
if wants G1; then
	say "[G1] 두 CSI 도메인 prepare 구간 겹침"
	if go_cold; then
		mark=$(now_uptime)
		gen=$(gen_new)
		prepare_bg "$PREP_A" "$gen" "$DUAL_W" "$DUAL_H" "$DUAL_EN" "$WORK/a"
		prepare_bg "$PREP_B" "$gen" "$DUAL_W" "$DUAL_H" "$DUAL_EN" "$WORK/b"
		wait
		fw1=$(fw_count_since "$mark")

		read -r a_start a_end a_rc <"$WORK/a"
		read -r b_start b_end b_rc <"$WORK/b"

		if [ "$a_rc" -ne 0 ] || [ "$b_rc" -ne 0 ]; then
			fail "prepare write 실패 rc=$a_rc,$b_rc  $(cat "$WORK"/*.err 2>/dev/null)"
		else
			# 두 구간의 교집합. 양수면 실제로 병렬로 돌았다는 뜻이다.
			# 사용자 공간에서 본 블로킹 write 구간 (참고값)
			dur_a=$(awk -v s="$a_start" -v e="$a_end" 'BEGIN { printf "%.1f", (e - s) / 1000000 }')
			dur_b=$(awk -v s="$b_start" -v e="$b_end" 'BEGIN { printf "%.1f", (e - s) / 1000000 }')
			say "  write 구간  A(1-0048) ${dur_a}ms   B(2-0048) ${dur_b}ms   펌웨어 로드 ${fw1}건"

			# 판정은 커널이 찍은 펌웨어 구간으로 한다. 인스턴스 태그가 붙어 있고
			# 같은 시간축이라 두 도메인의 겹침을 직접 계산할 수 있다.
			read -r k_a k_adur k_b k_bdur k_over k_span <<<"$(fw_overlap_since "$mark")"
			case "$k_a" in
			insufficient | incomplete)
				fail "펌웨어 구간을 두 인스턴스에서 모두 얻지 못했다 ($k_a ${k_adur:-})"
				;;
			*)
				say "  펌웨어 구간  $k_a ${k_adur}ms   $k_b ${k_bdur}ms   겹침 ${k_over}ms   전체 ${k_span}ms"
				if awk -v o="$k_over" 'BEGIN { exit !(o > 0) }'; then
					serial=$(awk -v a="$k_adur" -v b="$k_bdur" 'BEGIN { printf "%.1f", a + b }')
					pass "펌웨어 구간이 ${k_over}ms 겹쳤다 (직렬 합 ${serial}ms -> 실제 ${k_span}ms)"
				else
					fail "펌웨어 구간이 겹치지 않았다 - 여전히 직렬화되고 있다"
				fi
				;;
			esac
			for n in "$PREP_A" "$PREP_B"; do
				s=$(stat_of "$n" state)
				l=$(stat_of "$n" lease)
				m=$(stat_of "$n" match)
				[ "$s" = "READY" ] && [ "$l" = "1" ] && [ "$m" = "1" ] &&
					pass "$n state=READY lease=1 match=1" ||
					fail "$n state=$s lease=$l match=$m"
			done
			# 두 도메인이 같은 전원 epoch 위에 있어야 한다.
			ea=$(stat_of "$PREP_A" epoch)
			eb=$(stat_of "$PREP_B" epoch)
			[ -n "$ea" ] && [ "$ea" = "$eb" ] &&
				pass "동일 hardware epoch=$ea" ||
				fail "epoch 불일치 A=$ea B=$eb"
		fi
	fi
	hr
fi

# ---------------------------------------------------------------- G2 2차 다운로드
if wants G2; then
	say "[G2] STREAMON 에서 2차 펌웨어 다운로드 없음"
	# G1 이 남긴 READY lease 를 그대로 소비한다. G1 을 건너뛴 경우 여기서 준비한다.
	if [ "$(stat_of "$PREP_A" state)" != "READY" ]; then
		if go_cold; then
			gen=$(gen_new)
			prepare_bg "$PREP_A" "$gen" "$DUAL_W" "$DUAL_H" "$DUAL_EN" "$WORK/a"
			prepare_bg "$PREP_B" "$gen" "$DUAL_W" "$DUAL_H" "$DUAL_EN" "$WORK/b"
			wait
		fi
	fi
	mark=$(now_uptime)
	for v in "$VID_A" "$VID_B"; do
		timeout 30 v4l2-ctl -d "$v" \
			--set-fmt-video=width=${DUAL_W},height=${DUAL_H},pixelformat=$PIXFMT \
			--stream-mmap --stream-count=30 >"$WORK/streamon.$(basename "$v")" 2>&1 &
	done
	wait
	delta=$(fw_count_since "$mark")
	if [ "$delta" -eq 0 ]; then
		pass "STREAMON 구간 펌웨어 다운로드 0건 (준비된 하드웨어 재사용)"
	else
		fail "STREAMON 이 펌웨어를 ${delta}건 다시 내려받았다"
	fi
	for n in "$PREP_A" "$PREP_B"; do
		l=$(stat_of "$n" lease)
		[ "$l" = "0" ] &&
			pass "$n lease 가 V4L2 로 이양됨 (lease=0)" ||
			fail "$n lease=$l - 첫 power-on 이 lease 를 소비하지 않았다"
	done
	hr
fi

# ---------------------------------------------------------------- G3 정리 계약
if wants G3; then
	say "[G3] cancel / expiry / 잘못된 요청 정리"

	# G3-a cancel
	if go_cold; then
		gen=$(gen_new)
		printf '1 %s %s %s %s %s\n' "$gen" "$SINGLE_W" "$SINGLE_H" "$FPS" "$SINGLE_EN" >"$PREP_A"
		[ "$(stat_of "$PREP_A" state)" = "READY" ] ||
			fail "cancel 전 prepare 가 READY 가 아니다"
		printf '0\n' >"$PREP_A"
		s=$(stat_of "$PREP_A" state)
		l=$(stat_of "$PREP_A" lease)
		[ "$s" = "IDLE" ] && [ "$l" = "0" ] &&
			pass "cancel 후 state=IDLE lease=0" ||
			fail "cancel 후 state=$s lease=$l"
		# 취소가 전원을 남기지 않았는지: 다시 cold prepare 가 되면 정상이다.
		printf '1 %s %s %s %s %s\n' "$(gen_new)" "$SINGLE_W" "$SINGLE_H" "$FPS" "$SINGLE_EN" >"$PREP_A" 2>"$WORK/recancel.err"
		if [ "$(stat_of "$PREP_A" state)" = "READY" ]; then
			pass "cancel 후 재 prepare 성공 (전원 누수 없음)"
			printf '0\n' >"$PREP_A"
		else
			fail "cancel 후 재 prepare 실패: $(cat "$WORK/recancel.err")"
		fi
	fi

	# G3-b expiry: 미사용 lease 는 60초 후 만료된다.
	if go_cold; then
		printf '1 %s %s %s %s %s\n' "$(gen_new)" "$SINGLE_W" "$SINGLE_H" "$FPS" "$SINGLE_EN" >"$PREP_A"
		say "  lease 만료 대기 65초..."
		sleep 65
		s=$(stat_of "$PREP_A" state)
		l=$(stat_of "$PREP_A" lease)
		[ "$l" = "0" ] &&
			pass "미사용 lease 만료됨 state=$s lease=0" ||
			fail "60초 후에도 lease=$l (state=$s)"
	fi

	# G3-c 잘못된 요청은 거부되고 기존 상태를 훼손하지 않는다.
	if go_cold; then
		before=$(cat "$PREP_A")
		bad=0
		# generation 0 / fps 범위 밖 / 지원하지 않는 해상도 / enable 마스크 오류
		for cmd in "1 0 $SINGLE_W $SINGLE_H $FPS $SINGLE_EN" \
			"1 $(gen_new) $SINGLE_W $SINGLE_H 0 $SINGLE_EN" \
			"1 $(gen_new) $SINGLE_W $SINGLE_H 121 $SINGLE_EN" \
			"1 $(gen_new) 1234 567 $FPS $SINGLE_EN" \
			"1 $(gen_new) $SINGLE_W $SINGLE_H $FPS 7"; do
			if printf '%s\n' "$cmd" >"$PREP_A" 2>/dev/null; then
				fail "거부되어야 할 요청이 수락됨: $cmd"
				bad=1
			fi
		done
		[ "$bad" -eq 0 ] && pass "잘못된 요청 5종 전부 거부됨"
		after=$(cat "$PREP_A")
		[ "$before" = "$after" ] &&
			pass "거부된 요청이 기존 상태를 바꾸지 않았다" ||
			fail "거부된 요청 후 상태가 바뀌었다"
	fi
	hr
fi

# ---------------------------------------------------------------- G4 soak
# 한 구성에 대해 cold prepare 1회 + warm 재사용 사이클 count 회를 돈다.
# warm 구간에서는 prepare write 를 하지 않는다. 이 BSP 는 V4L2 close 에서
# s_power(0) 을 호출하지 않아 power_count 가 잔류하고, 그 상태의 prepare store
# 는 계약상 EBUSY 다. gstApp 설계도 warm 재사용을 "write 없이 상태만 확인"
# 으로 규정한다 (docs/superpowers/specs/2026-08-14-max9296-prepare-integration-design.md).
warm_soak() {
	local label=$1 w=$2 h=$3 en=$4 count=$7 want_fw=$8
	local -a nodes vids
	local i n v fw1 gen mark st l m we bad ok ng cycle_fw fw_total fw_lost
	read -ra nodes <<<"$5"
	read -ra vids <<<"$6"

	go_cold || return 1

	mark=$(now_uptime)
	gen=$(gen_new)
	for n in "${nodes[@]}"; do
		printf '1 %s %s %s %s %s\n' "$gen" "$w" "$h" "$FPS" "$en" >"$n" 2>/dev/null || {
			fail "$label cold prepare 실패 $n"
			return 1
		}
	done
	fw1=$(fw_count_since "$mark")
	if [ "$fw1" -eq "$want_fw" ]; then
		pass "$label cold prepare 펌웨어 ${want_fw}건"
	elif [ "$fw1" -lt 0 ]; then
		say "  $label cold 펌웨어 건수 판정 보류 (dmesg 링버퍼가 감김)"
	else
		fail "$label cold prepare 펌웨어 ${fw1}건 (기대 ${want_fw}건)"
	fi

	ok=0
	ng=0
	fw_total=0
	fw_lost=0
	mark=$(now_uptime)
	for i in $(seq 1 "$count"); do
		for v in "${vids[@]}"; do
			timeout 25 v4l2-ctl -d "$v" \
				--set-fmt-video=width="$w",height="$h",pixelformat="$PIXFMT" \
				--stream-mmap --stream-count=10 >/dev/null 2>&1 &
		done
		wait

		# 사이클마다 짧은 창으로 세어 누적한다. 이 드라이버는 모든 i2c write 를
		# KERN_NOTICE 로 찍어 링버퍼가 빨리 감기므로, soak 전체를 창 하나로 덮으면
		# 시작점이 사라져 판정 자체가 불가능해진다(50 사이클에서 실측).
		cycle_fw=$(fw_count_since "$mark")
		if [ "$cycle_fw" -lt 0 ]; then
			fw_lost=$((fw_lost + 1))
		else
			fw_total=$((fw_total + cycle_fw))
		fi
		mark=$(now_uptime)

		bad=0
		for n in "${nodes[@]}"; do
			st=$(stat_of "$n" state)
			l=$(stat_of "$n" lease)
			m=$(stat_of "$n" match)
			we=$(stat_of "$n" worker_errno)
			if [ "$st" != "CONSUMED" ] || [ "$l" != "0" ] || [ "$m" != "1" ] || [ "$we" != "0" ]; then
				bad=1
				say "  $label 사이클 $i: $n state=$st lease=$l match=$m worker_errno=$we"
			fi
		done
		if [ "$bad" -eq 0 ]; then ok=$((ok + 1)); else ng=$((ng + 1)); fi
		[ $((i % 10)) -eq 0 ] && say "  $label ${i}/${count}  성공 $ok  실패 $ng"
	done
	if [ "$fw_lost" -gt 0 ]; then
		fail "$label warm 펌웨어 건수를 ${fw_lost}개 사이클에서 세지 못했다 (링버퍼 랩)"
	elif [ "$fw_total" -eq 0 ]; then
		pass "$label warm ${count}회 - 펌웨어 재다운로드 0건"
	else
		fail "$label warm 구간에서 펌웨어를 ${fw_total}건 재다운로드했다"
	fi
	if [ "$ng" -eq 0 ]; then
		pass "$label warm ${count}회 전부 재사용 조건 유지 (CONSUMED/lease=0/match=1)"
	else
		fail "$label warm 실패 ${ng} / ${count}"
	fi
}

if wants G4; then
	half=$((CYCLES / 2))
	[ "$half" -lt 1 ] && half=1
	say "[G4] soak - dual ${half}회 + single ${half}회 (하드 리셋 2회만)"
	# 사이클마다 하드 리셋을 걸지 않는다. 이 보드는 HW 워치독이 15초인데
	# cam_hard_reset.sh 가 21.8초 걸려서 반복하면 워치독 리셋이 난다(실측:
	# 2026-08-19 사이클마다 리셋하는 초안에서 첫 사이클에 보드가 재부팅).
	# 모드 전환은 전원 epoch 를 넘어야 하므로 구성당 리셋 1회만 쓴다.
	warm_soak "dual" "$DUAL_W" "$DUAL_H" "$DUAL_EN" "$PREP_A $PREP_B" "$VID_A $VID_B" "$half" 2
	warm_soak "single" "$SINGLE_W" "$SINGLE_H" "$SINGLE_EN" "$PREP_A" "$VID_A" "$half" 1
	hr
fi

# ---------------------------------------------------------------- 결과
if [ "$FAILED" -eq 0 ]; then
	say "게이트 전체 PASS"
	exit 0
fi
say "게이트 FAIL - 위 항목 확인"
exit 1
