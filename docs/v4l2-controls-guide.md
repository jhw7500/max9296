# MAX9296 V4L2 커스텀 컨트롤 가이드

**대상 드라이버**: max9296.c (MAX9296 GMSL2 + AP1302 ISP)
**커널**: Linux 5.10.35 (NXP BSP, iMX8MP)

---

## 1. 채널 매핑

| 채널 | I2C 버스 | 디바이스 | AP1302 주소 | 컨트롤 접미사 |
|------|----------|----------|-------------|---------------|
| CH0 | I2C:2 | /dev/v4l-subdev2 | 0x11 | ch0 |
| CH1 | I2C:2 | /dev/v4l-subdev2 | 0x12 | ch1 |
| CH2 | I2C:1 | /dev/v4l-subdev3 | 0x11 | ch2 |
| CH3 | I2C:1 | /dev/v4l-subdev3 | 0x12 | ch3 |

Single 모드에서는 AP1302 주소 0x3c를 사용한다.

해상도와 디지털 crop은 독립이다. 공개 카메라당 해상도는 1920x1080,
1280x720, 640x360이며 dual V4L2 폭은 각각 3840, 2560, 1280이다. FHD/HD
일반 상한은 30 FPS, 640x360 드라이버 협상 상한은 120 FPS이고 모든 모드의
노출 쓰기 안전 상한은 30 FPS다. crop을 켜도 출력 크기는 변하지 않는다.

기본 640x360 `KEEP` 정책은 AP1302/CSI 출력을 640x360으로 바꾸지만 AR0234
sensor readout 모드를 강제로 선택하지 않는다. 따라서 전용 sensor-readout 후보와
ISP 축소 경로는 보드의 AR0234 window/read-mode/timing·전체 FOV로 구분한다.

---

## 2. 컨트롤 목록

### 2.1 전체 컨트롤 확인

```bash
v4l2-ctl -d /dev/v4l-subdev2 -l
v4l2-ctl -d /dev/v4l-subdev3 -l
```

### 2.2 Per-Channel 컨트롤 (AP1302 ISP)

v4l-subdev2 기준 (subdev3은 ch0→ch2, ch1→ch3).

| 컨트롤 이름 | 범위 | 기본값 | 설명 |
|-------------|------|--------|------|
| `ae_on_ch0` | 0~1 | 1 | AE 자동/수동 (1=auto) |
| `auto_white_balance_ch0` | 0~1 | 1 | AWB on/off |
| `auto_gain_ch0` | 0~1 | 1 | Auto Gain on/off |
| `gain_ch0` | 0~65535 | 256 | 수동 Gain 값 |
| `exp_time_ch0` | 0~INT_MAX | 10000 | 수동 Exposure Time (μs) |
| `hflip_ch0` | 0~1 | 0 | 수평 반전 |
| `vflip_ch0` | 0~1 | 0 | 수직 반전 |
| `lsc_ch0` | 0~65535 | 0x3fff | LSC 보정 강도 (fixed12) |
| `brightness_ch0` | 0~65535 | 0 | 밝기 (fixed12) |
| `contrast_ch0` | 0~65535 | 0 | 대비 (fixed12) |
| `saturation_ch0` | 0~65535 | 4096 | 채도 (fixed12) |
| `led_flash_ch0` | 0~0x1ff | 0 | AR0234 LED Flash (bit8=EN, bit7:0=DELAY) |
| `dz_x_ch0` | 0~65535 | 32768 | 정규화 중심 X |
| `dz_y_ch0` | 0~65535 | 32768 | 정규화 중심 Y |

CH1 컨트롤은 `_ch0` → `_ch1`로 동일 구조.

### 2.3 공유 컨트롤

| 컨트롤 이름 | 범위 | 기본값 | 설명 |
|-------------|------|--------|------|
| `exp_time` | 0~INT_MAX | 10000 | 공유 Exposure Time (양 채널 동기) |
| `dz` | 100~300 | 100 | 공통 디지털 줌 1.00x~3.00x |
| `dz_x` | 0~65535 | 32768 | 공유 정규화 중심 X |
| `dz_y` | 0~65535 | 32768 | 공유 정규화 중심 Y |
| `hue` | 0~359 | 0 | Hue |
| `power_line_frequency` | 0~3 | 1(50Hz) | 전원 주파수 필터 |
| `crop_enable` | 0~1 | 0 | hardware digital crop 적용 여부 |

### 2.4 MCP4018 디지털 가변저항

| 디바이스 | 컨트롤 이름 | 범위 | 기본값 | 설명 |
|----------|-------------|------|--------|------|
| subdev2 | `mcp4018_wiper_ch0` | 0~127 | 63 | Port B 가변저항 (50kΩ, 128단계) |
| subdev2 | `mcp4018_wiper_ch1` | 0~127 | 63 | Port A 가변저항 |
| subdev3 | `mcp4018_wiper_ch2` | 0~127 | 63 | Port B 가변저항 |
| subdev3 | `mcp4018_wiper_ch3` | 0~127 | 63 | Port A 가변저항 |

### 2.5 DMA 센서 레지스터 접근

AP1302 DMA를 통한 AR0234 센서 레지스터 직접 읽기/쓰기.

| 디바이스 | 컨트롤 이름 | 플래그 | 설명 |
|----------|-------------|--------|------|
| subdev2 | `dma_reg_write_ch0` | - | CH0 센서 레지스터 쓰기 |
| subdev2 | `dma_reg_write_ch1` | - | CH1 센서 레지스터 쓰기 |
| subdev2 | `dma_reg_read_ch0` | volatile, execute-on-write | CH0 센서 레지스터 읽기 |
| subdev2 | `dma_reg_read_ch1` | volatile, execute-on-write | CH1 센서 레지스터 읽기 |
| subdev3 | `dma_reg_write_ch2` | - | CH2 센서 레지스터 쓰기 |
| subdev3 | `dma_reg_write_ch3` | - | CH3 센서 레지스터 쓰기 |
| subdev3 | `dma_reg_read_ch2` | volatile, execute-on-write | CH2 센서 레지스터 읽기 |
| subdev3 | `dma_reg_read_ch3` | volatile, execute-on-write | CH3 센서 레지스터 읽기 |

값 인코딩 (32-bit): `[31:16] = 레지스터 주소, [15:0] = 데이터`

---

## 3. 사용 예시

### 3.1 기본 ISP 컨트롤

```bash
DEV=/dev/v4l-subdev2

# AE 끄고 수동 노출 설정
v4l2-ctl -d $DEV -c ae_on_ch0=0
v4l2-ctl -d $DEV -c exp_time_ch0=20000

# AE 다시 켜기
v4l2-ctl -d $DEV -c ae_on_ch0=1

# 밝기/대비 조절
v4l2-ctl -d $DEV -c brightness_ch0=8192
v4l2-ctl -d $DEV -c contrast_ch0=4096

# 수평 반전
v4l2-ctl -d $DEV -c hflip_ch0=1
```

노출 레지스터 `0x500c` 쓰기는 30 FPS 이하에서만 허용된다. 31~120 FPS에서는
`exp_time`, `exp_time_chX`, 수동 AE 전환이 I2C 전에 `-EBUSY`로 거부된다.
640x360 high-FPS의 AE auto 경로는 exposure seed만 생략하며 gain/AWB/flip 등은
그대로 적용한다. `0x510a` 수동 WB 쓰기는 추가하지 않았다.

### 3.2 디지털 줌

```bash
# enable 변경은 정지 상태에서만 가능
v4l2-ctl -d $DEV -c crop_enable=1

# 스트리밍 중 원자적 전체 tuple 변경
v4l2-ctl -d $DEV -c dz=200,dz_x_ch0=32768,dz_y_ch0=52000,dz_x_ch1=32768,dz_y_ch1=32768
```

`dz`는 AP1302 `0x1010` 8.8 fixed-point로 변환된다. 중심은 실제 중심 레지스터
`0x118c/0x118e`에 기록되며, `0x1012`에는 즉시 전이값 `0x8000`만 쓴다. 캐시는
펌웨어 재로드 후 복원되고 싱글 right는 공통 배율과 활성 ch1 중심 캐시를 사용한다.
듀얼 합성 모드에서 서로 다른 배율은 센서 판독 타이밍을 어긋나게 하므로 채널별
배율 컨트롤은 제공하지 않는다.

| `dz` | 배율 | `0x1010` |
|---:|---:|---:|
| 100 | 1.00x | `0x0100` |
| 150 | 1.50x | `0x0180` |
| 200 | 2.00x | `0x0200` |
| 300 | 3.00x | `0x0300` |

`0x1014`는 중심 레지스터가 아니며 쓰지 않는다. `crop_enable=false`에서는
`0x1010`, `0x1012`, `0x118c`, `0x118e` 쓰기가 0회다. true에서는 tuple을
step→X→Y→factor-last 순서로 적용한다. enable 자체의 스트리밍 중 변경은
`-EBUSY`다. true→false 뒤 기존 hardware crop을 확실히 지우려면 gstApp만
재시작하지 말고 `cam_hard_reset.sh -s -S` 또는 `init_cam.sh`를 사용한다.

edgeconf에는 `.VHL_CAM.cam_width=640`, `.VHL_CAM.cam_height=360`,
`.VHL_CAM.fps=120`을 사용한다. `width`/`height`는 gstApp parser 키가 아니다.
`docs/examples/edgeconf-max9296-640x360-fragment.json`을 기존 `.VHL_CAM`에
재귀 병합해 credential, enable, bitrate 등 장비별 값을 보존한다.

### 3.3 LED Flash 제어

```bash
# 켜기 (enable=1, delay=3)
v4l2-ctl -d $DEV -c led_flash_ch0=259
# 259 = 0x0103 → bit8=1(enable), bit7:0=3(delay)

# 끄기
v4l2-ctl -d $DEV -c led_flash_ch0=0
```

### 3.4 DMA 센서 레지스터 읽기

```bash
# 방법 1: v4l2-ctl 직접 사용
# 1) 읽을 주소 설정 (0x3000 = chip ID)
v4l2-ctl -d $DEV -c dma_reg_read_ch0=$((0x3000 << 16))
# 2) 결과 읽기
v4l2-ctl -d $DEV -C dma_reg_read_ch0
# 출력 예: dma_reg_read_ch0: 805309014 (= 0x30000A56)

# 방법 2: 셸 스크립트 사용 (16진수 출력)
./cam_dma_read.sh 0 0x3000
# [cam_dma_read.sh] ch0 /dev/v4l-subdev2 reg=0x3000 val=0x0a56 (raw=0x30000a56)
```

### 3.5 DMA 센서 레지스터 쓰기

```bash
# 방법 1: v4l2-ctl 직접 사용
# test pattern = 0x0001 쓰기 (0x3070 << 16 | 0x0001)
v4l2-ctl -d $DEV -c dma_reg_write_ch0=$((0x30700001))

# 방법 2: 셸 스크립트 사용
./cam_dma_write.sh 0 0x3070 0x0001

# readback 확인
./cam_dma_read.sh 0 0x3070
# val=0x0001 이면 PASS

# 원복
./cam_dma_write.sh 0 0x3070 0x0000
```

### 3.6 MCP4018 가변저항

```bash
# 최소 저항 (0Ω)
v4l2-ctl -d $DEV -c mcp4018_wiper_ch0=0

# 최대 저항 (50kΩ)
v4l2-ctl -d $DEV -c mcp4018_wiper_ch0=127

# 중간값 (기본, ~25kΩ)
v4l2-ctl -d $DEV -c mcp4018_wiper_ch0=63
```

---

## 4. DMA 검증 절차

### 4.1 기본 검증 (chip ID)

```bash
./cam_dma_read.sh 0 0x3000
# 기대값: val=0x0a56

./cam_dma_read.sh 1 0x3000
./cam_dma_read.sh 2 0x3000
./cam_dma_read.sh 3 0x3000
```

### 4.2 Write/Readback 검증

```bash
# test pattern 쓰기
./cam_dma_write.sh 0 0x3070 0x0001
./cam_dma_read.sh 0 0x3070
# val=0x0001 → PASS

# 원복
./cam_dma_write.sh 0 0x3070 0x0000
./cam_dma_read.sh 0 0x3070
# val=0x0000 → PASS
```

### 4.3 LED Flash 검증

```bash
./cam_dma_write.sh 0 0x3270 0x0103
./cam_dma_read.sh 0 0x3270
# val=0x0103 → PASS

./cam_dma_write.sh 0 0x3270 0x0000
./cam_dma_read.sh 0 0x3270
# val=0x0000 → PASS
```

---

## 5. 셸 스크립트

| 스크립트 | 위치 | 용도 |
|----------|------|------|
| `cam_dma_read.sh` | pim-package-org/dist/pim/opt/pim/bin/ | V4L2 DMA 레지스터 읽기 |
| `cam_dma_write.sh` | 같은 위치 | V4L2 DMA 레지스터 쓰기 |
| `cam_ap1302_dma_verify.sh` | 같은 위치 | I2C 직접 DMA read/write (교차 검증용) |
| `cam_ar0234_led_flash_read.sh` | 같은 위치 | I2C 직접 LED flash 읽기 |
| `cam_ar0234_led_flash_write.sh` | 같은 위치 | I2C 직접 LED flash 쓰기 |

---

## 6. 주의사항

- DMA 읽기/쓰기는 카메라 스트리밍 중에도 사용할 수 있다.
- DMA 접근은 AP1302 ISP 동작에 영향을 주지 않는다.
- LED Flash 레지스터(0x3270)는 상태 비트가 추가로 반영될 수 있다. 검증 시 `mask=0x01ff` 기준으로 비교한다.
- MCP4018 가변저항은 MAX9295 MFP4 GPIO가 HIGH일 때만 동작한다 (VCC 공급).
- 기존 SIPM 방식은 제거되었다. AR0234 센서 레지스터 접근은 DMA만 사용한다.

---

## 7. 초기화 로그 해석

`max9296_init_controls()` (max9296.c:2459~2468)는 V4L2 컨트롤 등록 직후 **디폴트 값**을 커널 로그에 남긴다. 이 로그는 사용자 공간에서 아직 값을 쓰지 않은 "갓 초기화된" 상태를 나타낸다.

### 7.1 로그 예시

```
max9296_init_controls (gain_ch0:256 awb_ch0:1 sat_ch0:4096 hue:0 con_ch0:0 hflip_ch0:0 vflip_ch0:0 light_freq:1)
```

### 7.2 필드별 의미

| 필드 | 값 | V4L2 CID | 범위 (min~max, def) | 의미 |
|------|----|----------|---------------------|------|
| `gain_ch0` | 256 | `V4L2_CID_GAIN_CH0` | 0~65535, def=256 | CH0 수동 Gain (Q8 고정소수점, 256 = 1.0x) |
| `awb_ch0` | 1 | `V4L2_CID_AUTO_WHITE_BALANCE_CH0` | 0~1, def=1 | CH0 Auto White Balance **ON** |
| `sat_ch0` | 4096 | `V4L2_CID_SATURATION_CH0` | 0~65535, def=4096 | CH0 채도 (fixed12, 4096 = 1.0 중립) |
| `hue` | 0 | `V4L2_CID_HUE` | 0~359, def=0 | 색상 회전 0° (채널 공용) |
| `con_ch0` | 0 | `V4L2_CID_CONTRAST_CH0` | 0~65535, def=0 | CH0 대비 (보정 없음) |
| `hflip_ch0` | 0 | `V4L2_CID_HFLIP_CH0` | 0~1, def=0 | 수평 뒤집기 OFF |
| `vflip_ch0` | 0 | `V4L2_CID_VFLIP_CH0` | 0~1, def=0 | 수직 뒤집기 OFF |
| `light_freq` | 1 | `V4L2_CID_POWER_LINE_FREQUENCY` | 메뉴, def=1 | 플리커 억제 **50Hz** (국내 전원, 채널 공용) |

### 7.3 해석 포인트

- **모두 디폴트 값** — 로그 시점에 사용자 공간이 어떤 컨트롤에도 값을 쓰지 않았음을 의미. `v4l2-ctl --set-ctrl=...`이 먼저 호출되지 않은 fresh 상태.
- **실동작 설정**은 `awb_ch0=1` (AWB ON)과 `light_freq=50Hz` 두 개뿐. 나머지는 중립/OFF.
- **고정소수점 주의**: `gain`은 Q8 (256=1.0x), `sat`/`con`/`lsc`/`brightness`는 fixed12 (4096=1.0). 배율 오해로 잘못된 값을 쓰지 않도록 주의.
- `hue`/`light_freq`는 **채널 공용 컨트롤**이므로 `_ch0`/`_ch1` 접미사가 없다.
- `con_ch0=0`과 `sat_ch0=4096`은 둘 다 "보정 없음"이지만 디폴트 숫자가 다르다 — 컨트롤 테이블(max9296.c:2261-2269)의 def 값을 그대로 반영한 것.
- 같은 라인이 CH1도 동일하게 찍혀야 정상. 한쪽만 로그에 없다면 해당 채널 v4l2_ctrl 등록 실패 가능성.
