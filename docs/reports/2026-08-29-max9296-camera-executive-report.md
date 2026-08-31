# MAX9296 카메라 640×360 및 120 FPS 검토 결과 보고

보고일: 2026-08-29
대상: PIM MAX9296/AP1302/AR0234 듀얼 카메라
보고 목적: 640×360 출력, 디지털 crop, 노출 안전성 및 120 FPS 적용 가능성에 대한
개발·보드 검증 결과와 양산 권고안 보고

## 1. Executive Summary

카메라당 640×360 출력, gstApp/edgeconf 연동, 디지털 crop 및 노출 안전 가드 구현과
실보드 검증을 완료했다. 현재 구성은 양산에서 **640×360@30 FPS, AP1302
sensor-mode KEEP, crop 비활성, 1.0배**를 기준안으로 사용할 수 있다.

120 FPS 요청 시 AR0234 timing은 약 120~125 FPS로 설정되고 ch1 sensor counter는
약 119 FPS를 보였지만 AP1302 이후 CSI/ISI 실제 출력은 약 113 FPS에 머물러 합격
기준 118.8 FPS를 충족하지 못했다. 센서 입력량을 줄이는 기존 firmware mode 1도
동일한 약 113 FPS여서 성능 개선이 없었다. 따라서 현재 software와 firmware만으로
120 FPS를 양산 지원하는 것은 권고하지 않는다.

다음 단계는 AR0234 레지스터를 host에서 직접 덮어쓰는 방식이 아니라, 전체 16:9
화각을 유지하는 color sensor profile과 AP1302 출력/MIPI timing이 함께 포함된
vendor bootdata를 확보하는 것이다.

### 최종 권고

| 구분 | 권고안 |
|---|---|
| 현재 양산 설정 | 카메라당 640×360@30 FPS |
| sensor readout | AP1302 `KEEP` |
| 사용자 crop | 기본 비활성, 필요 제품에서 선택 적용 |
| zoom 기본값 | `dz=100`, 1.0배 |
| 120 FPS | 현 firmware에서는 미지원 유지 |
| 후속 개발 | vendor 16:9 color readout profile 확보 후 재검증 |

## 2. 개발 및 검증 성과

### 2.1 640×360 출력 경로

- MAX9296 드라이버에 AP1302 640×360 preview mode를 추가했다.
- gstApp이 edgeconf의 640×360 설정을 읽고 카메라 prepare 단계에 적용하도록 했다.
- 듀얼 카메라는 CSI에서 1280×360으로 전달되며 카메라별 영상 경계는 640 pixel이다.
- gstApp에서 별도의 software downscale을 수행하지 않고 sensor/AP1302/CSI 경로에서
  출력 크기를 결정한다.

### 2.2 디지털 crop과 조준

- 출력 해상도와 crop을 독립된 기능으로 구현했다.
- FHD, HD, 640×360 출력에서 동일한 crop 제어를 사용할 수 있다.
- 배율 `dz`는 듀얼 채널 공통이며 `100=1.0배`, `150=1.5배`, `200=2.0배`다.
- 중심 좌표는 채널별로 설정할 수 있고, crop이 활성화된 상태에서는 배율과 중심을
  스트리밍 중 변경할 수 있다.
- `crop_enable` 전환은 스트리밍을 정지하고 hard reset하는 절차를 사용한다.

### 2.3 노출 안전성

- `exp_time`과 채널별 노출 제어가 AP1302 manual exposure register `0x500C`를
  사용하는 것을 소스와 공식 register reference에서 확인했다.
- 안전 상한 30 FPS를 넘는 상태에서는 manual exposure write를 I2C 실행 전에
  `-EBUSY` 또는 `-EINVAL`로 거부한다.
- 거부 로그에 채널, 모드, FPS, 요청 노출값과 안전 상한을 남긴다.
- 안전 범위의 AE, exposure, gain 및 기존 제어 동작은 유지한다.
- SoC 정지 이력이 있는 manual WB register `0x510A` 쓰기는 추가하지 않았다.

### 2.4 녹색 화면 원인 규명

관측된 녹색 화면은 sensor 또는 ISP 색 처리 고장이 아니었다. 실제 capture format인
RGB565(`RGBP`) 데이터를 consumer가 UYVY로 해석한 pixel-format mismatch가
원인이었다. RGB565로 해석하거나 gstApp RTSP를 decode했을 때 녹색 dominance는
재현되지 않았다.

## 3. 센서 readout 및 120 FPS 비교 결과

### 3.1 현재 HD와 640×360의 동작 구조

HD와 640×360 KEEP 설정 모두 AR0234에서 1920×1080 active window를 읽는다.
AP1302가 이를 각각 1280×720 또는 640×360으로 축소한다. 따라서 출력 해상도를
640×360으로 설정해도 센서가 native 640×360을 출력하는 구조는 아니다.

```text
AR0234 1920×1080 readout
          ↓
AP1302 ISP / resampler
          ↓
카메라당 640×360 출력
          ↓
MAX9296/GMSL → CSI/ISI → gstApp
```

### 3.2 현재 firmware가 제공하는 sensor mode

실보드에서 양 AP1302의 `SENSOR0_CONF_0..7` table pointer를 확인한 결과는 다음과
같다.

```text
AECC AF10 AF90 0000 0000 0000 0000 A330
```

공식 AP1302 정의에 따라 실제 readout table이 존재하는 mode는 0, 1, 2뿐이다.
mode 3~5는 null이며 6/7은 readout mode가 아니라 sensor deselect/select event다.
따라서 과거 mode 3~15 sweep 결과는 독립적인 센서 profile이 아니라
unmapped/fallback 동작이다.

### 3.3 640×360@120 비교

공통 시험 조건은 듀얼 ch0/ch1, crop 비활성, 1.0배, 양 채널 AE auto이며 각
case마다 module 설치와 hard reset 후 20초간 측정했다.

| 항목 | KEEP | Mode 1 | Mode 2 |
|---|---:|---:|---:|
| 센서 입력 구조 | 1920×1080 | 약 960×600 subsampling | 약 960×600 + monochrome |
| AP HINF | 113.8 / 113.8 FPS | 113.7 / 113.8 FPS | 106.7 / 0.0 FPS |
| CSI | 113.6 / 113.3 FPS | 113.2 / 112.8 FPS | 0.0 / 0.0 FPS |
| ISI | 113.7 / 113.3 FPS | 113.2 / 112.8 FPS | 0.0 / 0.0 FPS |
| System CPU | 30.3% | 30.6% | 13.5%¹ |
| 120 FPS 판정 | 실패 | 실패, 개선 없음 | 실패, 영상 출력 없음 |

¹ Mode 2의 CPU 감소는 CSI/ISI가 0 FPS인 비정상 상태이므로 성능 개선으로 해석할
수 없다.

Mode 1은 AR0234의 sample 수를 줄였지만 AP1302 이후 FPS와 system CPU가 KEEP과
동일했다. 현재 120 FPS 제한은 단순히 sensor readout pixel 수만 줄여서는 해소되지
않으며, AP1302 output pacing 또는 후단 timing까지 포함한 profile 변경이 필요하다.

## 4. 양산 영향과 위험

| 항목 | 영향 및 대응 |
|---|---|
| 640×360@30 | 실보드 검증 완료, 현 양산 후보 유지 |
| 120 FPS | 약 113 FPS로 기준 미달, 지원 표기 금지 |
| 센서 mode 1 | 입력량 감소 효과는 있으나 출력 FPS·CPU 개선 없음 |
| 센서 mode 2 | monochrome 설정이며 120 FPS color pipeline 출력 불가 |
| direct sensor write | AP1302 firmware 설정과 충돌 가능, 양산 적용 금지 |
| 디지털 crop | 해상도와 독립적으로 사용 가능, enable 전환 절차 준수 |
| 녹색 화면 | 원인 확정; consumer pixel format을 RGBP와 일치시켜야 함 |
| 노출 제어 | 30 FPS 초과 manual write 사전 차단 정책 유지 |

시험 후 production module, edgeconf 및 service 상태를 원상 복구했다. 현재 보드는
640×360@30, KEEP, crop 비활성, `dz=100`, service active 상태다.

## 5. 의사결정 요청사항

1. 현 양산 기준을 **640×360@30 FPS + KEEP**으로 승인한다.
2. 현 firmware에서 120 FPS 지원 완료를 선언하지 않는다.
3. AP1302/AR0234 vendor에 다음이 포함된 새 bootdata 또는 sensor profile 제공을
   요청한다.
   - 전체 16:9 화각을 유지하는 color subsampling
   - 우선 검토안: 1920×1080 → 960×540 → AP1302 640×360
   - 해당 sensor timing과 AP1302 output/MIPI timing의 일관된 설정
4. vendor profile 확보 전에는 direct AR0234 register override를 양산 기능으로
   개발하지 않는다.

## 6. 후속 계획과 완료 기준

vendor bootdata 확보 후 다음 세 경로를 같은 조건에서 비교한다.

1. 1920×1080 full readout 후 AP1302 640×360 축소
2. 전체 화각 sensor bin/subsample 후 AP1302 640×360 축소
3. 중앙 640×360 sensor window crop — 비교용이며 full-FOV 양산안과 분리

120 FPS 지원 완료 조건:

- 양 채널 sensor/AP/CSI/ISI 실제 FPS가 모두 118.8 이상
- CSI-to-ISI 손실률 1% 이하
- overflow, CRC, ECC, lost-frame 및 timeout 없음
- 전체 16:9 화각, 양 채널 timing 및 영상 품질 일치
- 30/60/120 FPS 전환과 hard reset 후 반복 재현
- 기존 노출 안전 가드 및 manual WB 금지 정책 유지

## 부록 A. 보고용 발표 구성

### 1장 — 추진 배경

- 레일 하단 검사 영역을 위한 640×360 출력 및 crop 요구
- 실제 120 FPS 가능 여부와 sensor/ISP 부하 차이 확인 필요

### 2장 — 완료 범위

- 드라이버 640×360 mode
- gstApp/edgeconf 연동
- 디지털 crop·채널별 중심
- 노출 안전 가드와 녹색 화면 원인 규명

### 3장 — 해상도 처리 구조

- 현재 HD/640×360은 AR0234 1920×1080 readout
- AP1302 resampler가 최종 출력 해상도 생성
- 사용자 digital crop은 출력 해상도와 별도 기능

### 4장 — 120 FPS 실측

- KEEP 약 113 FPS
- Mode 1도 약 113 FPS로 개선 없음
- Mode 2는 120 FPS color 출력 없음

### 5장 — 양산 판단

- 640×360@30 KEEP 유지
- crop과 노출 안전 기능 적용 가능
- 120 FPS 지원은 보류

### 6장 — 지원 요청

- vendor 16:9 color sensor profile 및 AP1302/MIPI timing 확보
- 확보 후 동일 기준으로 재검증

## 부록 B. 근거 자료

- 전체 보드 검증: `docs/360p-readout-120fps-validation.md`
- 아티팩트 인덱스: `artifacts/board-20260828-qualification/README.md`
- firmware table: `artifacts/board-20260828-qualification/issue41-readout-809d582/firmware-mode-table-evidence.txt`
- KEEP/Mode 1/Mode 2 원시 결과:
  `artifacts/board-20260828-qualification/issue41-readout-809d582/run2/`
- GitHub 후속 이슈: https://github.com/jhw7500/max9296/issues/41
- driver 브랜치/커밋: `feature/360p-zoom-exposure-safety`, `edcdbb0`
- gstApp 브랜치/커밋: `feature/max9296-360p-zoom`, `895d1f2`
