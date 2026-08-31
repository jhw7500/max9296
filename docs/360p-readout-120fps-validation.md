# 640x360 readout / crop / 120 FPS 보드 검증

검증일: 2026-08-28  
대상: `pim-camera-v016` (`192.168.214.4`, Linux 5.10.35)  
판정: **production은 AP1302 `KEEP` readout + 640x360 출력 + 30 FPS**

## 1. 최종 판정

- 640x360은 AP1302와 CSI 출력 해상도로 정상 동작한다. dual ch0/ch1 출력은
  `1280x360 RGBP`, 카메라별 경계는 640 pixel이다.
- 별도 HD/FHD 직접 비교에서 HD의 AR0234 active window도 FHD와 같은
  1920x1080, `X/Y_ODD_INC=1`이었다. 현재 HD는 native 1280x720 sensor readout이
  아니라 FHD 전체 readout을 AP1302에서 1280x720으로 축소하는 경로다.
- 기본 `KEEP`은 AR0234의 1920x1080 window를 유지하고 AP1302에서 640x360으로
  축소한다. 따라서 640x360 출력이 곧 센서 640x360 readout을 뜻하지 않는다.
- KEEP에 120 FPS를 요청했을 때 AP/CSI/ISI가 약 113~115 FPS여서 엄격 기준
  118.8 FPS를 통과하지 못했다. caps의 120 표시는 지원 근거가 아니다.
- AP1302 firmware의 `SENSOR0_CONF_0..7` pointer table을 보드에서 읽은 결과 실제
  readout mapping은 mode 0/1/2만 존재한다. mode 3~5는 null이고 6/7은 공식 문서상
  readout mode가 아니라 deselect/select event다. 따라서 과거 mode 3~15 sweep의
  동일 readback은 독립 profile이 아니라 unmapped/fallback 동작이다.
- mode 1/2는 1920x1200 full-array window에 2행/2열 skip+binning을 적용한 약
  960x600 상당 profile이다. mode 1의 640x360@120 host 출력은 KEEP과 같은 약
  113 FPS로 성능 이득이 없었고, mode 2는 AR0234 monochrome bit를 켜
  640x360@120 color pipeline의 CSI/ISI 출력이 0 FPS가 됐다. 둘 다 production
  후보가 아니다.
- qualification 중단 조건을 적용해 3-case 30/60/120 resource matrix는 실행하지
  않았다. production 기본은 `MAX9296_360P_SENSOR_MODE=KEEP`,
  `MAX9296_360P_MAX_FPS=30`이다. 연구용 candidate builder만 120을 명시한다.

## 2. 소스와 산출물

### 드라이버 revision

| commit | 내용 |
|---|---|
| `6c0989e` | 360p preview 정책 테스트 |
| `7c0d782` | AP1302 640x360 preview context |
| `1108e57` | `crop_enable`과 false 무쓰기 |
| `9606b62` | 노출 사전 거부 |
| `12d590a` | 기존 cached-control replay best-effort 복구 |
| `675c0d6` | 후보 빌더의 artifact 보존 회귀 수정 |
| `de9908f` | AR0234 odd-increment readback 추가 |
| `d15d613` | production 30 FPS 판정, 엄격 FPS 계측, 최종 문서/증적 |

gstApp revision은 `b9f4767`, `741b61a`, `895d1f2`다. 각각 edgeconf parsing,
crop tuple의 prepare 전 적용, 640x360 prepare target을 담당한다.

production clean build 결과는 driver `max9296.ko` SHA-256
`b27ae021fe4cb569ed6264712fabebb2a6b2cb6f5ab27278aebdb4113e09fc33`,
`srcversion=DA89ABE8A6E147911293CE6`이고, gstApp SHA-256는
`08ae95d148300d5ef25999a4c801287c6613bee738b80c6fe63dae297ba239ee`다.

qualification module 17개는 driver `675c0d64f176f1cc10bb56b271f80a5c4e8c7794`
clean revision에서 만들었다. 보드 경로는 다음과 같다.

```text
/root/camtest/max9296-360p-run-20260828T120000Z/candidates-675c0d6/
```

`manifest.tsv` 17행과 KEEP/sm00~sm15의 SHA-256를 호스트와 보드에서 모두
검증했다. 후보 빌드 도중 source-tree 아래의 이전 `.ko`가 `make clean`으로
삭제되던 문제는 mock external-module clean 회귀 테스트로 재현한 뒤 외부 staging
directory를 사용하도록 수정했다.

원본 복구 backup은 다음 경로에 있으며 생성 직후 원본과 SHA-256를 대조했다.

```text
/root/camtest/max9296-360p-backup-20260828T115744Z
```

원본 hash:

| 대상 | SHA-256 |
|---|---|
| 기존 `max9296.ko` | `138d6418...` |
| 기존 gstApp | `c17e1780...` |
| 기존 edgeconf | `b075884b...` |

## 3. 요구사항별 소스 확인

### 3.1 노출 `0x500c`

[AP1302 Register Reference](<AND9230-D (AP1302 RR)_PointImage.pdf>)는
`R0x500C`를 32-bit `AE_MANUAL_EXP_TIME`, 즉 manual exposure time으로 정의한다.
드라이버의 `exp_time`, `exp_time_chN`, cached prepare replay는 모두
`max9296_write_exposure()`를 거쳐 이 주소를 쓴다.

모드 구조에는 일반 `max_fps`와 별도로 `exposure_safe_max_fps=30`이 있다.
노출 preflight는 I2C 전에 실행되며 거부 로그에 channel, mode, FPS, 요청 노출값,
안전 상한을 넣는다. qualification module의 31~120 FPS에서 manual exposure는
`-EBUSY`, 잘못된 mode/FPS는 `-EINVAL`이다. AE auto는 high-FPS에서 초기 exposure
seed만 생략하고 나머지 AE/gain/AWB/flip 제어를 유지한다.

MCP4018이 없는 ch0의 operational `-ENXIO`는 기존처럼 로그만 남기고 prepare를
실패시키지 않는다. 노출 정책 위반만 첫 I2C 전에 반환한다. source와 테스트에는
SoC 정지 이력이 있는 manual-WB `0x510A` 쓰기가 없고 AWB는 `0x5100`만 사용한다.

### 3.2 digital crop과 중심

[AP1302 Register Reference](<AND9230-D (AP1302 RR)_PointImage.pdf>) 근거:

| 의미 | register | 기본값/형식 |
|---|---:|---|
| target digital zoom factor | `0x1010` | `0x0100`, s7.8, 1.00x |
| smooth zoom step | `0x1012` | `0x7fff`; 구현은 즉시 적용 `0x8000` |
| optical zoom current factor | `0x1014` | 중심 좌표가 아니므로 미사용 |
| digital zoom center X | `0x118c` | `0x0080`, s7.8, 화면 중앙 |
| digital zoom center Y | `0x118e` | `0x0080`, s7.8, 화면 중앙 |

사용자 ABI는 `dz=100..300`, `dz_x/dz_y=0..65535`이며 `dz=150`은 1.5배,
`dz=200`은 2배다. 배율은 dual 센서 timing 비대칭을 막기 위해 CSI domain 공통이고,
중심은 채널별이다. `crop_enable=false`는 cache만 갱신하고 네 crop register를 쓰지
않는다. `true`에서는 step→X→Y→factor 순서이며 factor가 마지막이다. streaming 중
factor/center tuple은 변경 가능하지만 enable 전환은 `-EBUSY`다.

해상도와 crop은 독립이다. FHD/HD/360p 어느 출력에서도 crop을 사용할 수 있지만
crop은 출력 크기를 640x360으로 바꾸는 기능이 아니다.

## 4. 녹색 화면 분석

활성 capture node `/dev/video4`의 실제 format은 dual `1280x360 RGBP`,
bytesperline 2560, sizeimage 921600이었다. 같은 921600-byte raw를 UYVY로 해석하면
관측된 녹색/자홍색 화면이 그대로 재현됐고, RGB565 little-endian으로 해석하면
정상적인 어두운 영상이었다.

따라서 녹색 화면 원인은 센서/ISP 색 손상이 아니라 **RGBP를 UYVY로 해석한 소비자
format mismatch**다. gstApp RTSP의 두 채널은 모두 640x360으로 정상 decode됐다.
YUYV direct capture는 STREAMON 후 dqbuf가 없어 CSI를 wedging했으므로 재시도하지
않았고 hard reset으로 복구했다.

## 5. KEEP 120 FPS 실측

설정은 640x360, dual 1280x360, crop false, dz=100, 양 채널 AE auto였다. manual
exposure seed는 정책대로 생략됐다. AP preview `MAX_FPS=120`, AR0234 frame length는
약 1172~1179 lines로 이론상 약 125 FPS였지만 host 출력은 다음과 같았다.

| channel | sensor | AP HINF | CSI2 | ISI | 엄격 120 |
|---|---:|---:|---:|---:|---|
| ch0 | frame-time 표본 일부가 1/2 rate로 흔들려 평균 제외 | 114.6 | 114.4 | 114.5 | FAIL |
| ch1 | 118.9 | 114.6 | 114.0 | 114.1 | FAIL |

8-bit HINF counter는 120 FPS에서 2초 interval과 I2C 지연을 합치면 wrap ambiguity가
있어 최종 판정은 1초 interval 결과를 사용했다. pass 조건은 양 채널에서 모든
sensor/AP HINF 표본이 유효하고 sensor/AP/CSI/ISI 각각 `>=118.8 FPS`, loss
`<=1%`, 신뢰 가능한 ISI, 이미지/transport error 없음이다.

resource 비교:

| 설정 | gstApp RSS | system CPU | 온도 | 비고 |
|---|---:|---:|---|---|
| KEEP 30 | 40,644 KiB | 18.8% | CPU/SOC 약 55~57/56~57 C | 안정 |
| KEEP 요청 120 | 37,460 KiB | 30.6% | CPU/SOC 약 56~57/59~60 C | 실제 113~115 FPS |

DDR PMU는 이 보드 커널에서 지원되지 않아 수치를 만들지 않았다. dmesg에는 측정
구간의 overflow/CRC/ECC/lost/timeout이 없었다.

요약값을 재현할 수 있는 원시 출력은
`artifacts/board-20260828-qualification/keep-120fps/`에 보존했다.

| evidence | SHA-256 |
|---|---|
| `fhd-readout-ap1302-640x360-120-i1-fps.txt` | `b23f3b99cb100ff1c783689d22a5473ede4b33cf70858f3941d5ed5464371b51` |
| `fhd-readout-ap1302-640x360-120-resource.txt` (media format, thermal, DDR capability, dmesg delta 포함) | `78a661df5edea9edc2637783ed3c0136704e007316abeb8dcb9f24f13bf13243` |
| `gstapp-640x360-30-fps.txt` | `626353f44c27e174d1329ab471004b4162e34d33694c826ebdd5aa4bc97abd93` |
| `gstapp-640x360-30-resource.txt` | `72d91a6f57d97c060be62cbd85dae108ea9a0676a6d99371c3ba0bafbbaadaee` |

## 6. sensor-mode 후보 스윕과 firmware mapping 정정

공통 조건은 640x360@30, crop false, dz=100, 동일 gstApp/edgeconf다. 각 후보마다
module hash를 확인하고 hard reset→prepare→stream-start→deep readback을 두 번
실행했다. 16개 후보 모두 두 사이클에서 `CONSUMED`, `errno=0`, `match=1`이었다.

| 후보 | AP `SENSOR_MODE` | firmware mapping | AR window / sampling | 해석 |
|---|---|---|---|---|
| KEEP | `0x0000` 유지 | 현재 context 유지 | 1920x1080, odd-inc 1/1 | FHD readout + AP 축소 |
| sm00 | `0x2000` | `CONF_0=0xAECC` | 1920x1200, odd-inc 1/1 | firmware mode 0 full-array 계열 |
| sm01 | `0x2001` | `CONF_1=0xAF10` | 1920x1200, odd-inc 3/3, `READ_MODE=0x3020` | 약 960x600 color subsampling |
| sm02 | `0x2002` | `CONF_2=0xAF90` | 1920x1200, odd-inc 3/3, `READ_MODE=0x3020` | mode 1 계열 + monochrome bit |
| sm03~sm15 | `0x2003..0x200f` | readout mapping 없음 | sweep에서 mode 0과 같은 값 관측 | 독립 profile로 해석 금지 |

ch0 `READ_MODE`에는 edgeconf의 hflip 때문에 `0x4000`이 추가된다. sm01/sm02의
`0x3020`은 [AR0234CS Register Reference](<AND9812-D (AR0234CS RR)=R2_PointImage.pdf>)의
`COL_BIN(bit13)`, `ROW_BIN(bit12)`, `COL_SUM(bit5)`이며 `X/Y_ODD_INC=3`은 공식
정의상 skip 2다. 두 채널과 두 hard-reset cycle 모두 같은 값이었다.

sm02 raw는 `1280x360x2 = 921600` bytes였고 SHA-256는
`1dc11439a07c5093e960cd4692ce263da76a1b41079efb7985a056852eb37c73`이다.
정상 RGB565 decode와 RTSP에는 녹색 dominance가 없었다. 다만 ch0은 완전 흑색,
ch1은 거의 흑색의 점 하나뿐이라 full-FOV를 비교할 target/조도가 없었다.

| image | SHA-256 | signal |
|---|---|---|
| sm02 RTSP ch0 | `ef8400399cbb84bdec02b43395c873daf7c06084742b5404a3ab3b45aec17708` | Y=16, U/V=128/128 |
| sm02 RTSP ch1 | `8424d83171c654d451b4ba9e1fcb8604560353ff31d7c56ff8394f9412765d06` | Yavg≈16.02, U/V=128/128 |
| final KEEP RTSP ch0 | `ef8400399cbb84bdec02b43395c873daf7c06084742b5404a3ab3b45aec17708` | 완전 흑색 |
| final KEEP RTSP ch1 | `e9869a1a269e0c19df1e0e96c97f79c64449e4d3118b8154c8b84175b72f41ce` | RGB 평균 `(0.005, 1.533, 17.482)`, 어두운 청색 장면 |

window/read-mode만으로 full-FOV를 주장할 수 없고 세 요구 profile도 없으므로 matrix를
중단했다. sm01/sm02는 upstream sensor sample workload를 KEEP보다 줄일 가능성이
있지만, MIPI/ISI/gstApp은 둘 다 640x360 출력이므로 workload 차이는 주로 AP1302
입력 이전에 나타난다. FOV/화질/resource가 검증되지 않은 값을 production mode로
추정하지 않는다.

후속 firmware table 확인으로 위 sweep의 의미를 정정했다. AP1302 Register
Reference의 `SENSOR0_CONF_0..7(0x60B0..0x60BE)`는 mode 0~5와
deselect/select event의 sensor setting table pointer다. 양 AP1302에서 읽은 값은
다음과 같았다.

```text
AECC AF10 AF90 0000 0000 0000 0000 A330
```

즉 mode 0/1/2만 실제 table이 있고 mode 3~5는 null이다. 공식 event 정의상
index 6/7은 deselect/select이므로 mode 6/7을 readout 후보로 세는 것도 잘못이다.
index 8~15는 이 mapping table의 범위 밖이다. `0xAF10` table은 AR0234
`0x3040=0x3020`, `0x30A2=0x0003`, `0x30A6=0x0003`을 설정하고 `0x30B0`
bit 7을 clear한다. `0xAF90`은 같은 sampling에 bit 7을 set한다. AR0234 Register
Reference에서 이 bit는 `MONO_CHROME_OPERATION`이다.

## 7. production 배포 상태와 절차

최종 production 설정:

```json
{
  "cam_width": 640,
  "cam_height": 360,
  "fps": 30,
  "i2c2": {
    "crop_enable": false,
    "dz": 100,
    "ch0": { "dz_x": 32768, "dz_y": 32768 },
    "ch1": { "dz_x": 32768, "dz_y": 32768 }
  }
}
```

설치 경로:

```text
/lib/modules/5.10.35-lts-5.10.y+g2fce14defc04/kernel/drivers/media/i2c/max9296.ko
/usr/local/bin/gstApp
/root/shared_v/edgeconf_pim.json
```

mode, enable, firmware epoch을 바꾼 뒤에는 gstApp restart만 사용하지 말고 다음을
실행한다.

```bash
/root/camtest/cam_hard_reset.sh -s -S
```

실제 FPS 확인:

```bash
/root/camtest/max9296-360p-run-20260828T120000Z/cam_fps_stack.sh \
  -c ch01 -d 20 -i 2 -D -L KEEP-PRODUCTION-30 -R 30
```

runtime crop tuple 예:

```bash
v4l2-ctl -d /dev/v4l-subdev2 \
  --set-ctrl=dz=150,dz_x_ch0=32768,dz_y_ch0=49152,dz_x_ch1=32768,dz_y_ch1=16384
```

이 명령은 `crop_enable=true`로 준비된 스트림에서만 live apply된다. streaming 중
`crop_enable` 전환은 `-EBUSY`가 정상이다. enable을 바꾸려면 stream을 멈추고
edgeconf를 갱신한 뒤 hard reset한다.

rollback은 backup의 module/gstApp/edgeconf를 원래 경로에 복원하고 `depmod -a`,
hard reset을 실행한 뒤 세 원본 SHA-256를 대조한다.

### 7.1 최종 production smoke

`d15d613` 기준 clean build를 설치하고 crop 시험을 끝낸 뒤 production JSON으로 다시
hard reset했다. 2026-08-28T15:51:42Z에 읽은 최종 상태는 다음과 같다.

| 항목 | 결과 |
|---|---|
| driver | SHA-256 `b27ae021...e09fc33`, version 2.9, `srcversion=DA89ABE8A6E147911293CE6` |
| gstApp | SHA-256 `08ae95d1...239ee`, `gstApp -d 11 -m 4`, service active |
| edgeconf | SHA-256 `eba52154...53654f`, 640x360@30, crop false, dz=100 |
| prepare | `CONSUMED`, dual `1280x360`, `errno=0`, `worker_errno=0`, `match=1` |
| AP1302 readback | ch0/ch1 모두 factor `0x0100`, step `0x8000`, X/Y `0x0080` |
| 8초 FPS 평균 | ch0 sensor/AP/CSI/ISI `30.0/29.9/29.5/29.8`, ch1 `30.0/30.0/29.3/29.5` |
| final probe log | crop register 쓰기 0회, `0x510A` 쓰기 0회, transport error keyword 0회 |

production 모듈에서 120 FPS를 요청한 `VIDIOC_SUBDEV_S_FRAME_INTERVAL`은
`-EINVAL`로 실패했고 active interval은 30 FPS로 유지됐다. `crop_enable=false`와
동일한 no-op 제어는 스트리밍 중 성공했으며 실제 enable 전환만 `-EBUSY`다.

crop true는 FHD(dual 3840x1080), HD(dual 2560x720), 360p(dual 1280x360)에서 각각
검증했다. 세 경우 모두 초기 1.5배/채널별 중심과 runtime 2.0배/새 중심이 AP1302
readback에 반영됐고, 출력 해상도는 바뀌지 않았다. enable 전환은 세 경우 모두
`-EBUSY`였으며 서비스는 active를 유지했다.

최종 RTSP H.265를 GStreamer로 decode한 PNG는 ch0가 완전 흑색, ch1의 RGB 평균이
`(0.005, 1.533, 17.482)`인 어두운 청색 장면이었다. 녹색 dominance는 없었다.
원시 출력과 frame은 다음 디렉터리에 보존한다.

```text
artifacts/board-20260828-qualification/final-production-d15d613/
```

| evidence | SHA-256 |
|---|---|
| `final-deployment-status.txt` | `d603da74ecd3e2c9a4718f8163303a43ac6e207f0df9f6c8035d6c6b7619feb9` |
| `final-production-30-fps.txt` | `ffe714af287034868db54c4c523f022204b9b140c7ef5fcce8ed402d6f87a17e` |
| `crop-fhd-runtime.txt` | `eeb1a2761696a1e3bf8624de25a81201e64f6db712fe4f8c8967875c5b255da3` |
| `crop-hd-runtime.txt` | `8c8d052385f37e4820bf459829fda58b5b7ece00fe1c7edca2ec35c3bc079074` |
| `crop-360p-runtime.txt` | `f882997dae79bea8b5e597adde85986625c30f4eeebfbc1f095bd439f8694a83` |
| `rtsp-ch0.png` | `ef8400399cbb84bdec02b43395c873daf7c06084742b5404a3ab3b45aec17708` |
| `rtsp-ch1.png` | `e9869a1a269e0c19df1e0e96c97f79c64449e4d3118b8154c8b84175b72f41ce` |

### 7.2 HD와 FHD sensor readout 직접 비교

2026-08-28T17:34~17:37Z에 같은 production module/gstApp, 30 FPS,
`crop_enable=false`, `dz=100` 조건에서 edgeconf만 HD와 FHD로 바꾸고 각 경우마다
hard reset했다. `cam_fps_stack.sh -D`로 AP1302 preview context와 AP1302 DMA를 통한
AR0234 레지스터를 두 채널에서 읽었다.

| 항목 | HD | FHD |
|---|---|---|
| 카메라당 AP/CSI 출력 | 1280x720 | 1920x1080 |
| AP `SENSOR_MODE` / ROI | `0x0000`, 전체 ROI | `0x0000`, 전체 ROI |
| AR X window | `4..1923` = 1920 pixels | `4..1923` = 1920 pixels |
| AR Y window | `64..1143` = 1080 lines | `64..1143` = 1080 lines |
| `X/Y_ODD_INC` | `1/1` | `1/1` |
| ch0/ch1 base `READ_MODE` | `0x4000` / `0x0000` | `0x4000` / `0x0000` |
| `LINE_LENGTH_PCK` | `0x042c` = 1068 | `0x093c` = 2364 |
| `FRAME_LENGTH_LINES` | ch0/ch1 `2777/2779` | ch0/ch1 `1252/1252` |
| 계산 sensor FPS | `30.35/30.32` | `30.41/30.41` |

active window와 sampling increment가 같으므로 **HD도 AR0234에서 FHD 전체
1920x1080을 읽고 AP1302가 1280x720으로 축소한다.** HD와 FHD는 blanking/timing
조합만 다르다. HD ch0의 total timing은 `1068 x 2777 = 2,965,836` pixel clocks,
FHD는 `2364 x 1252 = 2,959,728` pixel clocks로 거의 같다. 따라서 현재 HD는 센서
active-pixel 및 AP1302 입력 workload를 1280x720 비율로 줄이지 않는다. 반면 AP1302
출력 이후 MIPI/GMSL/CSI/ISI와 앱 입력 workload는 실제 1280x720로 감소한다.

원시 증적:

| evidence | SHA-256 |
|---|---|
| `hd-vs-fhd-readout-8a5bbe4/hd-readout.txt` | `c8e6adf6238b070e9c4522b5bd335a0c9aa1037451de390f8bd1f4b42d73f65a` |
| `hd-vs-fhd-readout-8a5bbe4/fhd-readout.txt` | `17c9db95728e847ff8e175f89779c43382a25c7be1c8b42f46d2e9e881a2f458` |

시험 후 original edgeconf SHA-256
`eba521544a39d0a8ab79786e1d5b7a7c06357942a5d94c61691531960e53654f`를 복원하고
hard reset했다. 최종 상태는 dual `1280x360`, 30 FPS, crop false,
`CONSUMED/match=1`, gstApp stream-start였으며 보드 점유를 해제했다.

## 8. 재시험에 필요한 조건

전용 readout 비교를 재개하려면 다음이 필요하다.

1. 고정된 16:9 chart 또는 rail 검사 대상과 충분한 조도
2. full-FOV 기준 KEEP frame
3. 현재 firmware에 없는 640x360/1280x720 full-FOV vendor profile 또는 새 bootdata
4. 선택 후보마다 30/60/120 세 번의 hard-reset 반복
5. raw RGBP와 RTSP image, AP/AR timing, CSI/ISI, CPU/RSS/DDR/온도 동시 수집

이 조건 전에는 mode number, central crop, `dz=300`을 sensor readout의 대체 근거로
사용하지 않는다.

현재 firmware의 sensor-mode table은 확보했으므로 더 이상 mode 번호를 임의로
sweep할 필요가 없다. 다음 시험은 vendor가 생성한 coherent AP1302 bootdata에서
16:9 color profile, AP1302 output timing과 MIPI 설정이 함께 제공될 때 재개한다.

## 9. issue #41 firmware/readout 120 FPS 비교

### 9.1 시험 구성

2026-08-29 KST에 driver `809d582`에서 빌드한 exact KEEP/SM01/SM02 module만
사용했다. 공통 조건은 dual ch0/ch1, 카메라당 640x360, 요청 120 FPS,
`crop_enable=false`, `dz=100`, 양 채널 AE auto다. 각 case는 module/JSON 설치,
`depmod`, hard reset, 20초 `cam_fps_stack.sh`, 20초 resource 측정 순서로 실행했다.

재현 runner `tools/run_360p_readout_compare.sh`는 시험 전 module과 edgeconf를 새로
백업하고, 정상/실패 어느 경우에도 EXIT trap에서 원본 설치, `depmod`, hard reset,
hash와 시험 전 active/inactive service 상태의 exact 복구 확인까지 수행한다.
AR0234 `0x30B0` DMA read는 부가 진단이며 core FPS/resource 측정 뒤 실행한다. 도구가
없거나 실행 불가이면 `SKIP`, 실행 중 오류이면 `FAIL`을 기록하되 core 비교는
계속한다. 첫 실행에서 이 부가 read가 `DMA_CTRL=0x0032`에 머문 것을 필수 실패로
처리했던 문제는, 기존 계측 도구의 optional 정책과 맞춰 non-fatal로 수정하고
failure-injection 회귀 테스트를 추가했다.

### 9.2 FPS 결과

| case | sensor ch0/ch1 | AP HINF ch0/ch1 | CSI ch0/ch1 | ISI ch0/ch1 | 판정 |
|---|---:|---:|---:|---:|---|
| KEEP | 113.3 / 119.3 | 113.8 / 113.8 | 113.6 / 113.3 | 113.7 / 113.3 | FAIL, 약 113 FPS |
| SM01 | 113.2 / 118.9 | 113.7 / 113.8 | 113.2 / 112.8 | 113.2 / 112.8 | FAIL, KEEP 대비 이득 없음 |
| SM02 | 107.9 / 61.4 | 106.7 / 0.0 | 0.0 / 0.0 | 0.0 / 0.0 | FAIL, video output 없음 |

KEEP ch0 sensor 평균에는 AP1302 `R0x00FC`의 알려진 half-rate 표본이 섞였다.
production 판정은 두 채널에서 일관된 AP/CSI/ISI 약 113 FPS를 기준으로 한다.
SM01의 AR0234 timing은 ch0/ch1 frame length 1217/1181 lines, 이론값
120.84/124.52 FPS였지만 AP 이후 출력은 KEEP과 같았다. 따라서 sensor sample 수를
줄여도 현재 병목 또는 AP1302 output pacing은 개선되지 않는다.

SM02의 AR window와 odd increment는 SM01과 같지만 `0x30B0=0x00A8`로
`MONO_CHROME_OPERATION` bit가 켜졌다. KEEP/SM01은 `0x0028`이다. 현재 RGB color
firmware/transport 구성에서 SM02는 CSI와 ISI가 모두 0 FPS이므로 CPU 감소값을
성능 개선으로 해석할 수 없다.

### 9.3 resource와 transport

| case | gstApp RSS before→after | process ticks | system CPU | CPU/SOC 온도 | 유효성 |
|---|---:|---:|---:|---|---|
| KEEP | 34,900→37,840 KiB | 1,237 | 30.3% | 58/60→57/59 °C | 유효 |
| SM01 | 35,180→35,708 KiB | 1,249 | 30.6% | 58/60→57/59 °C | 유효, 차이 없음 |
| SM02 | 31,672→31,672 KiB | 2 | 13.5% | 56/59→56/58 °C | 0 FPS라 비교 무효 |

세 case 모두 측정 구간 dmesg의 overflow/CRC/ECC/lost-frame/timeout/green keyword는
0이었다. gstApp이 `/dev/video4`를 점유해 resource tool의 별도 `v4l2-ctl` format
조회는 `EBUSY`였으므로 해당 줄은 unknown이다. 같은 실행의 FPS tool과 media graph는
active dual `1280x360@1/120`, AP context 카메라당 640x360을 독립적으로 기록했다.

### 9.4 복구와 결론

runner 종료 결과는 다음과 같다.

```text
RESTORE_RESULT module=PASS edgeconf=PASS reset=PASS service=active
```

독립 readback에서도 module SHA-256
`b27ae021fe4cb569ed6264712fabebb2a6b2cb6f5ab27278aebdb4113e09fc33`,
edgeconf SHA-256
`eba521544a39d0a8ab79786e1d5b7a7c06357942a5d94c61691531960e53654f`,
service active, `SENSOR_MODE=0x0000`을 확인했다. 최종 설정은 640x360@30,
crop false, dz=100이다.

원시 증적은
`artifacts/board-20260828-qualification/issue41-readout-809d582/run2/`에 보존한다.
핵심 파일 hash는 다음과 같다.

| evidence | SHA-256 |
|---|---|
| `keep-fps.txt` | `c2857deddc53977bcf43bb656bcc5ff19f00566f0c46b735f8b2d0a38cc5979f` |
| `sm01-fps.txt` | `a2a556786d500934a5aeaf9b0547af73f67b2a823132076df59c22943aa9f316` |
| `sm02-fps.txt` | `2b1decf6372e86aaa49957c2b1c1f76af66b5795caae4659ac6cb09f2c62ad76` |
| `keep-resource.txt` | `610208c1cdba840d226e92287589937aaf8d759800761ddd6688c50dac33ce4d` |
| `sm01-resource.txt` | `56a6693d3795e64b9e2134a1eeef7ed2b02edf3674471c4ddbeaaee614d206b9` |
| `sm02-resource.txt` | `d185651a60843adf6fe1ed24cd1b2b6591e58c954bcc756e015abb93376a0afb` |

현재 firmware로는 native/exact 640x360 sensor readout과 엄격 120 FPS를 달성하지
못했다. production은 KEEP + AP1302 scale @30을 유지하며, 다음 단계는 host에서
AR0234 register를 덮어쓰는 것이 아니라 vendor AP1302 bootdata에 16:9 color
subsample profile과 일관된 output/MIPI timing을 함께 추가하는 것이다.
