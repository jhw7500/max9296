# MAX9296 해상도 모드·720x360 센서 readout·디지털 crop 설계

## 문서 상태

- 작성일: 2026-08-28
- 대상: `max9296` 커널 드라이버, gstApp, edgeconf, 타겟 보드 검증
- 상태: 대화에서 승인된 설계를 문서화한 구현 전 기준안
- 이전 문서의 공개 `readout_mode`와 내부 `dz=300` 변환안은 폐기한다.

## 1. 목표

최종 사용자 해상도를 카메라당 `1920x1080`, `1280x720`,
`720x360` 세 가지로 정리한다. 해상도 선택은 AP1302가 소유하는 센서
readout·기본 ROI·출력 크기를 결정하고, 검사 띠 조준을 위한 디지털 crop은
모든 해상도에서 독립적으로 켜고 끌 수 있게 한다.

`720x360@120`은 단순히 GStreamer caps를 120으로 표시하는 것이 아니라
AR0234 → AP1302 → MIPI CSI → ISI 전체 경로의 실측 FPS와 영상 무결성으로
검증한다. 센서 readout 방식과 full-readout 후 ISP 축소 방식은 공개 설정을
추가하지 않고 시험용 A/B 빌드로 비교한다.

기존 노출 안전 정책, AE·gain·AWB·flip 제어는 안전 범위에서 그대로 유지하며,
SoC 정지 이력이 있는 AP1302 `0x510A` 수동 WB 쓰기는 추가하지 않는다.

## 2. 확인된 근거와 미확정 사항

### 2.1 소스와 레지스터 문서에서 확인된 사실

- 드라이버의 `exp_time`과 채널별 노출 경로는
  `AP1302_REG_EXP_TIME(0x500C)` 쓰기로 모인다.
- 모드 정보에는 일반 동작 한계인 `max_fps`와 별도로
  `exposure_safe_max_fps`가 존재한다.
- AP1302 공식 로컬 레지스터 문서는
  `docs/AND9230-D (AP1302 RR)_PointImage.pdf`이다.
- AP1302 preview context의 주요 레지스터는 다음과 같다.

| 레지스터 | 의미 |
|---:|---|
| `0x2000` | `PREVIEW_WIDTH` |
| `0x2002` | `PREVIEW_HEIGHT` |
| `0x2004~0x200A` | `PREVIEW_ROI_X0/Y0/X1/Y1` |
| `0x200C` | preview aspect 관련 설정 |
| `0x2014` | `PREVIEW_SENSOR_MODE` |
| `0x201C` | `PREVIEW_LINE_TIME` |
| `0x2020` | `PREVIEW_MAX_FPS` |

`PREVIEW_SENSOR_MODE[13:12]`의 `CROP_CTRL` 의미는 다음과 같다.

| 값 | crop 위치와 의미 |
|---:|---|
| 0 | 센서 crop; 센서와 AP1302 처리량 절감 효과가 가장 큼 |
| 1 | AP1302 front pipe crop |
| 2 | 통계 영역은 유지하고 resampler 전에 crop |
| 3 | debug 용도 |

낮은 sensor-mode 비트의 정확한 의미는 연결된 센서와 AP1302 펌웨어에
따라 달라질 수 있으므로 보드 readback 없이 값을 추정하지 않는다.

### 2.2 보드에서 확인된 기존 640x360 경로

듀얼 출력 `1280x360@1/120`, 15초 raw IRQ 계측 결과는 다음과 같다.

| 공개 `dz` | AR0234 active window | CSI FPS | ISI FPS |
|---:|---:|---:|---:|
| 100 | 1920x1080 | 113.580 | 112.348 |
| 200 | 960x540 | 115.379 | 115.013 |
| 300 | 640x360 | 114.083 | 112.884 |

원본 edgeconf의 복원 확인 SHA-256은
`b075884b59ce0d44b3361bd7ae37b0fa4bb04da2c96b94bddf4d9deb42447a6f`였다.

이 측정은 현재 펌웨어에서 디지털 줌이 센서 window까지 바꿀 수 있음을
보이지만, `dz=300`을 전용 센서 readout 모드로 간주할 근거는 아니다.
또한 그 변경만으로 유의미한 FPS 향상이 없었다. 따라서 최종 구현은
`dz=300`을 숨겨서 쓰지 않는다.

### 2.3 구현 전에 보드에서 확정해야 하는 사실

- 720x360에 적합한 AP1302 firmware sensor-mode 값
- 그 값이 실제 AR0234 crop/binning/subsampling과 타이밍을 일관되게
  구성하는지
- 양 채널에 같은 active window와 line/frame timing이 적용되는지
- 해당 경로가 120 FPS와 정상 UYVY 영상을 동시에 유지하는지

안정적인 펌웨어 지원 모드를 찾지 못하면 직접 AR0234 타이밍 레지스터를
추정하여 쓰거나 `dz=300`으로 대체하지 않는다.

## 3. 공개 해상도 계약

해상도는 카메라 한 대 기준으로 설정하고 듀얼 출력은 두 영상을 가로로
연결한다.

| 카메라당 해상도 | 단일 출력 | 듀얼 출력 | 화면비 |
|---:|---:|---:|---:|
| 1920x1080 | 1920x1080 | 3840x1080 | 16:9 |
| 1280x720 | 1280x720 | 2560x720 | 16:9 |
| 720x360 | 720x360 | 1440x360 | 2:1 |

기존 공개 `640x360`·`1280x360` 모드는 각각
`720x360`·`1440x360`으로 교체한다. gstApp의 해상도 검증, prepare
target 계산, V4L2 모드 표와 caps도 같은 계약을 사용한다.

### 3.1 720x360의 기본 ROI

720x360은 2:1이므로 1920x1080 전체 16:9 readout과 화면비가 다르다.
개념적으로는 중앙 `1920x960` 영역을 기준으로 720x360을 구성하며,
전체 1080 높이와 비교하면 상하 합계 약 11.1%의 수직 FOV를 제외한다.

이는 사용자 디지털 crop이 아니라 해상도를 성립시키는 기본 ROI다.
`crop_enable=false`여도 적용된다. 정확한 AR0234 window, binning 또는
subsampling 조합과 ROI 레지스터 값은 AP1302 sensor-mode 보드 검증 결과로
확정한다.

## 4. 해상도 기반 센서 readout

edgeconf와 V4L2에는 `readout_mode`를 추가하지 않는다. 선택된 해상도
모드가 내부적으로 다음 AP1302 context를 결정한다.

1. `PREVIEW_WIDTH/HEIGHT`
2. 해상도별 기본 ROI와 aspect
3. 보드에서 검증된 `PREVIEW_SENSOR_MODE`
4. 해상도·FPS별 preview timing 정책

1920x1080과 1280x720은 현재 검증된 경로를 유지한다. 720x360은 센서에서
가능한 한 이른 단계에 crop/binning/subsampling을 수행하여 센서와 AP1302의
불필요한 픽셀 처리를 줄이는 경로를 목표로 한다.

AP1302가 센서 설정의 소유자이므로 드라이버가 AR0234 window·line length·
frame length·read mode를 새 모드 값으로 직접 쓰지 않는다. AR0234 DMA
접근은 후보 모드의 결과를 readback하는 검증 용도로만 사용한다.

## 5. 디지털 crop 인터페이스

디지털 crop은 기본 해상도와 독립적이며 1920x1080, 1280x720, 720x360의
단일·듀얼 구성 모두에서 사용할 수 있다. crop을 켜도 출력 해상도는
바뀌지 않고, 기본 ROI 안의 일부를 선택해 같은 출력 크기로 resample한다.
기본 ROI 밖의 FOV를 복원할 수는 없다.

### 5.1 V4L2 제어

| 제어 | ID | 범위 | 기본값 | 적용 범위 |
|---|---:|---:|---:|---|
| `crop_enable` | `V4L2_CID_USER_BASE + 0x102B` | 0 또는 1 | 0 | CSI/MAX9296 공통 |
| `dz` | `V4L2_CID_USER_BASE + 0x1022` | 100~300 | 100 | 두 채널 공통 |
| `dz_x_ch0/ch1` | 기존 `+0x1027/+0x1028` | 0~65535 | 32768 | 채널별 |
| `dz_y_ch0/ch1` | 기존 `+0x1029/+0x102A` | 0~65535 | 32768 | 채널별 |

기존 공통 `dz_x`·`dz_y` 제어는 호환 목적으로 유지하며, 활성 채널
전체에 같은 중심을 설정하는 의미로 처리한다. `dz`는 듀얼 결합 영상의
두 높이가 달라지는 것을 막기 위해 공통값만 허용한다.

### 5.2 AP1302 실제 레지스터 매핑

초기 요구표의 `dz_x=0x1012`, `dz_y=0x1014` 매핑은 AP1302 공식
레지스터 의미와 다르므로 다음과 같이 바로잡는다.

| 사용자 의미 | AP1302 레지스터 | 변환·쓰기 |
|---|---:|---|
| 확대 배율 | `DZ_TGT_FCT 0x1010` | `round(dz * 256 / 100)` |
| 즉시 반영 | `DZ_STEP_FCT 0x1012` | `0x8000` |
| 중심 X | `DZ_CENTER_X 0x118C` | 0~65535를 0x0000~0x0100으로 변환 |
| 중심 Y | `DZ_CENTER_Y 0x118E` | 0~65535를 0x0000~0x0100으로 변환 |

`0x1014`는 `DZ_CUR_FCT`, 즉 현재 factor이며 Y 중심 설정 레지스터가
아니다. 새 구현은 `0x1014`를 쓰지 않는다.
`DZ_STEP_FCT(0x1012)`의 문서상 reset 값은 `0x7FFF`지만, crop을
활성화해 적용할 때는 다음 update frame에 즉시 반영하도록 의도적으로
`0x8000`을 쓴다. 공개 중심 기본값 `32768(0x8000)`은 변환 후
`DZ_CENTER_X/Y=0x0080`, 즉 정중앙을 뜻한다.

배율 예시는 다음과 같다.

| 공개값 | AP1302 8.8 값 | 기본 ROI에서 보이는 선형 범위 |
|---:|---:|---:|
| 100 | `0x0100` | 100%, 1.00배 |
| 150 | `0x0180` | 약 66.7%, 1.50배 |
| 200 | `0x0200` | 50%, 2.00배 |
| 300 | `0x0300` | 약 33.3%, 3.00배 |

예를 들어 1920x1080에서 `dz=200`은 기본 ROI의 중앙 절반 크기를
선택해 다시 1920x1080으로 출력한다. 1280x720과 720x360도 각자의 기본
ROI 안에서 동일한 배율 의미를 갖는다.

### 5.3 enable과 런타임 규칙

`crop_enable=false`일 때 드라이버는 live apply, prepare replay,
firmware replay 어디에서도 다음 레지스터에 host I2C 쓰기를 발행하지
않는다.

- `0x1010`
- `0x1012`
- `0x118C`
- `0x118E`

`dz`와 중심값은 범위 검증 후 cache만 한다. 해상도 기본 ROI를 위한
`0x2000~0x2014` 쓰기는 이 enable의 대상이 아니므로 항상 수행한다.

`crop_enable=true`이면 시작 준비 시 전체 crop tuple을 적용하고,
스트리밍 중 다음 값을 변경할 수 있다.

- 공통 `dz`
- 공통 또는 채널별 `dz_x`
- 공통 또는 채널별 `dz_y`

각 변경은 드라이버 lock 안에서 대상 채널의 현재 전체 tuple을 다시 쓰고
`DZ_TGT_FCT(0x1010)`를 마지막에 쓴다. 연관된 여러 값을 한 프레임
경계에 최대한 일관되게 적용하려면 V4L2 control cluster와
`VIDIOC_S_EXT_CTRLS`를 사용한다. 중간 I2C 실패 시 채널, 공개 배율,
중심값과 errno를 기록하고 오류를 호출자에게 반환한다.

`crop_enable` 자체는 스트리밍 중 변경할 수 없으며 `-EBUSY`를
반환한다. 정지 상태에서는 cache를 바꾸고 prepared state를 stale로 만든다.
`true`에서 `false`로 바꿀 때 1배 기본값을 강제로 쓰지 않는다.
이전 crop을 확실히 제거하려면 다음 prepare에서 AP1302 firmware reload가
발생해야 하며, 운영 절차에서는 `cam_hard_reset.sh -s -S` 또는
`init_cam.sh`를 사용한다. gstApp만 재시작하는 것은 하드웨어 epoch
변경으로 간주하지 않는다.

## 6. edgeconf와 gstApp

새 JSON 키는 기존 GStreamer 후단 crop용 `crop_en`과 혼동하지 않도록
각 CSI의 I2C 객체 안에 `crop_enable`로 둔다.

```json
{
  "width": 720,
  "height": 360,
  "i2c2": {
    "crop_enable": false,
    "dz": 100,
    "ch0": {
      "dz_x": 32768,
      "dz_y": 32768
    },
    "ch1": {
      "dz_x": 32768,
      "dz_y": 32768
    }
  }
}
```

- 키가 없거나 타입·값이 잘못되면 `false`로 정규화하고 로그를 남긴다.
- `dz`는 I2C 객체의 공통값이고 중심은 채널별 값이다.
- gstApp은 `crop_enable`과 crop tuple을 V4L2에 적용한 뒤
  `max9296_prepare_all()`을 호출한다.
- crop tuple은 가능하면 한 번의 `VIDIOC_S_EXT_CTRLS`로 보낸다.
- JSON은 시작 구성이지 hot-reload 인터페이스가 아니다.
- 단일/듀얼 prepare target과 GStreamer caps는 3절의 해상도 표를 따른다.

## 7. 준비 순서와 상태 일관성

`crop_enable`을 shared control cache와
`max9296_hw_fingerprint`에 포함한다. 따라서 enable이 다른 준비 결과를
기존 hardware epoch로 오인하여 재사용하지 않는다.

준비 순서는 다음과 같다.

1. 모드, 요청 FPS, 노출 안전성, crop 범위를 I2C 전에 검증한다.
2. MAX9296/MAX9295 모드 표를 적용하고 AP1302 firmware를 로드한다.
3. AP1302 atomic context에서 output width/height, 기본 ROI,
   검증된 sensor mode와 high-FPS 정책을 적용한다.
4. `crop_enable=true`일 때만 디지털 crop tuple을 적용한다.
5. AE, exposure, gain, AWB, flip, LSC, LED 등 기존 제어를 replay한다.
6. 모든 단계가 성공한 뒤 hardware fingerprint를 publish한다.
7. 그 뒤에만 MIPI 출력과 FSYNC를 허용한다.

중간 실패 시 prepared state와 fingerprint를 무효화하고 MIPI/FSYNC를
켜지 않는다.

## 8. FPS와 노출 안전 정책

일반 스트림 동작 한계와 노출 레지스터 쓰기 안전 한계를 분리한다.

- `max_fps`: 해당 모드가 협상·동작할 수 있는 일반 최대 FPS
- `exposure_safe_max_fps`: AP1302 `0x500C` 쓰기를 허용하는 안전 상한

현재 안전 상한은 30 FPS다. 현재 모드나 FPS가 유효하지 않으면
`-EINVAL`, 일반 모드는 유효하지만 안전 상한을 넘으면 `-EBUSY`를
반환한다. 거부 로그에는 채널, 모드 ID·해상도, 현재/요청 FPS, 요청 노출값,
안전 상한과 errno를 포함하며, 로그 후 쓰기를 계속하지 않는다.

`exp_time`, 채널별 exposure, AE 전환 또는 replay를 포함한 모든
`0x500C` 경로는 같은 guard helper를 통과해야 한다. 안전 범위에서는
기존 노출·gain·AE 동작을 바꾸지 않는다.

30 FPS를 넘는 준비에서 AE가 활성화되어 있고 `0x500C`를 쓸 동작이
없다면 스트림 자체를 거부하지 않는다. 반대로 cache/replay에 실제
`0x500C` 쓰기가 예정되어 있으면 첫 mode-table I2C 쓰기 전에 거부한다.
따라서 120 FPS 시험은 수동 노출 요청 없이 AE 경로로 시작한다.

720x360은 보드 검증을 거쳐 카메라당 최대 120 FPS를 목표로 한다.
1920x1080과 1280x720의 일반 `max_fps`는 이번 120 FPS 자격 검증으로
임의 상향하지 않는다.

### 8.1 720x360 high-FPS preview 정책

720x360에서 요청 FPS가 30을 넘을 때 각 활성 AP1302에 다음 atomic
정책을 적용한다.

1. `ATOMIC(0x1184)=0x0001`
2. `PREVIEW_MAX_FPS(0x2020)=fps << 8`
3. `TRIGGER_MAX_MISMATCH(0x6112)=0x0000`
4. `ATOMIC(0x1184)=0x0013`

초기 자격 시험에서는 `PREVIEW_LINE_TIME(0x201C)`을 자동값으로 둔다.
보드 계측에서 sensor readout이 목표 cadence를 만들지 못하고 검증된
line-time 값이 필요한 경우에만 후속 설계 변경으로 추가한다.

중간 쓰기 실패 시 첫 오류를 보존하고 atomic finish를 best effort로
시도한 뒤 준비를 실패시킨다.

## 9. 센서 readout 대 ISP 축소 A/B

공개 `readout_mode` 없이 동일한 소스 revision에서 sensor-mode 표
한 항목만 다른 두 개의 명시적 시험 빌드를 만든다.

| Case | 센서 측 | AP1302 출력 | 공개 crop |
|---|---|---|---|
| A: full-readout scale | 1920x1080 readout 유지 | 기본 aspect crop/scale 후 720x360 | disabled |
| B: sensor readout | 검증된 sensor crop/bin/subsample | 720x360 | disabled |

두 빌드는 artifact 이름과 git revision·시험 mode-table 차이를 기록하며
edgeconf/V4L2에는 이를 선택하는 필드를 넣지 않는다. 각 case 사이에는
hard reset을 수행한다.

동일하게 유지할 조건은 다음과 같다.

- `crop_enable=false`; 네 digital-crop 레지스터 쓰기 없음
- 동일한 요청 FPS와 노출/AE/AWB 설정
- 동일 조명, 카메라 위치와 온도 안정화 시간
- 동일 gstApp, encoder, RTSP와 계측 기간

### 9.1 예상되는 차이와 해석

두 case 모두 AP1302 이후에는 같은 720x360 UYVY 프레임과 FPS를 내므로
MIPI 출력 대역폭, ISI 픽셀 처리량, gstApp 메모리 이동량과 encoder 부하는
대체로 비슷할 수 있다. sensor readout의 주된 이점 후보는 AR0234가 읽는
행·열 수와 AP1302 front-end 입력 픽셀 수 감소에 따른 센서/AP1302
처리량, 전력과 온도 감소다.

반대로 sensor crop은 FOV를 줄이고, binning/subsampling 조합에 따라
노이즈·해상력·aliasing이 달라질 수 있다. full-readout scale은 센서와
AP1302 내부 처리량이 크지만 더 넓은 원본 정보를 resample할 수 있다.
이 차이는 CPU만으로 판단하지 않고 sensor window/timing, DDR, 온도,
화질과 FOV를 함께 비교한다.

gstApp과 RTSP 소비자는 두 case에서 같은 caps를 받는다. 응용단 자유도는
공개 `readout_mode`가 아니라 세 해상도 선택과 독립적인
`crop_enable/dz/center` 조합으로 제공한다.

## 10. 검증 계획

### 10.1 자동 시험

드라이버의 모드·crop·FPS 결정을 가능한 한 pure policy로 분리하고 먼저
실패하는 host test를 작성한다.

- 단일/듀얼 1920x1080, 1280x720, 720x360 모드 매핑
- 기존 640x360/1280x360 공개 모드 제거
- 720x360 기본 ROI/aspect와 sensor-mode 선택
- `dz` 100/150/200/300의 8.8 변환
- 0/32768/65535 중심 좌표 변환
- `crop_enable=false`에서 네 레지스터 write 호출 0회
- `crop_enable=true`의 전체 tuple 순서와 factor-last
- runtime crop 허용과 runtime enable 변경 `-EBUSY`
- control cluster와 채널별 중심값
- 31/60/120 FPS high-FPS 값과 30 FPS 이하 no-op
- `0x500C` 모든 경로의 사전 guard와 거부 로그 필드
- AP1302 `0x510A` 쓰기 부재
- prepare 실패 시 fingerprint/MIPI/FSYNC 미게시

gstApp host test는 다음을 확인한다.

- 최종 세 해상도와 단일/듀얼 target 폭
- `crop_enable` 기본/누락/잘못된 값은 false
- 공통 `dz`와 채널별 center parser
- crop control을 prepare 전에 한 tuple로 적용
- 기존 GStreamer `crop_en`과 새 hardware `crop_enable`의 분리

드라이버 health/prepare 테스트와 커널 모듈 빌드를 통과시킨다. gstApp은
plain `make`가 아니라 `./make-for-imx8`로만 빌드한다.

### 10.2 보드 sensor-mode 자격 시험

각 AP1302 sensor-mode 후보마다 다음 순서로 확인한다.

1. 보드 예약을 확보하고 기존 module, gstApp, edgeconf와 hash를 백업한다.
2. `crop_enable=false`와 720x360 기본 모드를 적용하고 hard reset한다.
3. AP1302 `0x2000~0x2014`를 readback한다.
4. 두 AR0234의 `0x3002/0x3004/0x3006/0x3008` window,
   `0x300C` line length, `0x300A` frame length,
   `0x3040` read mode를 readback한다.
5. 양 채널의 window와 timing이 동일하고 연속 재시작에서도 안정적인지
   확인한다.
6. AE/AWB, 노출 안전 guard와 영상 포맷을 확인한다.

후보가 안정적일 때만 production mode table에 넣는다.

### 10.3 120 FPS 및 자원 비교

각 A/B case에서 최소 15~20초 안정 구간을 측정한다.

- 수정된 `cam_fps_stack.sh` 결과
- raw CSI/ISI IRQ delta
- CSI-to-ISI 손실률
- overflow, CRC, ECC, lost-frame 로그
- gstApp CPU/RSS와 시스템 전체 CPU
- DDR PMU 사용량과 온도
- sensor/AP1302 window·timing·frame counter
- RTSP 실제 decode FPS와 캡처 프레임

120 FPS 합격 조건은 다음과 같다.

- raw CSI FPS와 ISI FPS가 각각 118.8 이상
- CSI-to-ISI 손실률 1% 이하
- overflow, CRC, ECC, lost frame 없음
- 대부분이 녹색인 프레임, 채널 분할 오류 또는 stride/caps 오류 없음

RTSP caps나 `videorate`의 120 표기만으로 합격 처리하지 않는다.

### 10.4 모드·crop 회귀

- 세 해상도의 단일·듀얼 스트림을 일반 FPS에서 확인한다.
- 720x360@120은 별도 엄격 기준으로 확인한다.
- 각 해상도에서 crop false와 true를 확인한다.
- crop true에서 100/150/200과 채널별 중심 이동을 런타임 적용한다.
- `VIDIOC_S_EXT_CTRLS` tuple의 일관성과 I2C 오류 반환을 확인한다.
- hard reset·firmware reload 후 JSON crop 값이 재적용되는지 확인한다.

## 11. 녹색 화면 처리

현재 관찰된 녹색 화면은 성능 결과와 별개의 경고가 아니라 합격을 막는
blocking failure다. 다음을 확인한다.

- V4L2/Media Bus UYVY 포맷
- single/dual 실제 frame width와 bytes-per-line/stride
- ISI output caps와 gstApp caps 협상
- dual 영상의 채널 분할 경계
- RTSP decoder가 받은 실제 프레임의 색상과 유효 픽셀 비율

FPS가 120에 근접해도 녹색 화면이 남아 있으면 해당 case는 실패다.

## 12. 배포·복구

자동 시험과 보드 자격 시험을 통과한 하나의 production build만 배포한다.

1. 설치된 kernel module, gstApp binary와 edgeconf를 run별 이름으로 백업한다.
2. 같은 git revision의 module과 gstApp을 배포한다.
3. 720x360, 120 FPS, `crop_enable=false` JSON으로 hard reset 후 검증한다.
4. 별도 시험에서 `crop_enable=true`와 runtime 중심/배율을 검증한다.
5. 세 해상도와 기존 안전 제어의 smoke test를 수행한다.
6. 결과와 register readback, FPS, 자원 사용량, 영상 캡처를 문서화한다.

실패 시 세 artifact를 모두 복원하고 `cam_hard_reset.sh -s -S`를 수행한
뒤 원본 hash와 서비스 상태를 확인한다. 마지막으로 보드 holder와 예약이
남아 있지 않은지 확인한다.

## 13. 중단 조건과 완료 기준

다음 중 하나라도 발생하면 120 FPS production 지원으로 배포하거나
지원 완료라고 주장하지 않는다.

- 안정적인 AP1302 firmware sensor mode를 찾지 못함
- 양 채널 AR0234 window/timing 불일치
- raw CSI 또는 ISI가 118.8 FPS 미만
- CSI-to-ISI 손실률 1% 초과
- overflow/CRC/ECC/lost-frame 발생
- 녹색 화면 또는 채널/stride 손상
- 노출 guard 우회나 새 `0x510A` 쓰기 발견

완료 결과에는 full-readout scale과 sensor readout의 실제 FPS, CPU,
메모리, DDR, 온도, 센서 window/timing, FOV와 화질 차이를 함께 기록한다.
720x360 모드가 낮은 FPS에서만 안정적이라면 그 결과를 별도 제한으로
문서화할 수 있지만 120 FPS 자격과 혼동하지 않는다.
