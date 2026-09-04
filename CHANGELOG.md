# Changelog

All notable changes to the MAX9296 driver will be documented in this file.

The format is based on [Keep a Changelog](https://keepachangelog.com/en/1.0.0/),
and this project adheres to [Semantic Versioning](https://semver.org/spec/v2.0.0.html).

## [Unreleased]

드라이버 버전은 2.11로 유지한다.

### Changed
- 1280x720과 2560x720의 일반 FPS 협상 상한을 30에서 60으로 올렸다
  (`MAX9296_HD_MAX_FPS`, `max9296_360p_policy.h`). `docs/fps-limit-analysis.md`가
  1280x720 @60 요청에서 54.0~55.5 FPS를 펌웨어·드라이버·DTS 변경 없이 실측했고,
  AP1302 펌웨어의 720p 라인타임이 14.80 us(60 FPS급)이므로 기존 30은 하드웨어가
  아니라 드라이버가 만든 상한이었다. init/single/dual/right-hand 네 개 720p 모드
  테이블이 모두 이 상수를 공유한다.
- 1920x1080과 3840x1080은 30을 유지한다. 1080p 모드의 라인타임 26.27 us로는 60 FPS
  트리거 주기 안에 한 프레임을 못 읽어 AP1302가 정수 트리거 분주로 떨어지고, 요청을
  올릴수록 오히려 나빠진다(40 -> 19.9, 60 -> 19.8 FPS). 이를 여는 것은 드라이버 상수의
  문제가 아니다 — 센서 라인타임과 ISP 클럭 트리가 함께 정합된 벤더 펌웨어가 필요하다.
  blob의 `HINF_MIPI_FREQ`만 올리면 동반 PLL·분주비가 맞지 않아 ISP 출력이 0이 된다
  (`docs/fps-limit-analysis.md` §1, §4.3, §7).
- `EXP_TIME(0x500c)` 쓰기 안전 상한은 모든 모드에서 30 FPS 그대로다. 따라서 1280x720의
  31~60 FPS도 640x360의 31~120 FPS와 같이 경고 후 적용 구간이 된다.

### Unchanged
- 640x360 고속 preview 경로는 출력 해상도 640x360으로 계속 게이트된다. HD 상한을
  올려도 이 경로로 들어가지 않으며, 테스트가 이를 어서션으로 고정한다.

## [2.11] - 2026-09-01

### Changed
- 모드가 허용하는 FPS 범위에서는 `EXP_TIME(0x500c)` 수동 쓰기를 더 이상
  30 FPS 기준으로 차단하지 않는다. 31~120 FPS에서는 채널, 모드, FPS,
  요청 노출값, frame period와 기존 검증 상한을 경고로 남긴 뒤 I2C 쓰기를
  진행한다.
- 640x360@120에서 JSON 초기 수동 노출과 런타임 `exp_time`/`exp_time_chN`
  변경을 모두 허용한다. AE auto의 고속 초기 exposure seed 생략 동작은 유지한다.
- 모드 상한을 넘는 FPS, 0 FPS 또는 잘못된 검증 상한은 계속 `-EINVAL`로
  거부한다. SoC 정지 이력이 있는 수동 WB `0x510a`는 추가하지 않았다.
- 드라이버 버전을 2.10에서 2.11로 올렸다.

### Operational note
- 120 FPS의 nominal frame period는 약 8,333 us다. `exp_time=5000`처럼
  10,000 us보다 작은 값도 설정할 수 있으며, frame period 이상 값도 시험을
  막지는 않지만 `over_period=1` 경고가 남는다. 실제 영상과 전달 FPS는 보드에서
  함께 확인해야 한다.

## [2.10] - 2026-08-31

### Changed
- AR0234의 120 FPS 사양보다 낮은 compile-time 협상 제한을 두지 않도록 일반
  640x360 `MAX9296_360P_MAX_FPS` 기본값을 30에서 120으로 올렸다. 별도
  qualification 플래그 없이 single 640x360과 dual 1280x360에서 1~120 FPS를
  요청할 수 있다.
- HD/FHD의 일반 상한은 30 FPS로 유지한다. 배포 edgeconf의 초기 선택값도
  640x360@30을 유지하며, 120 FPS가 필요할 때 `.VHL_CAM.fps=120`으로 선택한다.
- 드라이버 버전을 2.9에서 2.10으로 올렸다.

### Safety
- 모든 모드의 `EXP_TIME(0x500c)` 쓰기 안전 상한은 30 FPS로 유지한다.
  640x360의 31~120 FPS에서는 AE auto가 exposure seed를 생략하고, 수동 노출 및
  manual AE 전환은 I2C 전에 `-EBUSY`로 거부한다.
- 120은 요청 허용 상한이며 실제 전달 FPS 보장은 아니다. 기존 KEEP/FHD-readout
  실측 113~115 FPS와 엄격 118.8 FPS 미달 결과를 그대로 공개한다.

## [2.9] - 2026-08-28

### Fixed
- 듀얼 합성 모드에서 서로 다른 채널별 디지털 줌 배율이 AP1302 센서 판독 높이를
  다르게 만들 수 있는 위험을 공통 배율 ABI로 구조적으로 차단했다.
- 녹색 화면은 센서/ISP 손상이 아니라 `1280x360 RGBP` raw를 UYVY로 해석한
  consumer format mismatch임을 raw RGB565 및 RTSP decode로 확인했다.
- 후보 빌더를 tracked working-tree의 임시 snapshot에서 실행해 연구용 120 FPS
  module/object가 양산용 generic build 경로에 남거나 기존 후보 artifact를 다음
  `make clean`이 삭제하지 않게 했다.
- 엄격 120 FPS 판정은 유효한 sensor/AP HINF 표본 전부와 CSI/ISI가 각각
  118.8 FPS 이상일 때만 통과하며, AP 읽기 실패를 0 FPS 평균이나 합격으로
  처리하지 않는다.

### Added
- single 640x360과 dual 1280x360(채널당 640x360)의 AP1302 preview context를
  width/height/full-ROI/aspect 단위로 원자 적용한다. 기본 `KEEP`은 AP1302/CSI
  출력만 바꾸며 AR0234 sensor readout 변경을 주장하지 않는다.
- `crop_enable` boolean V4L2 제어(기본 false)와 prepare fingerprint를 추가했다.
  false에서는 `0x1010`, `0x1012`, `0x118c`, `0x118e` 쓰기가 0회이고 true에서는
  공통 배율·채널별 중심 전체 tuple을 factor-last 순서로 적용한다.
- 640x360 sensor-mode `KEEP`, 0~15 후보를 같은 revision에서 격리 빌드하는 도구와
  AP1302/AR0234 readback, 엄격한 120 FPS, CPU/RSS/DDR/온도, raw UYVY 녹색·stride
  측정 게이트를 추가했다.

### Changed
- 디지털 줌 배율은 공통 `dz`만 노출하고 기본값을 AP1302 기본값과 같은
  `100`(1.00x)으로 변경했다. 채널별 `dz_chX` 컨트롤과 캐시는 제거했다.
- 조준 자유도는 채널별 `dz_x_chX`/`dz_y_chX`로 유지한다. 펌웨어 재로드와
  스트림 재시작 때도 공통 배율과 채널별 중심을 복원한다.
- 1920x1080/1280x720/640x360 production 일반 상한과 모든 모드의 노출 쓰기
  안전 상한을 30 FPS로 유지한다. 후보 조사용 build만
  `MAX9296_360P_MAX_FPS=120`을 명시한다. qualification build의 high-FPS AE auto는
  exposure seed만 생략하고, 수동 노출은 첫 mode I2C 전에 거부한다.
- streaming 중 `crop_enable` 변경은 `-EBUSY`; 배율·중심 tuple은 runtime 변경
  가능하다. 동일 enable 값의 no-op은 성공하고 실제 전환만 `-EBUSY`다.
  true→false의 기존 hardware crop 제거에는 hard reset/firmware reload가 필요하며
  gstApp 재시작만으로는 충분하지 않다.
- 드라이버 버전 2.8 -> 2.9.

### 검증
- health exporter 22건, probe cleanup, parallel prepare, 360p/노출/공통 줌,
  후보 빌드 17건, FPS/readback, resource capability, UYVY 무결성 테스트 통과.
- KEEP 640x360에 120 FPS를 요청한 보드 실측은 AP/CSI/ISI 약 113~115 FPS로
  엄격 기준 118.8 FPS를 통과하지 못했다. sensor-mode 0~15를 두 번씩 hard reset해
  확인했으나 검증 가능한 full-FOV 640x360 전용 readout profile은 없었다.
- i.MX8 BSP 5.10.35 크로스 빌드 성공(`max9296.ko`, srcversion
  `DA89ABE8A6E147911293CE6`, SHA-256
  `b27ae021fe4cb569ed6264712fabebb2a6b2cb6f5ab27278aebdb4113e09fc33`).

## [2.8] - 2026-08-27

### Added
- `1280x360` 듀얼(채널당 640x360)과 `640x360` 싱글 left/right 모드를 V4L2
  format 및 parallel prepare ABI에 추가.
- 공유 `dz`/`dz_x`/`dz_y`와 채널별 `*_chX` 디지털 줌 컨트롤. 줌 배율은
  100~300%를 AP1302 8.8 fixed-point로 변환하고, 중심 좌표는 0~65535 정규화 ABI를
  `DZ_CENTER_X/Y(0x118c/0x118e)`로 변환한다. 캐시는 펌웨어 재로드 뒤 복원된다.

### Safety
- 모드의 일반 `max_fps`와 별도로 `exposure_safe_max_fps=30` 정책을 추가했다.
  31~120 FPS의 `EXP_TIME(0x500c)` 쓰기는 I2C 전에 `-EBUSY`로 거부하고 채널,
  모드, FPS, 요청 노출값, 안전 상한을 로그에 남긴다.
- 모든 `0x500c` 쓰기를 단일 guarded helper로 통합했으며, SoC 정지 이력이 있는
  수동 WB `0x510a` 쓰기는 추가하지 않았다.
- `0x1012`는 중심 X가 아니라 줌 전이 속도임을 레지스터 문서와 보드에서 확인해
  즉시 적용값 `0x8000`만 사용한다. 중심 조준은 `0x118c/0x118e`를 사용한다.

### Changed
- 드라이버 버전 2.7 -> 2.8.
- live control I2C 주소는 요청 중인 `current_mode`가 아니라 실제 프로그램된
  `last_mode` 토폴로지를 기준으로 선택한다. pending S_FMT 값은 캐시만 하고 성공한
  STREAMON 이후 하드웨어 쓰기를 재개한다.

### 검증
- 호스트 계약/회귀 테스트: health exporter 22건, probe cleanup, parallel prepare,
  360p/노출/줌 테스트 통과.
- i.MX8 BSP 5.10.35 크로스 빌드 성공(`max9296.ko`).
- AP1302 레지스터 사전 보드 측정: 채널별 `0x118c/0x118e` 이동 독립성 및
  `0x1012` step 동작 확인.

## [2.7] - 2026-08-21

### Changed
- `ctrl_cache.pending` 제거 (#28, #29). 2.6 이 고친 것은 증상이고, 원인은 이
  플래그가 dirty 표시와 실행 게이트 두 뜻을 겸한 것이었다.

  독자는 `max9296_apply_cached_controls()` 의 가드 하나뿐인데, 두 호출부
  (`max9296_stream_commit_locked()`, `max9296_enable()`)가 **모두** 부르기 직전에
  `pending = true` 로 세워 그 가드를 무력화하고 있었다. 게이트는 아무 역할도 하지
  않으면서 "누가 먼저 소모했는가"로 동작을 가르는 함정만 남긴 셈이다 (#26).

  캐시가 비어 있는데 apply 가 불릴 창은 존재하지 않는다 - probe 는 5749 행
  `v4l2_ctrl_handler_setup()` 에서 `s_ctrl` 을 돌려 캐시를 채우고(이 시점
  `firmware_ready` 가 false 라 하드웨어는 건드리지 않는다), subdev 등록은 그보다
  뒤인 5862 행이다. apply 로 가는 두 경로는 모두 등록 이후에야 열린다. 따라서
  가드는 무언가를 막고 있던 것이 아니라 도달 불가 구간을 지키고 있었다.

  플래그를 없애면 apply 는 무조건 적용이 되고, 호출부가 dirty 상태를 거짓으로
  세우는 관용구가 사라지며(#28), 읽기와 clear 사이에 동시 `s_ctrl` 이 유실될 창도
  함께 사라진다(#29). `firmware_ready` 는 독자가 5 곳으로 실제 판단에 쓰이므로
  그대로 둔다.

- 드라이버 버전 2.6 -> 2.7

### 검증
- 빌드: `./make-for-imx8` exit 0, `srcversion` 19824075B55FFF0DB6EF7CA(2.6) ->
  A490660F641EEB5577DD19B(2.7)
- 온타겟 (2026-08-21, pim-camera-v016, 720p_4ch ch0 ae_off·gain 512).
  2.6 과 같은 결정 신호로 회귀 없음을 확인했다:

  |  | `0x5002` | `0x5006` |
  | --- | --- | --- |
  | 2.7 콜드 정착 후 | 0x0290 manual | 0x0200 |
  | 2.7 respawn ×3 | 0x0290 manual | 0x0200 |
  | 2.7 respawn 후 15/25/40/55s | 0x0290 유지 | 0x0200 유지 |

  대조군 2.5 는 respawn 후 `0x0299`(AUTO)가 120 초 이상 유지되는 종착 상태다.

- **측정 주의**: 콜드 경로는 정착에 약 25 초가 걸린다. 실측 샘플:
  `t+10s 0x029c/0x0100` -> `t+15s 0x0290/0x0100` -> `t+25s 0x0290/0x0200`.
  15 초 이내에 읽으면 전이 중인 값을 잡는다. 이 레지스터를 보는 검사는 콜드
  기동 후 25 초 이상 기다려야 한다.

## [2.6] - 2026-08-21

### Fixed
- 수동 AE 설정의 실효 여부가 스트림 라이프사이클 순서에 따라 갈리던 것 (#26).
  `max9296_enable()` 의 enable 스레드는 AE/AWB/LSC 를 하드코딩으로 초기화한 뒤
  `max9296_apply_cached_controls()` 로 V4L2 캐시를 복원하는데, 그 복원 함수가
  `ctrl_cache.pending` 을 게이트로 쓰고 스스로 소모한다. `s_stream(1)` ->
  `max9296_stream_commit_locked()` 가 먼저 apply 해 pending 을 소모하면 이
  복원이 통째로 no-op 이 되고, 바로 위에서 쓴 하드코딩 AUTO(`0x5002=0x299`)가
  최종 상태로 남았다.

  콜드부트는 이쪽이 먼저 돌아 설정값(manual)이 이기고, 앱 재기동(respawn)은
  반대편이 먼저 돌아 설정이 소실됐다 - 같은 설정이 기동 순서에 따라 다르게
  동작했다. pim-check smoke 의 fhd_4ch ch0(ae_off·manual gain) bitrate 검증이
  콜드부트에서 백색 포화로 4/4 FAIL 한 원인이다.

  복원은 멱등해야 하는 동작이므로 소모성 게이트를 걸 이유가 없다. 하드코딩
  직전 상태와 무관하게 다시 적용하도록 `pending` 을 세우고 호출한다 -
  `max9296_stream_commit_locked()` 의 quick-restart 경로가 쓰는 관용구와 같다.

- 드라이버 버전 2.5 -> 2.6

### 검증
- 빌드: `./make-for-imx8` exit 0, `srcversion` 41D8E9E128B7BB8873D14D7(2.5) ->
  19824075B55FFF0DB6EF7CA(2.6)
- 온타겟 (2026-08-21, pim-camera-v016, 720p_4ch ch0 ae_off·gain 512).
  판정 신호는 `pkill` -> respawn 후 ch0 AE_CTRL(`bus2 @0x11 0x5002`) readback:

  |                    | 0x5002 | 0x5006 |
  | --- | --- | --- |
  | 2.5 콜드 기동 후   | 0x0290 manual | 0x0200 |
  | 2.5 respawn 후     | **0x0299 AUTO** | 0x0200 |
  | 2.6 콜드 기동 후   | 0x0290 manual | 0x0200 |
  | 2.6 respawn 3 회   | **0x0290 manual** | 0x0200 |

  2.5 는 SIGTERM 한 번으로 뒤집혔고 2.6 은 3/3 유지했다. 게인 미러
  `0x5006` 은 양쪽 모두 잔존한다. 2.6 상태에서 정상 녹화도 확인했다 -
  ch0 세그먼트 7.4MB(720p@30 gain 512)로, 백색 포화 시의 ~65kbps 와
  명확히 구분된다.

## [2.5] - 2026-08-20

### Fixed
- prepare 가 벤더 ISI capture 드라이버의 반환되지 않는 V4L2 전원 참조를 lease 로
  인수 (`0608424`). `imx8-isi-cap.c` 는 `s_power(1)` 만 호출하고 해제 경로가
  `s_stream(0)` 까지만 가서 `power_count` 가 단조 증가한다. 그래서 첫 카메라 기동
  이후의 모든 prepare 가 `-EBUSY` 로 막혔고 rebind 나 재부팅 외에 회복 경로가
  없었다. 실측: `s_power(1)` 14회 / `s_power(0)` 0회.

### Changed
- prepare **admission** 게이트가 `power_count` 대신 실제 `streaming` 여부로만
  거부한다. 이 BSP 에서 `power_count` 는 살아있는 소유자의 증거가 아니다.
  `max9296_cancel_prepare()` 의 게이트는 바꾸지 않았다 - 인수가 `power_count` 를
  0 으로 만들어 정상 흐름에서 그대로 동작한다.
- 드라이버 버전 2.4 → 2.5

### 검증
- 온타겟(pim-camera-v016): 누수 잔류 상태에서 prepare 성공, 그 구간 펌웨어
  다운로드 0건(`epoch` 불변 - warm 재사용 유지), cancel 후 재 prepare 성공,
  스트리밍 중에는 여전히 `-EBUSY`
- 보드 게이트 **G1~G4 전부 통과** (2026-08-21, 드라이버 2.5 한 버전). G4 는 사이클마다
  하드 리셋하는 방식으로 dual 50/50 + single 50/50 을 완주했고, 각 사이클이 펌웨어
  재다운로드와 `v4l2-ctl` 종료 상태를 함께 확인한다 (`docs/prepare-board-gate-v1.md`)

## [2.4] - 2026-08-12

### Added
- 읽기 전용 `health_raw` sysfs ABI: MAX9296 DES, RX3 GMSL link, MAX9295 SER
  management endpoint, AP1302 ISP HINF counter를 요청 시점에 한 번만 샘플링
- `tools/max9296_health_export.py`: 두 MAX9296 인스턴스의 raw snapshot을
  `/run/pim-camera/max9296.json` camera-health v1 문서로 원자적으로 변환
- DES/GMSL/SER/ISP/Sensor를 분리한 진단 상태와
  `configured_channel_mask`, `physical_present_mask`,
  `stream_domain_active_mask` 세 종류의 mask
- dual-wide 공유 stream domain, 독립적인 MAX9295/AP1302 remote probe branch,
  SER 귀속 불가 및 Sensor/ISP stall 모호성에 대한 단위 테스트

### Changed
- sysfs attribute 생성 실패 시 이미 생성한 attribute를 역순으로 회수
- 드라이버 버전 2.3 → 2.4

### Safety
- health read는 reset, power toggle, register write, module reload를 수행하지 않음
- health용 I2C read는 retry/log 없이 한 번만 시도하며 control mutex가 사용 중이면
  대기하지 않고 `busy:true`를 반환
- AR0234 deep DMA probe는 수백 ms 지연 가능성 때문에 이 shallow ABI에서 제외

## [2.1] - 2026-04-23

### Added
- **MCP4018 VCC power V4L2 컨트롤**: `mcp4018_power_ch0/ch1` (bool). MAX9295 MFP4 GPIO로 MCP4018 I²C-bus 게이트를 제어. 진단/디버그용 standalone handle
- **apply_channel_controls에서 led_flash replay**: 캐시된 `ch_ctrl->led_flash`를 AR0234 R0x3270으로 DMA write. firmware_ready 이전에 V4L2로 내려온 설정이 초기화 완료 후 자동 적용됨
- **apply_channel_controls에서 MCP4018 wiper replay**: 지정 port의 MFP4 GPIO를 열고 wiper를 쓴 뒤 닫는 원자 시퀀스를 함수 내부에서 수행. dual/single 모드 콜러가 포트 정보(ser_addr/host/wiper)를 넘겨 per-channel replay로 통합

### Changed
- **`V4L2_CID_MCP4018_WIPER/_CH1` handler 원자화**: s_ctrl 내부에서 MFP4 open → I²C write → MFP4 close를 원자적으로 수행. Port A/B가 host 0x2F를 공유해도 코드 차원에서 상호배제되어 address remap 없이 두 포트 독립 wiper 설정 가능
- **통합 로그 포맷**: `max9296_apply_channel_controls`가 채널+모드+결과+상세(AE/AWB/gain/exp/rot/mcp/wiper/delay)를 한 줄로 출력. 예: `ch0 dual applied ok(addr:0x12 ae:on ... mcp:on wiper:0x3f delay:0x00) ret:0`
- MCP4018 주석 정정: "VCC controlled by MFP4 HIGH" → "I²C-bus gate controlled by MFP4 (wiper is retained by the pot after gate closes)"
- 버전 번호: 2.0 → 2.1

### Removed
- `max9296_apply_cached_controls` 말미 중복 요약 로그 `cached controls applied (exp:%d)` 제거 (채널별 통합 로그로 대체)

### Notes
- **MCP4018 port 매핑**: 드라이버 내부 "CH0/CH1"은 local(Port A / Port B) 개념. adapter 2 → 전역 ch0/ch1, adapter 1 → 전역 ch2/ch3에 대응
- **replay gating**: flash enable bit(0x100)가 꺼진 채널은 MCP4018 write를 생략 → 미장착 보드에서 ENXIO 로그 방지
- **Single 모드**: led_flash는 CH0 슬롯(AP1302 firmware 라우팅), MCP4018은 `sensor->enable`로 active local port를 선택

## [2.0] - 2026-02-11

### Fixed
- **[CRITICAL] kthread_stop UAF**: `max9296_shared_init` 자연 종료 후 task_struct 자동 회수로 인한 kthread_stop UAF 패닉 수정. `get_task_struct()`/`put_task_struct()`로 참조 카운트 관리
- **[HIGH] 듀얼 모드 peer UAF**: sensor_B remove 완료 후 sensor_A 스레드의 freed memory 접근 패닉 수정. 4-phase remove 구조로 재설계 (peer threads → own threads → cleanup → V4L2)
- **[MEDIUM] kthread_stop soft lockup**: `ssleep()`/`msleep()` 중 `kthread_should_stop()` 미확인으로 인한 soft lockup 패닉 수정
- **probe 실패 경로 리소스 누수**: `get_task_struct()` 이후 probe 실패 시 스레드 및 task_struct 참조 누수 수정. `free_ctrls` 에러 경로에 클린업 추가

### Refactored
- 커스텀 `max9296_interruptible_sleep()` → `msleep_interruptible()` 표준 커널 API 전환
- kthread_stop 후 스레드 포인터 NULL 할당 추가

### Changed
- 버전 번호: 1.9 → 2.0

## [1.9] - 2026-02-09

### Fixed
- **usleep_range 타이밍 최적화**: `max9296_load_regs`에서 delay_ms 사용 시 범위 폭을 10%로 증가하여 커널 타이머 효율성 개선 (708줄)
- **매크로 안전성 개선**: `_FILE_` 매크로 정의에서 `__FILE__` 참조에 괄호 추가하여 매크로 전개 시 연산자 우선순위 문제 방지 (46줄)

### Changed
- 버전 번호: 1.8 → 1.9

## [1.8] - 2026-02-08 (추정)

### Added
- FSYNC 기반 FPS 제어 메커니즘 문서화 (V4L2_CTRL_GUIDE.md)

### Fixed
- 죽은 코드(`#if 0` 블록) 11개 완전 제거
- `max9286_set_ctrl_pixelrate` 함수명 오타 수정 → `max9296_set_ctrl_pixelrate`
- CI 빌드 테스트를 Linux 5.10 환경에서 실행하도록 수정

### Refactored
- 코드 스타일 정리 및 로직 개선

## [1.7] - 2026-02-07 (추정)

### Fixed
- rmmod 시 kthread use-after-free 에러 수정
- build-test와 auto-rereview-request 워크플로우 비활성화

### Added
- GitHub Actions 워크플로우 추가

## [1.6] - 2026-02-06 이전

### Added
- 초기 드라이버 구현
- MAX9296 GMSL2 Deserializer 지원
- AP1302 ISP 통합
- 듀얼 채널 per-channel V4L2 커스텀 컨트롤 지원
- FSYNC GPIO 기반 프레임 동기화 (1~120 FPS)
- 48개 V4L2 컨트롤 지원
  - 채널별 AE, AWB, Gain, Exposure 제어
  - 채널별 Flip (H/V) 제어
  - 채널별 이미지 튜닝 (Brightness, Contrast, Saturation, LSC)

---

## 버전 관리 규칙

- **Major.Minor** 형식 사용
- **Major**: 주요 기능 추가 또는 호환성 변경
- **Minor**: 버그 수정, 경미한 개선, 코드 정리

## 링크

- [소스 코드](https://github.com/jhw7500/max9296)
- [이슈 트래커](https://github.com/jhw7500/max9296/issues)
