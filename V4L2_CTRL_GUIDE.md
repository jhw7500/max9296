# V4L2 Control 사용 가이드 (PIM Camera / max9296 + AP1302)

기존 i2ctransfer 기반 스크립트(`cam_*.sh`)는 그대로 유지하고, 런타임 튜닝은 V4L2 컨트롤(`v4l2-ctl`)로 대체하는 것을 목표로 한다.

이 문서는 다음을 제공한다.
- 어떤 `/dev/v4l-subdevX` 노드가 어떤 채널(ch0~ch3)을 의미하는지
- 각 V4L2 컨트롤이 어떤 AP1302 레지스터에 매핑되는지
- fixed-point(고정점) 값의 스케일(256=1.0, 4096=1.0)과 사용 예제
- 운영 환경에서 안전하게 적용하는 절차(cam-operate/재시작 스크립트와의 관계)

## 1) 장치 노드/채널 매핑 규칙

이 플랫폼의 전역 채널 정의는 이미 운영 스크립트/JSON에서 고정되어 있다.

- `ch0/ch1` = i2c2 쪽 카메라
- `ch2/ch3` = i2c1 쪽 카메라

V4L2 subdev 노드도 이 전역 채널과 정합되게 사용한다.

- `/dev/v4l-subdev2` : (전역) ch0/ch1
- `/dev/v4l-subdev3` : (전역) ch2/ch3

참고: 드라이버는 `/dev/v4l-subdev3`에서 커스텀 컨트롤 이름을 `*_ch2/_ch3`로 표시되도록 맞춰 혼란을 줄였다.

배포 패키지(pim-package)에는 동일 문서가 `/opt/pim/docs/V4L2_CTRL_GUIDE.md`로 포함된다.

## 2) 값 표현(고정점) 규칙

V4L2 컨트롤은 기본적으로 정수값으로 노출된다. 드라이버는 하드웨어 레지스터 값을 스케일링하지 않고 그대로 전달하는 것이 원칙이며, 사용자가 스케일을 이해하고 값을 넣는다.

- Gain: `ufixed8` (u16)
  - 스케일: `256 = 1.0x`, `512 = 2.0x`
- Brightness/Contrast/Saturation/LSC: `fixed12` (u16)
  - 스케일: `4096 = 1.0`, `6144 = 1.5`, `8192 = 2.0`
- Exposure: `unsigned 32-bit`
  - V4L2는 s32 범위를 쓰므로 드라이버는 `0..INT_MAX` 범위를 허용한다.

보드 기본값(참고: `--list-ctrls` 기준)
- `brightness_chX`: default=0
- `contrast_chX`: default=0
- `saturation_chX`: default=4096
- `gain_chX`: default=256
- `lsc_chX`: default=16383(0x3fff)

참고
- V4L2 컨트롤(`contrast_chX`, `saturation_chX` 등)은 `v4l2-ctl --list-ctrls` 출력에 레지스터 주소를 직접 표시할 수 없다.
- 레지스터 주소는 아래 "컨트롤 ↔ 레지스터 매핑" 섹션을 기준으로 본다.

## 3) 컨트롤 ↔ 레지스터 매핑 (요약)

드라이버 소스 기준: `projects/max9296/max9296.c`

공통 컨트롤
- `ext_time` -> `0x500c` (u32, 듀얼 모드에서는 양 채널에 동일 적용)

커스텀 컨트롤(채널별)
- `/dev/v4l-subdev2`: `*_ch0`, `*_ch1`
- `/dev/v4l-subdev3`: `*_ch2`, `*_ch3`

예:
- `gain_chX` -> `0x5006` (u16, ufixed8)
- `ext_time_chX` -> `0x500c` (u32)
- `ae_on_chX` -> `0x5002` (1=auto, 0=manual)
- `auto_white_balance_chX` -> `0x5100` (0/1)
- `auto_gain_chX` -> `0x5002` (0/1, 현재 드라이버는 AE 모드에서 암묵 처리)
- `hflip_chX` / `vflip_chX` -> `0x100c`
- `brightness_chX` -> `0x7000` (u16, fixed12)
- `contrast_chX` -> `0x7002` (u16, fixed12)
- `saturation_chX` -> `0x7006` (u16, fixed12)
- `lsc_chX` -> `0x54a0` (u16, fixed12)

## 3.5) FPS 제어 (Frame Sync)

### FSYNC 기반 프레임 레이트 제어

이 드라이버는 AP1302 펌웨어에서 직접 FPS를 설정하지 않고, **GPIO를 통한 FSYNC(Frame Sync) 신호**로 프레임 레이트를 제어한다.

#### 동작 원리

1. **FPS 설정** (V4L2 Frame Interval)
   ```bash
   v4l2-ctl -d /dev/v4l-subdev2 --set-parm=30  # 30 FPS
   v4l2-ctl -d /dev/v4l-subdev2 --set-parm=15  # 15 FPS
   v4l2-ctl -d /dev/v4l-subdev2 --set-parm=60  # 60 FPS
   ```

2. **FSYNC 신호 생성** (`max9296_fsync` 커널 스레드)
   - GPIO를 통해 주기적인 펄스 신호 생성
   - 주기 = `1,000,000 / fps` (마이크로초)
   - 펄스 폭: HIGH 1ms, LOW (주기 - 1ms)

3. **타이밍 예시**
   ```
   30 FPS:  HIGH 1.0ms + LOW 32.3ms = 33.3ms 주기
   15 FPS:  HIGH 1.0ms + LOW 65.7ms = 66.7ms 주기
   60 FPS:  HIGH 1.0ms + LOW 15.7ms = 16.7ms 주기
   120 FPS: HIGH 1.0ms + LOW 7.3ms  = 8.3ms 주기
   ```

4. **AP1302 동작**
   - FSYNC 신호의 rising edge마다 새 프레임 캡처 시작
   - 펌웨어가 센서(AR0234)를 FSYNC에 동기화

#### 지원 FPS 범위

- **최소**: 1 FPS
- **최대**: 120 FPS (센서 스펙 기준)
- **권장**: 15, 30, 60 FPS

#### 코드 위치

- FSYNC 스레드: `max9296.c:2373-2502` (`max9296_fsync`)
- FPS 저장: `max9296.c:1949` (`max9296_s_frame_interval`)
- GPIO 초기화: `max9296.c:2834-2842`
- 스레드 시작: `max9296.c:2971`

#### FPS 확인

```bash
# 현재 FPS 확인
v4l2-ctl -d /dev/v4l-subdev2 --get-parm

# 출력 예:
# Streaming Parameters Video Capture:
#   Frames per second: 30.000 (30/1)
```

#### 주의사항

- FPS 변경은 스트리밍 시작 전에 설정하는 것을 권장
- 듀얼 채널 모드에서는 양쪽 채널이 동일한 FSYNC 신호를 공유
- FSYNC GPIO가 없으면 FPS 제어가 불가능 (Device Tree 확인 필요)

## 4) v4l2-ctl 기본 사용

### 4.0 현재값 스냅샷/원복(권장)

테스트/튜닝 중에는 flip, gain_chX, ext_time(_chX) 같은 값이 누적되어 “현재 상태가 기본값이 아닌 상태”가 되기 쉽다.
아래처럼 스냅샷을 떠두면 언제든 원복할 수 있다.

1) 스냅샷 저장

```bash
v4l2-ctl -d /dev/v4l-subdev2 --list-ctrls --list-ctrls-menus > /tmp/subdev2.ctrls.before.txt
v4l2-ctl -d /dev/v4l-subdev3 --list-ctrls --list-ctrls-menus > /tmp/subdev3.ctrls.before.txt
```

2) “기본값으로 원복” 예시(필요한 항목만)

```bash
# 채널별(커스텀) 기본값
sudo v4l2-ctl -d /dev/v4l-subdev2 --set-ctrl=brightness_ch0=0,brightness_ch1=0,contrast_ch0=0,contrast_ch1=0,saturation_ch0=4096,saturation_ch1=4096
sudo v4l2-ctl -d /dev/v4l-subdev2 --set-ctrl=gain_ch0=256,gain_ch1=256
sudo v4l2-ctl -d /dev/v4l-subdev2 --set-ctrl=hflip_ch0=0,hflip_ch1=0,vflip_ch0=0,vflip_ch1=0
sudo v4l2-ctl -d /dev/v4l-subdev2 --set-ctrl=lsc_ch0=16383,lsc_ch1=16383

sudo v4l2-ctl -d /dev/v4l-subdev3 --set-ctrl=brightness_ch2=0,brightness_ch3=0,contrast_ch2=0,contrast_ch3=0,saturation_ch2=4096,saturation_ch3=4096
sudo v4l2-ctl -d /dev/v4l-subdev3 --set-ctrl=gain_ch2=256,gain_ch3=256
sudo v4l2-ctl -d /dev/v4l-subdev3 --set-ctrl=hflip_ch2=0,hflip_ch3=0,vflip_ch2=0,vflip_ch3=0
sudo v4l2-ctl -d /dev/v4l-subdev3 --set-ctrl=lsc_ch2=16383,lsc_ch3=16383

```

3) 원복 후 확인

```bash
v4l2-ctl -d /dev/v4l-subdev2 --get-ctrl=ext_time,gain_ch0,gain_ch1,brightness_ch0,brightness_ch1,contrast_ch0,contrast_ch1,saturation_ch0,saturation_ch1,hflip_ch0,hflip_ch1,vflip_ch0,vflip_ch1,lsc_ch0,lsc_ch1
v4l2-ctl -d /dev/v4l-subdev3 --get-ctrl=ext_time,gain_ch2,gain_ch3,brightness_ch2,brightness_ch3,contrast_ch2,contrast_ch3,saturation_ch2,saturation_ch3,hflip_ch2,hflip_ch3,vflip_ch2,vflip_ch3,lsc_ch2,lsc_ch3
```

### 4.1 컨트롤 목록 확인

```bash
v4l2-ctl -d /dev/v4l-subdev2 --list-ctrls --list-ctrls-menus
v4l2-ctl -d /dev/v4l-subdev3 --list-ctrls --list-ctrls-menus
```

권장: 보드에서 결과를 파일로 남기기

```bash
v4l2-ctl -d /dev/v4l-subdev2 --list-ctrls --list-ctrls-menus > /tmp/subdev2.ctrls.txt
v4l2-ctl -d /dev/v4l-subdev3 --list-ctrls --list-ctrls-menus > /tmp/subdev3.ctrls.txt
```

확인 포인트(문서/스크립트 작성 시 기준)
- `gain_chX`, `ext_time`, `brightness_chX`, `contrast_chX`, `saturation_chX`, `lsc_chX`의 `min/max/default` 값
- 커스텀 채널 컨트롤이 실제로 보이는지
  - subdev2: `*_ch0`, `*_ch1`
  - subdev3: `*_ch2`, `*_ch3`
- `ae_on_chX` 값(1/0의 의미)

예시(참고용)
```text
User Controls

ext_time (int) : min=0 max=... step=1 default=10000 value=20000
ae_on_ch0 (bool) : default=1 value=1
gain_ch0 (int) : min=0 max=... step=1 default=256 value=256

```

### 4.2 공통(커스텀) 컨트롤 설정 예시

```bash
# ext_time: 예) 20000
sudo v4l2-ctl -d /dev/v4l-subdev2 --set-ctrl=ext_time=20000

# fixed12: contrast/saturation 1.0 (4096)
sudo v4l2-ctl -d /dev/v4l-subdev2 --set-ctrl=contrast_ch0=4096,contrast_ch1=4096,saturation_ch0=4096,saturation_ch1=4096

# brightness: default/mid-point는 list-ctrls에서 default 값을 확인 권장
sudo v4l2-ctl -d /dev/v4l-subdev2 --set-ctrl=brightness_ch0=0,brightness_ch1=0

# LSC: 1.0 근처(예: 0x3fff)
sudo v4l2-ctl -d /dev/v4l-subdev2 --set-ctrl=lsc_ch0=16383,lsc_ch1=16383
```

### 4.3 채널별(커스텀) 설정 예시

```bash
# /dev/v4l-subdev2 = ch0/ch1
sudo v4l2-ctl -d /dev/v4l-subdev2 --set-ctrl=gain_ch0=256,gain_ch1=512
sudo v4l2-ctl -d /dev/v4l-subdev2 --set-ctrl=hflip_ch0=1,hflip_ch1=0

# /dev/v4l-subdev3 = ch2/ch3
sudo v4l2-ctl -d /dev/v4l-subdev3 --set-ctrl=gain_ch2=256,gain_ch3=512
sudo v4l2-ctl -d /dev/v4l-subdev3 --set-ctrl=vflip_ch2=0,vflip_ch3=1
```

### 4.4 Auto/Manual 권장 순서

Auto가 켜져 있으면 manual 값이 즉시 덮어써질 수 있다. 실사용 권장 순서:

```bash
# AE를 manual로 (ch0/ch1)
sudo v4l2-ctl -d /dev/v4l-subdev2 --set-ctrl=ae_on_ch0=0,ae_on_ch1=0

# 이후 채널별 ext_time/gain 값을 설정(예: ch0)
sudo v4l2-ctl -d /dev/v4l-subdev2 --set-ctrl=ae_on_ch0=0,ext_time_ch0=20000,gain_ch0=512
```

채널별로 AE on/off를 하고 싶으면 `ae_on_chX` / `auto_gain_chX`를 사용한다.

## 5) 운영 절차(서비스/스크립트와 함께 안전하게 쓰기)

이 시스템은 `cam-operate`(systemd)와 여러 감시 스크립트가 카메라 앱을 자동 재시작/초기화할 수 있다.
V4L2 컨트롤을 수동으로 만질 때는 아래처럼 “자동 루프”를 잠깐 멈추는 것을 권장한다.

### 5.1 테스트/튜닝 시(권장)

```bash
# 1) 자동 운영 데몬 중지
sudo systemctl stop cam-operate

# 2) 앱/헬퍼 종료(필요시)
sudo /opt/pim/bin/kill_test.sh
```

이 상태에서 `v4l2-ctl`로 컨트롤을 바꾸고 확인한다.

### 5.2 모듈 리셋이 필요할 때

`init_cam.sh`는 카메라 앱 종료 + `rmmod/modprobe`(max9296, imx8-media-dev) + 앱 재기동까지 수행한다.

```bash
sudo /opt/pim/bin/init_cam.sh
```

## 6) 레거시 i2c 스크립트와의 관계(참고)

기존 스크립트는 그대로 유지되므로, 아래 스크립트가 실행되면 V4L2로 설정한 값이 덮일 수 있다.

- `cam_ae_on.sh` / `cam_ae_off.sh` : `0x5002` 직접 write
- `cam_manual_gain.sh` : `0x5006` 직접 write
- `cam_manual_exp_time.sh` : `0x500c` 직접 write
- `cam_rotate_setting.sh` : `0x100c` 직접 write
- `cam_ae_setting.sh` : `0x5100` 직접 write

따라서 “V4L2로 대체” 관점에서는, 앞으로 운영 경로에서 위 스크립트 호출을 줄이거나, 문서/런북에서 사용 금지(또는 디버그 전용)로 명시하는 것이 안전하다.

## 7) 아직 미대체(갭)

- `cam_manual_iso.sh`는 `0x5008`을 직접 write한다.
  - 이 레지스터의 의미/스케일이 확정되면 V4L2 컨트롤로 추가할 수 있다.
