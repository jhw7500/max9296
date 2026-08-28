## 2026-08-29 firmware table 및 640x360@120 실측 업데이트

`809d582` 기준 exact KEEP/SM01/SM02 candidate를 PIM 보드에서 비교했습니다.
공통 조건은 dual ch0/ch1, 카메라당 640x360, 요청 120 FPS,
`crop_enable=false`, `dz=100`, 양 채널 AE auto, case별 hard reset입니다.

### firmware에서 확인한 실제 sensor mode

양 AP1302의 `SENSOR0_CONF_0..7(0x60B0..0x60BE)` readback은 동일했습니다.

```text
AECC AF10 AF90 0000 0000 0000 0000 A330
```

AP1302 Register Reference의 event 정의에 따르면 `CONF_0..5`가 mode 0..5,
`CONF_6/7`이 deselect/select입니다. 따라서 현재 firmware에 실제 readout table이
있는 mode는 **0, 1, 2뿐**입니다. 이전 mode 3~15 sweep에서 관측한 동일 register
상태는 별도 profile이 아니라 unmapped/fallback 동작으로 정정합니다.

- mode 1 table: AR0234 `READ_MODE=0x3020`, `X/Y_ODD_INC=3`, `0x30B0` bit 7 clear
- mode 2 table: 같은 sampling, `0x30B0` bit 7 set
- AR0234 문서상 `0x30B0[7]`은 `MONO_CHROME_OPERATION`

동적 readback도 KEEP/SM01은 `0x30B0=0x0028`, SM02는 `0x00A8`로 일치했습니다.

### 20초 FPS 비교

| case | sensor ch0/ch1 | AP HINF ch0/ch1 | CSI ch0/ch1 | ISI ch0/ch1 | 결과 |
|---|---:|---:|---:|---:|---|
| KEEP | 113.3 / 119.3 | 113.8 / 113.8 | 113.6 / 113.3 | 113.7 / 113.3 | 120 기준 FAIL |
| SM01 | 113.2 / 118.9 | 113.7 / 113.8 | 113.2 / 112.8 | 113.2 / 112.8 | KEEP 대비 이득 없음 |
| SM02 | 107.9 / 61.4 | 106.7 / 0.0 | 0.0 / 0.0 | 0.0 / 0.0 | color pipeline 출력 없음 |

SM01은 1920x1200 full-array window에 2x skip/bin/sum을 적용한 약 960x600
sensor input이지만 AP/CSI/ISI와 system CPU는 KEEP과 사실상 같습니다
(KEEP 30.3%, SM01 30.6%). 즉 sensor sample 수 감소가 현재 host output 병목을
해소하지 못했습니다. SM02의 CPU 13.5%는 CSI/ISI 0 FPS 상태이므로 비교값으로
사용할 수 없습니다.

세 case 모두 측정 구간의 overflow/CRC/ECC/lost-frame/timeout/green keyword는
0이었습니다. 시험 runner는 실패 포함 모든 종료 경로에서 module/edgeconf 복구,
`depmod`, hard reset, hash/service 확인을 수행하도록 회귀 테스트했습니다.

```text
RESTORE_RESULT module=PASS edgeconf=PASS reset=PASS service=active
```

최종 보드는 production module
`b27ae021fe4cb569ed6264712fabebb2a6b2cb6f5ab27278aebdb4113e09fc33`,
production edgeconf
`eba521544a39d0a8ab79786e1d5b7a7c06357942a5d94c61691531960e53654f`,
640x360@30, crop false, `SENSOR_MODE=0x0000`, service active 상태입니다.

### 판정 및 남은 작업

- 현재 bootdata에는 native/exact 640x360 또는 full-FOV 16:9 subsampling mode가 없음
- SM01은 color-compatible하지만 120 FPS/CPU 이득이 없어 production 후보 아님
- SM02는 monochrome 설정이고 640x360@120 color pipeline 출력이 없어 후보 아님
- production은 `KEEP + AP1302 scale @30 FPS` 유지
- 다음 단계는 host의 direct AR0234 write가 아니라 vendor AP1302 bootdata에
  16:9 color sampling profile과 일관된 AP output/MIPI timing을 함께 추가하는 것

이 이슈는 vendor profile 확보 및 새 bootdata 검증을 위해 open 상태로 유지합니다.
