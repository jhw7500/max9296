# 640x360 readout / crop / 120 FPS 보드 검증

검증일: 2026-08-28  
대상: `pim-camera-v016` (`192.168.214.4`, Linux 5.10.35)  
판정: **production은 AP1302 `KEEP` readout + 640x360 출력 + 30 FPS**

## 1. 최종 판정

- 640x360은 AP1302와 CSI 출력 해상도로 정상 동작한다. dual ch0/ch1 출력은
  `1280x360 RGBP`, 카메라별 경계는 640 pixel이다.
- 기본 `KEEP`은 AR0234의 1920x1080 window를 유지하고 AP1302에서 640x360으로
  축소한다. 따라서 640x360 출력이 곧 센서 640x360 readout을 뜻하지 않는다.
- KEEP에 120 FPS를 요청했을 때 AP/CSI/ISI가 약 113~115 FPS여서 엄격 기준
  118.8 FPS를 통과하지 못했다. caps의 120 표시는 지원 근거가 아니다.
- AP1302 sensor-mode 0~15를 각각 두 번 hard reset해 측정했지만 full-FOV
  `SENSOR-640`, `HD-ISP`, `FHD-ISP` 세 프로필을 식별하지 못했다.
- mode 1/2는 1920x1200 window에 2행/2열 skip+binning을 적용한 약 960x600
  상당 profile이었다. 전용 640x360 readout이 아니고, 암흑 장면 때문에 FOV도
  입증하지 못했으므로 production 후보로 선택하지 않았다.
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

## 6. sensor-mode 후보 스윕

공통 조건은 640x360@30, crop false, dz=100, 동일 gstApp/edgeconf다. 각 후보마다
module hash를 확인하고 hard reset→prepare→stream-start→deep readback을 두 번
실행했다. 16개 후보 모두 두 사이클에서 `CONSUMED`, `errno=0`, `match=1`이었다.

| 후보 | AP `SENSOR_MODE` | AR window | odd inc | ch1 base `READ_MODE` | 해석 |
|---|---|---|---|---|---|
| KEEP | `0x0000` 유지 | 1920x1080 (`4..1923`, `64..1143`) | 1/1 | `0x0000` | FHD readout + AP 축소 |
| sm00, sm03~sm15 | `0x2000`, `0x2003..0x200f` | 1920x1200 (`4..1923`, `4..1203`) | 1/1 | `0x0000` | full-array 계열 + AP 변환 |
| sm01, sm02 | `0x2001`, `0x2002` | 1920x1200 | 3/3 | `0x3020` | 약 960x600 상당 2x skip/bin/sum |

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

## 8. 재시험에 필요한 조건

전용 readout 비교를 재개하려면 다음이 필요하다.

1. 고정된 16:9 chart 또는 rail 검사 대상과 충분한 조도
2. full-FOV 기준 KEEP frame
3. AP1302 firmware sensor-mode table 또는 640x360/1280x720 full-FOV profile
4. 선택 후보마다 30/60/120 세 번의 hard-reset 반복
5. raw RGBP와 RTSP image, AP/AR timing, CSI/ISI, CPU/RSS/DDR/온도 동시 수집

이 조건 전에는 mode number, central crop, `dz=300`을 sensor readout의 대체 근거로
사용하지 않는다.
