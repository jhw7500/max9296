# AR0234 LED flash delay / exposure / FPS board test

시험일: 2026-09-01  
대상 보드: `pim-camera-v016` (`192.168.214.4`)  
패키지: `pim-mp 0.6.3+jhw.camera5`  
드라이버: MAX9296 `2.11`  
파이프라인: gstApp `-d 5 -m 4`, dual `1280x360@120`
(카메라당 `640x360@120`), crop disabled

## 시간축 시각화

- 정적 PNG:
  [/home/jhw/ai/opencode/projects/max9296/artifacts/board-20260901-led-flash-delay/fps-exptime-delay-timeline.png](/home/jhw/ai/opencode/projects/max9296/artifacts/board-20260901-led-flash-delay/fps-exptime-delay-timeline.png)
- 브라우저용 독립 실행 HTML:
  [/home/jhw/ai/opencode/projects/max9296/artifacts/board-20260901-led-flash-delay/fps-exptime-delay-timeline.html](/home/jhw/ai/opencode/projects/max9296/artifacts/board-20260901-led-flash-delay/fps-exptime-delay-timeline.html)

인라인 visualization을 지원하지 않는 클라이언트에서는 PNG를 확인하거나 HTML을
일반 브라우저로 연다.

## 결론

- AR0234 `LED_FLASH_CONTROL(0x3270)`의 delay byte는 unsigned 시간값이 아니라
  **8-bit two's-complement** 값이다. 따라서 raw `128(0x80)`은 `+128`이 아니라
  signed `-128`, 즉 현재 타이밍에서 integration start보다 약 `435.2 us` 앞선다.
- `exp_time=8000 us`에서 delay `0` 또는 raw `32`(약 `+108.8 us` lag)는
  센서/ISP 약 `118~120 FPS`, CSI 약 `117~118 FPS`를 유지했다.
- 같은 노출에서 음수 lead를 늘리면 CSI 전달률이 급격히 낮아졌다. raw `224`
  (`-32`)는 약 `115 FPS`, raw `192`(`-64`)는 약 `65 FPS`, raw `160`
  (`-96`)은 약 `57 FPS`, raw `128`(`-128`)은 약 `47 FPS`였다.
- raw `128` 고정 시 `5000/7000 us`는 큰 구간 손실이 없었지만 `7500 us`부터
  장기 측정 CSI 평균이 약 `111 FPS`로 낮아졌고, `7800/8000 us`에서는 각각
  약 `61/47 FPS`로 붕괴했다.
- 따라서 `640x360@120`에서 LED flash를 사용할 때 기본 delay는 `0`이 안전하다.
  raw `128`을 반드시 써야 한다면 이번 보드에서는 `7000 us` 이하만 정상 범위로
  관찰됐지만, 이를 범용 상한으로 확정하지 않는다. 센서 편차·온도·펌웨어를 포함한
  추가 qualification 전에는 여유를 두어야 한다.
- 정상 조합도 측정 도구의 엄격 기준(모든 계층 `>=118.8 FPS`, 손실 `<=1%`)은
  일부 채널에서 통과하지 못했다. 따라서 이 결과는 delay 영향의 A/B 증적이며
  `120 FPS production qualification 통과`를 의미하지 않는다.

## 데이터시트 근거와 환산

onsemi AR0234CS Register Reference `AND9812-D`, PDF page 41은 다음을 정의한다.

- `R0x3270[8]`: `LED_FLASH_EN`
- `R0x3270[7:0]`: `LED_DELAY`
- 단위: `row_time / 2`
- 표현: two's-complement
- bit 7이 0이면 integration start보다 lag, bit 7이 1이면 lead

근거 문서:
[/home/jhw/ai/opencode/projects/max9296/docs/AND9812-D (AR0234CS RR)=R2_PointImage.pdf](</home/jhw/ai/opencode/projects/max9296/docs/AND9812-D (AR0234CS RR)=R2_PointImage.pdf>)

시험 중 AR0234 readback은 `LINE_LENGTH_PCK=612`, pixel clock `90 MHz`였다.
따라서 row time은 `612 / 90 MHz = 6.8 us`, delay 1 step은 `3.4 us`다.

```text
signed_delay = raw < 128 ? raw : raw - 256
delay_us     = signed_delay * 3.4 us
```

| raw | signed | 의미 |
|---:|---:|---|
| 0 | 0 | 0 us |
| 32 | +32 | 약 108.8 us lag |
| 224 | -32 | 약 108.8 us lead |
| 192 | -64 | 약 217.6 us lead |
| 160 | -96 | 약 326.4 us lead |
| 128 | -128 | 약 435.2 us lead |

데이터시트는 delay의 단위와 부호를 정의하지만 `exp_time + delay`에 따른 FPS
보장식이나 AP1302 dual packing 제약은 제시하지 않는다. 아래 임계값은 보드 실측값이다.

## 실측 결과

### exp_time 8000 us, delay sweep

ch0만 LED flash enable, ch1은 disable 상태다. 표의 값은 `ch0/ch1` 순서이며,
ISI IRQ는 저하 조합에서 신뢰 불가로 판정되어 FPS 해석에서 제외했다.

| raw delay | 방향·크기 | Sensor FPS | ISP FPS | CSI FPS | 판정 |
|---:|---|---:|---:|---:|---|
| 0 | 0 us | 119.5 / 119.1 | 119.0 / 118.9 | 118.5 / 117.9 | 구간 손실 없음 |
| 32 | +108.8 us lag | 119.5 / 119.4 | 118.5 / 118.4 | 118.1 / 117.0 | 구간 손실 없음 |
| 224 | 108.8 us lead | 113.7 / 119.7 | 118.8 / 118.8 | 115.6 / 114.6 | 경계/저하 시작 |
| 192 | 217.6 us lead | 119.5 / 118.4 | 113.5 / 113.0 | 65.0 / 64.4 | 약 43% 구간 손실 |
| 160 | 326.4 us lead | 122.2 / 112.8 | 113.3 / 113.6 | 57.3 / 56.9 | 약 50% 구간 손실 |
| 128 | 435.2 us lead | 105.7 / 119.3 | 104.6 / 112.3 | 47.2 / 47.0 | 약 55~58% 구간 손실 |

짧은 AP1302 sensor-FPS counter 샘플은 일부 구간에서 비정상 값이 섞였으므로,
저하 판정은 ISP와 CSI의 지속 평균 및 AR0234 timing readback을 함께 사용했다.

### raw delay 128, exposure sweep

| exp_time | 실제 AR0234 노출 | CSI FPS(ch0/ch1) | 판정 |
|---:|---:|---:|---|
| 5000 us | 약 4991 us | 118.3 / 117.7 | 구간 손실 없음 |
| 7000 us | 약 6997 us | 117.5 / 116.2 | 구간 손실 없음 |
| 7500 us | 약 7494 us | 111.7 / 111.3 (20초) | 120 FPS 유지 실패 |
| 7800 us | 약 7793 us | 61.5 / 61.0 | 심각한 저하 |
| 8000 us | 약 7990~7997 us | 47.2 / 47.0 | 심각한 저하 |

`8000 us + raw 128`에서 ch0 `FRAME_LENGTH_LINES`가 정상 약 `1173`에서
`1394`로 늘어 이론 센서 속도가 약 `105.5 FPS`가 된 시점이 관찰됐다. 그러나
CSI는 이보다 더 낮은 약 `47 FPS`였고, 비활성 flash 채널도 영향을 받았다.

저하 중 MAX9296 `CTRL3(0x0013)`은 bus 2에서 `0xfa`였다. 이는 splitter mode,
`LOCKED=1`, `ERROR=0`, `CMU_LOCKED=1`이다. 따라서 물리 GMSL unlock이 직접 원인은
아니며, 한 센서의 timing 변화가 AP1302 dual 출력/CSI 수락 타이밍을 흐트러뜨린
것으로 추정한다. 정확한 내부 원인은 vendor timing 문서 또는 추가 계측이 필요하다.

## 시험 방법과 범위

- 최초 JSON으로 `5000 us / delay 0`을 기동한 뒤, 안정된 동일 gstApp 스트림에서
  `/dev/v4l-subdev2`의 `exp_time`, `led_flash_ch0` V4L2 control만 변경했다.
- 각 조합 전에 delay 0으로 복귀시켜 이전 저하 상태를 제거했다.
- `cam_ar0234_led_flash_read.sh`로 `0x3270`을 readback하고,
  `cam_fps_stack.sh -D`로 AP1302/AR0234/CSI 값을 교차 확인했다.
- MCP4018 wiper write는 이 보드에서 `-ENXIO`로 실패했다. 따라서 본 시험은
  AR0234 flash timing output과 영상 FPS의 관계를 검증한 것이며, 실제 LED 광량이나
  광 펄스 파형까지 검증한 것은 아니다.

## 시작 안정성 별도 관찰

120 FPS 시험 시작과 원래 30 FPS 복구 모두 cold start 직후 gstApp이 두 차례
`pipeline NOT prerolled`로 실패하고 watchdog의 세 번째 실행에서 stream-start했다.
이는 runtime A/B 결과와 분리된 startup 문제다. 현재 `gstApp -d 5`와 전원/링크
준비 시퀀스의 추가 검토가 필요하다.

## 종료 상태

- 운영 edgeconf SHA-256:
  `87f9bbb1910f1dd385aef96496d03e5a94feace9ac3acb7bc2197e2d6400ad03`
- 운영 설정: dual, 카메라당 `640x360@30`, common `exp_time=2000`
- 최종 12초 smoke:
  - ch0: sensor `30.0`, ISP `29.9`, CSI `29.7`, ISI `29.8 FPS`
  - ch1: sensor `30.0`, ISP `29.9`, CSI `29.5`, ISI `29.7 FPS`
- `cam-operate.service=active`, gstApp 안정 실행 확인

원시 결과는
[/home/jhw/ai/opencode/projects/max9296/artifacts/board-20260901-led-flash-delay/raw/](/home/jhw/ai/opencode/projects/max9296/artifacts/board-20260901-led-flash-delay/raw/)에
보존했다. 최초 cold-start 실패 결과
[/home/jhw/ai/opencode/projects/max9296/artifacts/board-20260901-led-flash-delay/raw/e5000-d0-fps.txt](/home/jhw/ai/opencode/projects/max9296/artifacts/board-20260901-led-flash-delay/raw/e5000-d0-fps.txt)와
[/home/jhw/ai/opencode/projects/max9296/artifacts/board-20260901-led-flash-delay/raw/final-restored-30-fps.txt](/home/jhw/ai/opencode/projects/max9296/artifacts/board-20260901-led-flash-delay/raw/final-restored-30-fps.txt)는
유효 A/B 결과에서 제외하고, 각각 이후 안정화 측정과
[/home/jhw/ai/opencode/projects/max9296/artifacts/board-20260901-led-flash-delay/raw/final-restored-30-stable-fps.txt](/home/jhw/ai/opencode/projects/max9296/artifacts/board-20260901-led-flash-delay/raw/final-restored-30-stable-fps.txt)를
사용했다.
