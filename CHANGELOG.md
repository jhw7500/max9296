# Changelog

All notable changes to the MAX9296 driver will be documented in this file.

The format is based on [Keep a Changelog](https://keepachangelog.com/en/1.0.0/),
and this project adheres to [Semantic Versioning](https://semver.org/spec/v2.0.0.html).

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
