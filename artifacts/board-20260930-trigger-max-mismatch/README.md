# AP1302 `TRIGGER_MAX_MISMATCH` / `PREVIEW_MAX_FPS` — epoch 전이 뒤 복원 측정

시험일: 2026-09-30  
대상 보드: `192.168.214.4` (i.MX8MP, MAX9296 4ch)  
커널: `5.10.35-lts-5.10.y+g2fce14defc04`  
패키지: `pim-mp 0.6.3+jhw.camera7`  
드라이버: MAX9296 `2.12`, srcversion `00FEDDF6CFD9B1C1F2F0E6B` (소스 `f6230c5`) — Phase 1·2 와 복구.
기준선만 교체 전 빌드 `38670DF208FF25E7BC1C29F`(소스 `c25f7a68`)에서 읽었다  
펌웨어: `/lib/firmware/v4l-ap1302-ar0234.fw` (81,616 bytes,
sha256 `071d38572b5039af0c69cdd43fef18ca3bb9fbe29030d4cc16b37fae6164306e`,
mtime `2026-09-18T00:08:45`) — 같은 디렉터리에 같은 크기의 변형본
`.mipi500`·`.orig` 가 함께 있으므로 파일명과 크기로는 특정되지 않는다  
관련: 이슈 #91 (#85 후속), PR #90

## 무엇을 물었나

`max9296_program_preview_context_channel()` 은 출력이 `640x360` 이고 rate 가 30 을
넘을 때만 `0x2020 PREVIEW_MAX_FPS` 와 `0x6112 TRIGGER_MAX_MISMATCH=0` 을 쓴다.
술어가 거짓이 되었을 때 기본값을 되돌려 쓰는 코드는 없다.

한 board-power epoch 안에서는 그 교차가 하드웨어에 도달할 수 없다 — 프로그램된
천장이 술어와 일대일로 대응하므로 교차 요청은 레지스터를 만지기 전에 `-ESTALE` 로
거부된다 (`tests/max9296_360p_policy_test.c` 가 전 모드 × 전 rate 로 고정).

**측정 대상은 epoch 을 건너는 경우다.** 고fps epoch 뒤 board-power epoch 전이(레일
사이클 + 리셋 + 펌웨어 재로드)를 거치면 두 레지스터가 어떤 값을 갖는가. 복원되지 않으면
저fps 세션이 고fps 전용 튜닝을 물려받은 채 돈다.

## 측정 전 상태 전이

기준선은 스트리밍 중이고 `max9296_prepare_request()` 는 `sensor->streaming` 이면
`-EBUSY` 로 거부한다(`max9296.c:5485`). 그래서 Phase 1 전에 `cam-operate.service` 를
정지하고 `imx8-media-dev`·`max9296` 를 내렸으며, 같은 기회에 드라이버를 `c25f7a68`
빌드에서 `f6230c5` 빌드로 교체했다(양쪽 사본을 `*.bak-c25f7a68` 로 백업, `depmod -a`,
설치 후 두 경로의 sha256 이 `4a002c3ecfff…` 로 일치). 다시 올린 뒤 srcversion 은
`00FEDDF6CFD9B1C1F2F0E6B`, 두 인스턴스는 `state=IDLE … epoch=1 … match=0` 이었다.

### epoch 회계

`max9296_hw_epoch` 는 모듈 적재 시 정적 초기값 1 로 시작한다(`max9296.c:1885`).
측정 구간의 이력은 `1`(적재) → `2`(Phase 1, first-on) → `3`(취소, last-off) →
`4`(Phase 2, first-on) 이다.

**Phase 1 과 Phase 2 사이에 물리적 전원 사이클이 정확히 한 번 일어났고, 그 한 사이클이
epoch 을 두 번 올린다.** `max9296_hw_epoch++` 는 두 분기가 공유하는 단일 `if (run)`
블록 안에 있어(`max9296.c:2317-2321`) 전력 카운트가 0 을 **어느 방향으로 교차해도**
오르기 때문이다. 측정 구간 전체의 상승은 Phase 1 의 `1 → 2` 를 포함해 **세 번**이다.
정지·교체는 Phase 1 이전에 끝났으므로 두 리드백 사이에 끼어들지 않는다.

**복구는 epoch 을 다시 1 로 되돌린다.** Phase 2(epoch 4) 뒤 lease 를 취소하고
`cam-operate.service` 를 기동했을 때 상태 줄은 `epoch=6` 이 아니라 `epoch=2` 를 보였다.
원인은 모듈 재적재다 — dmesg 에 두 번째 probe(`max9296 version : 2.12` / `shared Init` /
`Registered sensor subdevice`)가 커널 시각 `771165.5` 에 찍혔고, 이는 설치 probe
`770986.8`·Phase 1 `771034`·Phase 2 `771093` 보다 뒤다. **서비스 체인의 어느 컴포넌트가
재적재를 수행하는지는 확인하지 못했다** — `start_cam.sh` 에는 `rmmod`/`modprobe` 가 없다.
재적재는 디스크의 새 빌드를 다시 읽으므로 적재본은 바뀌지 않는다: 복구 직후 srcversion 이
`00FEDDF6CFD9B1C1F2F0E6B` 로 재확인됐다.

## 절차와 결과

읽기: `i2ctransfer -f -y -a <bus> w2@<addr> 0x61 0x12 r2`
대상: AP1302 4개 — 호스트 adapter `1`·`2` × `0x11`(ch0)·`0x12`(ch1)

| 단계 | 상태 | 술어 | `0x6112` | `0x2020` |
|---|---|---|---|---|
| 기준선 (손대기 전, 라이브 스트림, epoch 2, `2560x720@15`, 교체 전 빌드) | `CONSUMED` | 거짓 | `0x0014` | `0x1e00` |
| Phase 1 — 병렬 prepare `1280x360@120 enable=3` (출력 `640x360@120`) | `READY` epoch 2 | **참** | **`0x0000`** | `0x7800` |
| epoch 교차 — 양쪽 lease 취소 (전력 카운트 → 0) | `IDLE` epoch 3 | — | **주소 응답 없음** | 주소 응답 없음 |
| Phase 2 — 새 epoch, 펌웨어 재로드, 병렬 prepare `2560x720@30 enable=3` (출력 `1280x720@30`) | `READY` epoch 4 | 거짓 | **`0x0014`** | **`0x1e00`** |

4개 AP1302 가 모든 단계에서 같은 값을 보였다.
`0x7800` = u8.8 의 120.0, `0x1e00` = 30.0, `0x0014` = 20.

기준선의 `epoch 2` 와 Phase 1 의 `epoch 2` 는 **같은 번호이지만 다른 모듈 수명**이다 —
그 사이에 모듈을 내렸다 올렸고 `max9296_hw_epoch` 는 적재마다 1 에서 다시 시작한다.
비교할 수 있는 것은 번호가 아니라 위 「epoch 회계」의 이력이다.

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

1. **board-power epoch 전이를 거치면 `0x6112` 는 `0x0014`(20µs), `0x2020` 은
   `0x1e00`(30.0) 으로 돌아와 있다.** 드라이버에 되돌리기 쓰기는 필요하지 않다.
   복원을 **펌웨어 재로드 단독**에 귀속시키지는 않는다 — 아래 한계 참조.
2. `20`µs 기본값이 하드웨어에서 측정됐다. `docs/fps-limit-analysis.md` 는 이 값을
   기본값으로 기록해 왔고, 이제 리드백 근거가 있다.
3. 드라이버에는 여전히 이 두 레지스터의 리드백이 없다. 위 값은 호스트 i2c 어댑터에서
   읽은 것이다.

부수 관측: prepare 상태의 epoch 이 전원을 내릴 때 `2 → 3`, 올릴 때 `3 → 4` 로 올랐다 —
epoch 이 전력 카운트의 **어느 방향 0 교차에서도** 오른다는 서술과 일치한다.

## 이 측정이 주장하지 않는 것

- 보드 1대, 펌웨어 `v4l-ap1302-ar0234.fw`(81,616 bytes) 1종의 결과다.
- **복원의 원인을 펌웨어 재로드 단독으로 좁히지 못한다.** epoch 전이는 공유 레일을
  내렸다 올리므로(교차 구간에 AP1302 4개가 i2c 응답을 아예 멈췄다) 전원 투입 리셋
  기본값과 펌웨어가 쓴 값이 이 측정에서 구별되지 않는다. 레일을 내리지 않는 통제된
  재로드는 재지 않았다. 리셋 없는 재로드 경로 자체는 아래 「후속 측정」에서 따로 쟀다.
- epoch 교차를 **lease 취소로 전력 카운트를 0 으로 만들어** 얻었다. `cam_hard_reset.sh`
  같은 물리 리셋 경로는 따로 측정하지 않았다. 둘 다 새 board-power epoch 이지만 같은
  경로는 아니다.
- `0x6112` 의 내부 비트 의미는 데이터시트로 확인하지 않았다. 확인된 것은 기본값과
  `0` 이 프레임 스킵을 억제한다는 기존 실측(`docs/fps-limit-analysis.md`)이다.
- 전달 프레임레이트는 이 측정의 대상이 아니다.

## 후속 측정 — 리셋 없는 재로드 경로

PR #95 리뷰에서 Codex 가 제기한 경로다: peer 가 전역 전력 참조를 쥔 상태에서 한
인스턴스를 rebind 하면 `max9296_set_power()` 의 `run` 이 거짓이 되어
`max9296_set_power_on()`/`max9296_reset()` 이 호출되지 않는데, 그 인스턴스는 cold init
으로 펌웨어 재로드에 도달할 수 있다 — 그러면 저fps prepare 가 앞선 고fps 값을
물려받을 수 있다.

세 구성을 쟀다. 공통 절차: 서비스 정지 → 양쪽 병렬 prepare(술어 **거짓**인 튜플) →
**A(bus1)에만 손으로 `0x6112=0x0000`·`0x2020=0x7800` 을 심고 리드백 확인**(B 는 그대로 —
두 도메인이 독립임을 보이는 대조) → `echo 1-0048 > /sys/bus/i2c/drivers/max9296/unbind`
→ A 재bind → A 만 같은 튜플로 prepare → 읽기.

| # | 구성 | 결과 |
|---|---|---|
| 1 | dual `2560x720@30` (새 epoch) | `set_power (on users:2 skip)` → `0x40`·`0x60`·`0x48` 쓰기 실패 → `disconnect bitmask=0xc` → `errno=-6`, `FAILED`, 재로드 없음 |
| 2 | single `640x360@30 enable=1` (dual 초기화된 epoch 안) | prepare 가 `ESTALE`(-116)로 거부되어 단일채널 상태를 못 만들었다. rebind 후의 단일 테이블 시도는 `0x40`·`0x48` 실패 → `disconnect bitmask=0x4` → `errno=-6` |
| 3 | single `640x360@30 enable=1` (**모듈 재적재로 만든 새 epoch 의 첫 튜플**) | prepare `rc=0`, `mode=single table=left`. rebind 후 시도는 다시 `0x40`·`0x48` 실패 → `disconnect bitmask=0x4` → `errno=-6`, `FAILED`, 재로드 없음 |

**세 구성 모두에서 resetless rebind 는 `max9296_loadfw()` 에 도달하지 못했다.**
`loaded ... firmware` 로그가 어느 구성에도 없다. 재로드가 완료된 적이 없으므로 stale 값이
재로드를 넘겨 사용되는 경로는 관측되지 않았다.

### 실패의 근접 원인 — `bind` 이고 `unbind` 가 아니다

구성 3 에서 역직렬화기 도달성을 단계별로 쟀다:

| 시점 | A 의 MAX9296 `0x48` CTRL3 | A 의 AP1302 `0x3c` |
|---|---|---|
| 모듈 재적재 직후(전원 off) | 응답 없음 | — |
| 단일채널 prepare 후 | — | `ver=0x0265`, 심은 `0x0000` |
| **A unbind 직후** | **`0xea` 정상 응답** | **`ver=0x0265`, `0x6112=0x0000`** |
| **A 재bind 직후** | **응답 없음** | **응답 없음** |

unbind 뒤에는 역직렬화기와 AP1302 가 모두 살아 있고 심은 값도 그대로다. **둘이 함께
응답을 멈추는 시점은 `bind`** 다.

### 원인 — probe 시점의 전원 차단 (PWDN)

이 저장소의 이전 판(커밋 `077d8cb` 까지)은 실패를 *"프로그램된 인스턴스는 직렬화기가
재매핑돼 있어 모드 테이블이 갈 곳이 없다"* 로 설명했고, 그 뒤에는 *"원인 미확정"* 으로
남겼다. **둘 다 틀렸다.** PR #95 리뷰에서 Codex 가 코드로 규명했고 확인했다:

| 단계 | 근거 |
|---|---|
| probe 가 pwdn 라인을 HIGH 로 구동하며 요청 | `max9296.c:7487` — `devm_gpiod_get_optional(dev, "powerdown", GPIOD_OUT_HIGH)` |
| **논리 1 = 전원 off** | `max9296.c:2010` — `gpiod_set_value_cansleep(pwdn_gpio, enable ? 0 : 1)` |
| 전원을 되돌리는 호출은 한 곳뿐 | `max9296_reset()` 안의 `max9296_power(sensor, true)`("camera power cycle") |
| 그 함수는 `max9296_set_power_on()` 에서만 호출 | `max9296.c:2095` |
| resetless 경로는 `run=false` 로 그것을 건너뛴다 | 측정: `set_power (on users:N skip)` |
| 이 보드에 pwdn GPIO 가 실제로 있다 | `docs/imx8mp-evk.dts:576`, `:662` — `powerdown-gpios … GPIO_ACTIVE_LOW` |

**즉 `bind` 가 역직렬화기를 전원 차단하고, resetless 경로는 그것을 되살리는 유일한 호출을
건너뛴다.** 위 도달성 표와 정확히 맞는다 — unbind 뒤에는 살아 있고 bind 직후 둘이 함께
죽는다. 실패 쓰기에 `0x48` 자신이 포함되는 이유도, **직렬화기를 아예 재매핑하지 않는**
단일채널 테이블이 같은 방식으로 실패하는 이유도 이것으로 설명된다.

재매핑 가설이 틀린 직접 근거도 남긴다: 실패에 `0x48` 이 포함되고, 단일채널 테이블은
`0x40` 만 쓴다(`max9296_ser_addr()` 위 주석, `max9296.c:1092`). `reset` GPIO 가설 역시
코드가 반증한다 — `max9296_acquire_reset_gpio()` 는 `max9296_power_users > 0` 이면
`GPIOD_ASIS` 로 요청하고(`max9296.c:1947`) 이미 보유한 descriptor 는 건드리지 않는다.
원인은 `reset` 이 아니라 같은 probe 함수의 **바로 위 줄**인 `powerdown` 이었다.

### 되돌리기 쓰기 질문에 대한 답

**필요하지 않다** — 단, **`powerdown-gpios` 를 선언한 보드로 한정한다.** 근거는 위 세
측정과 PWDN 메커니즘이다: resetless rebind 는 역직렬화기가 전원 차단된 상태로 진행되므로
재로드를 완료하지 못한다. 같은 바인딩 안의 창 교차는 fingerprint 경로가 `-ESTALE` 로 이미
거부한다(#85 / PR #90, `tests/max9296_360p_policy_test.c` 전수).

**범위 조건이 중요하다.** pwdn 핀은 optional API(`devm_gpiod_get_optional`)로 얻으므로
DT 에 없으면 `pwdn_gpio` 가 NULL 이고 설정이 무동작이다. 그런 보드에서는 bind 가 전원을
끄지 않아 resetless rebind 가 여기서 측정한 것보다 더 진행될 수 있고, `max9296_loadfw()`
에 도달할 수도 있다. 이 문서의 어떤 측정도 그 경우를 덮지 않는다.

### 이 후속 측정의 한계

- **고fps 로 성공적으로 초기화된 단일채널 인스턴스의 rebind 는 재현하지 못했다.**
  `640x360@31..120` 은 FSYNC 예약이 그 rate 로 묶이므로, 같은 epoch 에서 술어가 거짓인
  튜플로 재초기화할 수 없다. 그래서 stale 값은 손으로 심었다 — 레지스터는 누가 썼는지
  기억하지 않으므로 전제로는 동등하지만, 드라이버가 쓴 경우를 그대로 재현한 것은 아니다.
- "resetless 경로는 항상 fail-closed" 를 일반 명제로 주장하지 않는다. 주장하는 것은
  **측정한 세 구성에서 그랬고, 그 원인이 probe 시점 PWDN 으로 규명됐다**는 것이다.
  `powerdown-gpios` 가 없는 보드는 위 「되돌리기 쓰기 질문에 대한 답」의 범위 조건대로
  다를 수 있으며 재지 않았다.
- 세 실험 모두 보드를 고장 상태로 만들었고 매번 `systemctl start cam-operate.service`
  로 복구했다. `cam_hard_reset.sh -s -S` 는 `DEPRECATED: forwards one recovery request`
  를 찍고 `rc=0` 을 반환하지만 서비스가 내려간 상태에서는 복구하지 않았다 — exit code 를
  복구의 증거로 쓰면 안 된다. 복구 후 매번 AP1302 4개 응답·프레임 흐름·원래 튜플을
  확인했다.

## 원시 기록

단계별 명령/출력 전사는 커밋하지 않는다 — 이 저장소는 `artifacts/board-*/raw/` 를
추적하지 않는다(`.gitignore:38`). 재현에 필요한 것은 위 표에 있다: 읽기 명령, 대상 주소,
각 단계의 prepare 튜플과 관측된 상태·레지스터 값, 그리고 판정을 지지하는 dmesg 문구.
측정 당시의 상태 줄 원문은 이슈 #91 의 측정 코멘트에 남아 있다.
