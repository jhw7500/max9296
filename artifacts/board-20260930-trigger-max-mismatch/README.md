# AP1302 `TRIGGER_MAX_MISMATCH` / `PREVIEW_MAX_FPS` — epoch 간 복원 측정

시험일: 2026-09-30
대상 보드: `192.168.214.4` (i.MX8MP, MAX9296 4ch)
커널: `5.10.35-lts-5.10.y+g2fce14defc04`
패키지: `pim-mp 0.6.3+jhw.camera7`
드라이버: MAX9296 `2.12`, srcversion `00FEDDF6CFD9B1C1F2F0E6B` (소스 `f6230c5`)
펌웨어: `/lib/firmware/v4l-ap1302-ar0234.fw` (81,616 bytes)
관련: 이슈 #91 (#85 후속), PR #90

## 무엇을 물었나

`max9296_program_preview_context_channel()` 은 출력이 `640x360` 이고 rate 가 30 을
넘을 때만 `0x2020 PREVIEW_MAX_FPS` 와 `0x6112 TRIGGER_MAX_MISMATCH=0` 을 쓴다.
술어가 거짓이 되었을 때 기본값을 되돌려 쓰는 코드는 없다.

한 board-power epoch 안에서는 그 교차가 하드웨어에 도달할 수 없다 — 프로그램된
천장이 술어와 일대일로 대응하므로 교차 요청은 레지스터를 만지기 전에 `-ESTALE` 로
거부된다 (`tests/max9296_360p_policy_test.c` 가 전 모드 × 전 rate 로 고정).

**측정 대상은 epoch 을 건너는 경우다.** 고fps epoch 뒤 펌웨어가 다시 로드되면 두
레지스터가 어떤 값을 갖는가. 복원되지 않으면 저fps 세션이 고fps 전용 튜닝을 물려받은
채 돈다.

## 절차와 결과

읽기: `i2ctransfer -f -y -a <bus> w2@<addr> 0x61 0x12 r2`
대상: AP1302 4개 — 호스트 adapter `1`·`2` × `0x11`(ch0)·`0x12`(ch1)

| 단계 | 상태 | 술어 | `0x6112` | `0x2020` |
|---|---|---|---|---|
| 기준선 (손대기 전, 라이브 스트림, epoch 2, `2560x720@15`) | `CONSUMED` | 거짓 | `0x0014` | `0x1e00` |
| Phase 1 — 병렬 prepare `1280x360@120 enable=3` (출력 `640x360@120`) | `READY` epoch 2 | **참** | **`0x0000`** | `0x7800` |
| epoch 교차 — 양쪽 lease 취소 (전력 카운트 → 0) | `IDLE` epoch 3 | — | **주소 응답 없음** | 주소 응답 없음 |
| Phase 2 — 새 epoch, 펌웨어 재로드, 병렬 prepare `2560x720@30 enable=3` (출력 `1280x720@30`) | `READY` epoch 4 | 거짓 | **`0x0014`** | **`0x1e00`** |

4개 AP1302 가 모든 단계에서 같은 값을 보였다.
`0x7800` = u8.8 의 120.0, `0x1e00` = 30.0, `0x0014` = 20.

## 대조 — 왜 이 값들이 구별 가능한가

| 대조 | 관측 | 배제하는 대안 설명 |
|---|---|---|
| `0x0000` CHIP_VERSION | `0x0265` (4개 전부) | 읽기가 칩에 도달하지 않은 채 그럴듯한 숫자가 나온 것 |
| Phase 1 의 `0x0000` | 드라이버 쓰기가 관측된다 | `0x0014` 가 애초에 0 이 된 적이 없어 그대로인 것 |
| dmesg `preview addr=0x11/0x12 output=640x360 fps=120` | 술어가 참인 분기를 실제로 탔다 | 고fps 를 요청했으나 다른 분기로 간 것 |
| 교차 구간의 `No such device or address` (4개 전부) | AP1302 가 전원을 잃었다 | epoch 카운터만 오르고 칩은 계속 켜져 있던 것 |
| dmesg `loaded v4l-ap1302-ar0234.fw firmware (81616 bytes)` (`I2C:1`·`I2C:2`) | 펌웨어가 다시 로드됐다 | 새 epoch 이지만 재로드가 생략된 것 |

Phase 1 이 없으면 Phase 2 의 `0x0014` 는 "복원됐다"와 "0 이 된 적 없다"를 구별하지 못한다.

## 결론

1. **재로드는 `0x6112` 를 `0x0014`(20µs) 로, `0x2020` 을 `0x1e00`(30.0) 으로 복원한다.**
   드라이버에 되돌리기 쓰기는 필요하지 않다.
2. `20`µs 기본값이 하드웨어에서 측정됐다. `docs/fps-limit-analysis.md` 는 이 값을
   기본값으로 기록해 왔고, 이제 리드백 근거가 있다.
3. 드라이버에는 여전히 이 두 레지스터의 리드백이 없다. 위 값은 호스트 i2c 어댑터에서
   읽은 것이다.

부수 관측: prepare 상태의 epoch 이 전원을 내릴 때 `2 → 3`, 올릴 때 `3 → 4` 로 올랐다 —
epoch 이 전력 카운트의 **어느 방향 0 교차에서도** 오른다는 서술과 일치한다.

## 이 측정이 주장하지 않는 것

- 보드 1대, 펌웨어 `v4l-ap1302-ar0234.fw`(81,616 bytes) 1종의 결과다.
- epoch 교차를 **lease 취소로 전력 카운트를 0 으로 만들어** 얻었다. `cam_hard_reset.sh`
  같은 물리 리셋 경로는 따로 측정하지 않았다. 둘 다 새 board-power epoch 이지만 같은
  경로는 아니다.
- `0x6112` 의 내부 비트 의미는 데이터시트로 확인하지 않았다. 확인된 것은 기본값과
  `0` 이 프레임 스킵을 억제한다는 기존 실측(`docs/fps-limit-analysis.md`)이다.
- 전달 프레임레이트는 이 측정의 대상이 아니다.

## 부록 — 실행한 명령과 출력

`artifacts/board-*/raw/` 는 이 저장소 관례상 추적하지 않으므로(`.gitignore:38`) 증거를
여기에 둔다. 파일로 캡처한 로그가 아니라 **세션 출력을 전사한 것**이다. 모든 명령은
호스트에서 `ssh root@192.168.214.4` 로 실행했다.

읽기 함수:

```sh
rd() { i2ctransfer -f -y -a "$1" "w2@$2" "$3" "$4" r2; }
```

### 기준선 — 손대기 전, 라이브 스트림

```
state=CONSUMED generation=17747831707155854 epoch=2 mode=dual-wide table=dual width=2560 height=720 fps=15 code=0x2006 enable=3 crop_enable=0 errno=0 worker_errno=0 lease=0 match=1   (1-0048)
state=CONSUMED generation=17747831707155854 epoch=2 mode=dual-wide table=dual width=2560 height=720 fps=15 code=0x2006 enable=3 crop_enable=0 errno=0 worker_errno=0 lease=0 match=1   (2-0048)

bus1/bus2 × 0x11/0x12/0x3c:  0x0000 = 0x02 0x65    0x6112 = 0x00 0x14    0x2020 = 0x1e 0x00
```

`0x3c` 는 듀얼 모드에서 브로드캐스트이므로 이후 단계는 `0x11`·`0x12` 만 읽었다.

### Phase 1 — 술어 참

```
$ gen=$(date +%s)
$ printf "1 %s 1280 360 120 3\n" "$gen" > /sys/bus/i2c/devices/1-0048/prepare &
$ printf "1 %s 1280 360 120 3\n" "$gen" > /sys/bus/i2c/devices/2-0048/prepare &
$ wait                                     # rc: bus1=0 bus2=0

state=READY generation=1790734620 epoch=2 mode=dual-wide table=dual width=1280 height=360 fps=120 code=0x2006 enable=3 crop_enable=0 errno=0 worker_errno=0 lease=1 match=1   (양쪽 동일)

bus1/bus2 × 0x11/0x12:  0x6112 = 0x00 0x00    0x2020 = 0x78 0x00

[771034.695681] [I2C:1][max9296.c:5131] preview addr=0x11 output=640x360 fps=120 sensor_mode=KEEP
[771034.699669] [I2C:2][max9296.c:5131] preview addr=0x11 output=640x360 fps=120 sensor_mode=KEEP
[771034.703253] [I2C:1][max9296.c:5131] preview addr=0x12 output=640x360 fps=120 sensor_mode=KEEP
[771034.704234] [I2C:2][max9296.c:5131] preview addr=0x12 output=640x360 fps=120 sensor_mode=KEEP
```

### epoch 교차 — lease 취소로 전력 카운트를 0 으로

```
$ printf "0\n" > /sys/bus/i2c/devices/1-0048/prepare
$ printf "0\n" > /sys/bus/i2c/devices/2-0048/prepare

state=IDLE ... epoch=3 ... lease=0 match=0                (양쪽 동일)

bus1/bus2 × 0x11/0x12:  Error: Sending messages failed: No such device or address
```

### Phase 2 — 새 epoch, 펌웨어 재로드, 술어 거짓

```
$ gen=$(date +%s)
$ printf "1 %s 2560 720 30 3\n" "$gen" > /sys/bus/i2c/devices/1-0048/prepare &
$ printf "1 %s 2560 720 30 3\n" "$gen" > /sys/bus/i2c/devices/2-0048/prepare &
$ wait                                     # rc: bus1=0 bus2=0

state=READY generation=1790734679 epoch=4 mode=dual-wide table=dual width=2560 height=720 fps=30 code=0x2006 enable=3 crop_enable=0 errno=0 worker_errno=0 lease=1 match=1   (양쪽 동일)

bus1/bus2 × 0x11/0x12:  0x6112 = 0x00 0x14    0x2020 = 0x1e 0x00

[771093.805582] [I2C:2][max9296.c:4736] loaded v4l-ap1302-ar0234.fw firmware (81616 bytes)
[771093.817601] [I2C:1][max9296.c:4736] loaded v4l-ap1302-ar0234.fw firmware (81616 bytes)
[771093.805593] [I2C:2][max9296.c:5131] preview addr=0x11 output=1280x720 fps=30 sensor_mode=KEEP
[771093.815491] [I2C:2][max9296.c:5131] preview addr=0x12 output=1280x720 fps=30 sensor_mode=KEEP
[771093.817608] [I2C:1][max9296.c:5131] preview addr=0x11 output=1280x720 fps=30 sensor_mode=KEEP
[771093.821286] [I2C:1][max9296.c:5131] preview addr=0x12 output=1280x720 fps=30 sensor_mode=KEEP
```

### 복구

```
$ printf "0\n" > /sys/bus/i2c/devices/{1,2}-0048/prepare
$ systemctl start cam-operate.service      # rc=0

active
/dev/video3:  root  3112365 F.... gstApp
/dev/video4:  root  3112365 F.... gstApp
state=CONSUMED ... epoch=2 ... width=2560 height=720 fps=15 ... lease=0 match=1   (양쪽 동일)
```

AP1302 HINF 프레임 카운터(`0x3c` `0x0002`)를 5초 간격으로 두 번 읽어 프레임 흐름을 확인했다:
`bus1=0xc801 bus2=0xc901` → `bus1=0x1401 bus2=0x1401`.

### 드라이버 교체 기록

이 측정 직전에 드라이버를 `c25f7a68` 빌드(srcversion `38670DF208FF25E7BC1C29F`)에서
`f6230c5` 빌드로 교체했다. 되돌리기용 백업:
`/lib/modules/5.10.35-lts-5.10.y+g2fce14defc04/kernel/drivers/media/i2c/max9296.ko.bak-c25f7a68`,
`/opt/pim/driver/max9296.ko.bak-c25f7a68`.
