# Changelog

All notable changes to the MAX9296 driver will be documented in this file.

The format is based on [Keep a Changelog](https://keepachangelog.com/en/1.0.0/),
and this project adheres to [Semantic Versioning](https://semver.org/spec/v2.0.0.html).

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
