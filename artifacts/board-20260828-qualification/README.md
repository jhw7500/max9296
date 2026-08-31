# MAX9296 360p / crop / 120 FPS qualification artifact index

정리일: 2026-08-29
대상 보드: `pim-camera-v016`
드라이버 브랜치: `feature/360p-zoom-exposure-safety` (`809d582`)
gstApp 브랜치: `feature/max9296-360p-zoom` (`895d1f2`)

## 최종 판정

- production 기본은 카메라당 `640x360@30`, AP1302 sensor-mode `KEEP`,
  `crop_enable=false`, `dz=100`이다.
- `640x360@120` 요청은 AP/CSI/ISI 약 `113~115 FPS`로 동작했지만 엄격 기준
  `118.8 FPS`를 통과하지 못해 production 상한은 30 FPS로 유지했다.
- 640x360 KEEP과 HD 모두 AR0234 active window가 `1920x1080`,
  `X/Y_ODD_INC=1`이다. 현재 640x360과 HD는 센서 native readout이 아니라
  AP1302 resampler 축소 경로다.
- AP1302 `SENSOR0_CONF_0..7` readback 결과 이 firmware의 실제 readout table은
  mode `0/1/2`만 존재한다. mode `3~5`는 null, `6/7`은 deselect/select event이며
  `8~15`는 mapping 범위 밖이다. 이전 `0~15` sweep의 mode `3~15` 동일 readback은
  별도 profile이 아니라 unmapped/fallback 동작이다.
- mode `1/2`는 약 `960x600` 상당의 2x skip/bin/sum이다. mode 1의
  640x360@120 AP/CSI/ISI는 KEEP과 같은 약 113 FPS라 이득이 없었다. mode 2는
  AR0234 monochrome bit를 켜 640x360@120 color pipeline에서 CSI/ISI 0 FPS가
  됐다.
- 디지털 crop은 FHD/HD/360p와 독립이다. crop을 켜도 출력 해상도는 유지되며,
  `dz`와 채널별 중심 tuple은 스트리밍 중 변경 가능하다. `crop_enable` 전환은
  정지 상태와 hard reset이 필요하다.
- 녹색 화면은 센서/ISP 색 손상이 아니라 capture의 `RGBP`를 `UYVY`로 해석한
  format mismatch였다. gstApp RTSP decode에서는 녹색 dominance가 없었다.
- 노출 레지스터 `0x500c` 쓰기는 안전 상한 30 FPS를 넘으면 I2C 전에 거부한다.
  SoC 정지 이력이 있는 수동 WB `0x510a` 쓰기는 추가하지 않았다.

## 핵심 증적

| 디렉터리/파일 | 내용 |
|---|---|
| `final-production-d15d613/` | 최종 배포 상태, 30 FPS smoke, FHD/HD/360p runtime crop, RTSP PNG |
| `keep-120fps/` | KEEP 30/120 FPS, resource, dmesg, 녹색 화면 판별 로그 |
| `hd-vs-fhd-readout-8a5bbe4/` | HD와 FHD의 AP1302/AR0234 readback 직접 비교 |
| `discovery-675c0d6/` | AP1302 sensor-mode 0~15, 각 2회 hard-reset/prepare/readback 원시 로그 |
| `issue41-readout-809d582/firmware-mode-table-evidence.txt` | firmware provenance, SENSOR0_CONF pointer와 mode table raw bytes |
| `issue41-readout-809d582/run2/` | KEEP/SM01/SM02 640x360@120 FPS·resource·0x30B0·복구 결과 |
| `oddinc/` | mode 1/2의 AR0234 `X/Y_ODD_INC` 재확인 |
| `images/` | KEEP와 mode 2의 raw/decoded 비교 이미지 |
| `keep-final-30-readback.txt` | 시험 종료 후 production KEEP 상태 복구 확인 |
| `../360p-candidates/` | KEEP 및 sensor-mode 0~15 qualification 모듈 산출물 |

## 최종 배포 fingerprint

| 대상 | 값 |
|---|---|
| `max9296.ko` | SHA-256 `b27ae021fe4cb569ed6264712fabebb2a6b2cb6f5ab27278aebdb4113e09fc33` |
| gstApp | SHA-256 `08ae95d148300d5ef25999a4c801287c6613bee738b80c6fe63dae297ba239ee` |
| edgeconf | SHA-256 `eba521544a39d0a8ab79786e1d5b7a7c06357942a5d94c61691531960e53654f` |
| 최종 상태 | dual `1280x360`, 30 FPS, crop false, `CONSUMED/match=1`, service active |

## 구현 범위

### max9296 driver

- 640x360 AP1302 preview context와 해상도별 FPS 정책
- `crop_enable`, 공통 `dz`, 채널별 `dz_x/dz_y` V4L2 control 및 cache replay
- unsafe exposure 사전 거부와 상세 로그
- 360p candidate builder, sensor/AP/CSI/ISI FPS 및 resource 측정 도구
- AR0234 window/read-mode/odd-increment/timing DMA readback

관련 커밋 범위: `097265e..809d582` (18 commits).

### gstApp

- edgeconf의 MAX9296 hardware crop 설정 parsing
- camera prepare 전에 crop tuple 적용
- single/dual 640x360 prepare target 지원

관련 커밋: `b9f4767`, `741b61a`, `895d1f2`.

## 정본 문서와 후속 작업

- 상부 보고 자료: `docs/reports/2026-08-29-max9296-camera-executive-report.md`
- 전체 판정과 수치: `docs/360p-readout-120fps-validation.md`
- 설계 계약: `docs/superpowers/specs/2026-08-28-max9296-360p-readout-120fps-design.md`
- V4L2/edgeconf 사용법: `docs/v4l2-controls-guide.md`
- 배포 JSON 예시: `docs/examples/edgeconf-max9296-640x360-fragment.json`
- AR0234 직접 접근 근거: `docs/AR0234-register-access.md`
- 후속 조사: GitHub issue #41, `AR0234 센서측 HD/640x360 readout 모드 조사 및 검증`

현재 AP1302 firmware용 AR0234 sensor-mode table은 보드에서 확인했다. 그러나
정확한 full-FOV 16:9 HD/640 color profile은 포함돼 있지 않다. 후속 조사는 새 vendor
bootdata가 16:9 sensor sampling, AP1302 output timing과 MIPI 설정을 함께 제공할 때
재개한다. 그 전에는 production의 `KEEP + AP1302 scale` 경로를 유지한다.

## 저장소 상태 메모

`final-production-d15d613/`, `keep-120fps/`, `hd-vs-fhd-readout-8a5bbe4/`와 함께
candidate manifest, `discovery-675c0d6/`, `images/`, `oddinc/`,
`keep-final-30-readback.txt`, `issue41-readout-809d582/`의 비민감 원시 증적을 feature
브랜치에 보존한다. 재현용 `.ko`, production backup과 전체 edgeconf snapshot은
로컬에 유지하되 `.gitignore`로 제외한다. edgeconf에는 카메라 범위 밖의 deployment
또는 network credential이 포함될 수 있기 때문이다.
