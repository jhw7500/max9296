# 세션 작업 내역 — 2026-08-10 (session 2958c6b2)

MAX9296 듀얼 디시리얼라이저 드라이버의 전원/FSYNC/enable 결함 감사 및 수정, DTS pinctrl 정합,
그리고 ch0+ch1 듀얼 FHD 고fps 스트림 실패 원인 추적.

| 항목 | 값 |
|---|---|
| 세션 ID | `2958c6b2-9d5e-4db1-a9ca-9ba4cf5e6469` (API: `session_01Af2q6WQLgQns8v2BLU5X3h`) |
| 기간 | 2026-08-10 03:31 ~ 14:55 UTC (12:31 ~ 23:55 KST), 약 11.4시간 |
| 브랜치 | `fix/power-refcount-fsync-gates` |
| 산출 커밋 | `d8ec2e1` (max9296.c +322 −148), `5ae703f` (docs/imx8mp-evk.dts 신규 1673줄) |
| PR | [#10](https://github.com/jhw7500/max9296/pull/10) — 리뷰 완료, **머지 안 됨**(블로킹 지적) |
| 편집 | `max9296.c` 45회, `docs/imx8mp-evk.dts` 4회 |
| 검증 | 크로스빌드 26회, 독립 검증 워크플로우 8회(약 50 에이전트), 타겟보드 실측 60여 회 |
| 종료 사유 | `/compact` → 후속 세션으로 인계 (ch2/ch3 대조 시험 진행 중 중단) |

---

## 0. 타임라인

| 시각(UTC) | 단계 | 내용 |
|---|---|---|
| 03:31–04:00 | 감사 | i2c2/i2c3 듀얼 구성의 reset·전원·pinctrl·공유상태 결함 탐색 |
| 04:00–05:46 | 수정+검증 | 전원 refcount 일원화, FSYNC/enable 게이트 정정. 6라운드 독립 검증 |
| 05:47–06:00 | DTS | pinctrl 그룹을 실제 사용 핀에 정합, DTB 디컴파일 대조 |
| 06:07–07:46 | 후속 수정 | 로그 가시성, probe 에러경로, remove 순서, ser 주소 주석, 죽은 상태값 제거 |
| 07:47–08:22 | 런타임 fps | 앱의 fps 변경 실패 원인 3층 분해, FSYNC 주기 재계산 구현 |
| 08:43–08:58 | 릴리스 | PR #10 생성, 4개 리뷰어 라운드 모니터링 및 지적 판정 |
| 08:48–14:55 | fps 추적 | ch0+ch1 듀얼 FHD 60fps 실패 원인 추적 — 가설 6개 제기·검증·철회 |

---

## 1. 커밋 `d8ec2e1` — 전원 시퀀스 refcount 일원화 및 FSYNC/enable 게이트 정정

### 1-0. 전제 (사용자 확인으로 확정된 운용 모델)

이 절의 모든 판단은 아래 사실 위에 서 있다. 감사 초기 가정과 달라 여러 지적이 폐기·재평가됐다.

| # | 확정 사실 |
|---|---|
| 1 | 데시리얼라이저를 하나만 제어하는 시나리오는 **없다**. 전원은 전체 on 또는 전체 off. |
| 2 | `GPIO1_IO01` = 전 채널 카메라 모듈 전원, `GPIO1_IO08` = ch0/1 데시리얼라이저 전원, `GPIO1_IO09` = ch2/3 데시리얼라이저 전원. **모두 리셋이 아니라 전원 레일**이다(속성명이 `reset-gpios`/`powerdown-gpios`인 것과 무관). |
| 3 | `GPIO1_IO05` = FSYNC, 전 채널 공용 |
| 4 | ch0/1 사용 시 `/dev/video4`, ch2/3 사용 시 `/dev/video3` |
| 5 | 채널 구성 변경에는 **드라이버 재로드 또는 재부팅이 필수**다(펌웨어 레지스터가 채널에 맞게 로드되어야 함). stream on/off는 동일 채널 내 앱 재시작에만 유효. |
| 6 | 앱 재시작은 하드웨어 리셋이 아니다(로그로 확인: `s_power(1)`에서 `power_count != 0` → 시퀀스 미실행). |

### 1-1. 전원 시퀀스 refcount 일원화

- **목적** — 보드 전원(모듈 레일 + 데시리얼라이저 레일 2개)은 전 채널 공용이므로 한 번 켜고 한 번 꺼야 한다.
- **근거** — 기존 `state.power` 핸드셰이크가 off 방향에서 오동작:
  (a) 마지막 채널을 닫아도 peer의 `state.power`가 `DONE`이라 전원이 안 꺼짐,
  (b) `s_power`가 peer 상태까지 `IDLE`로 덮어써, 한 채널 재open이 스트리밍 중인 상대를 전원 사이클로 날리는 경로 존재.
- **수정** — `state.power` 핸드셰이크 → 모듈 전역 refcount(`max9296_power_users` + `max9296_power_lock`).
  `s_power`는 더 이상 `state.power`, 특히 peer의 것을 쓰지 않는다. `ssleep(5)`는 실제로 전원을 끈 경우에만 실행되도록 `set_power` 안으로 이동.
- **불변식** — `max9296_power_users == (power_count > 0 인 인스턴스 수)`. 증감은 각 인스턴스의 0→1 / 1→0 에서만, `sensor->lock` 아래에서, 카운터 자체는 `power_lock` 아래에서 변경.
- **전원 시퀀스 함수 자체(`max9296_power`/`max9296_reset`/`set_power_on`/`set_power_off`)는 무수정**. 검증에서 실측된 시퀀스:
  ```
  ON : des 양쪽 OFF → 1s → 모듈 OFF → 2s → 모듈 ON → 1s → des ON   (총 ~5s)
  OFF: 모듈 OFF → des 양쪽 OFF → 5s settle
  ```
- **테스트/결과** — 검증 에이전트가 A/B open·close 전 순서를 열거해 불변식 성립 확인. 기존 "마지막 close에서 전원이 영구히 안 꺼짐" 버그가 실제로 닫혔음을 확인.

### 1-2. `max9296_remove` Phase 0 신설

- **목적** — 1-1이 만든 회귀 차단. `state.power`는 디바이스별이라 probe에서 초기화되지만, 전역 refcount는 `remove()`가 반환하지 않으면 unbind/rebind 시 0 이상으로 고착 → rebind 후 첫 open이 전원 시퀀스를 건너뛰어 노드가 죽은 채 올라온다.
- **수정** — subdev unregister 후 `sensor->lock` → `power_lock` 순으로 refcount 반환. **kthread 정지 이전**에 수행(그래야 `s_stream`의 펌웨어 대기와 엮여 unbind가 영구 블록되지 않음). peer 포인터 NULL 대입은 `kthread_stop` **이후**로(순서 역전 시 peer 스레드가 1s sleep에서 깨어나 `NULL+offset` 접근).
- **테스트/결과** — 전용 검증 라운드(WF8)에서 Phase 0의 락 대기가 유계인지 홀더별로 열거 확인. 3건 지적 반영 후 재검증 통과.

### 1-3. `max9296_fsync` — 단일측 분기의 `state.setup` 항 제거

- **목적** — 상대 video 노드를 열어 포맷만 잡아도 FSYNC가 영영 안 도는 경로 제거.
- **근거** — `state.setup`은 `set_fmt`의 `out:` 라벨에서 **TRY·에러 포함 무조건** 래치되고 지워지지 않는다. 실질 의미는 "probe 이후 `set_fmt`가 한 번이라도 불렸음"일 뿐인데 이름은 "설정 완료"로 읽힌다. 이 값이 FSYNC 게이트를 조용히 막아 `0x0313`이 기록되지 않았다.
- **수정** — 게이트를 `init` + `enable`(실제 스트리밍 여부)로 교체. 이후 `state.setup` **필드 자체를 제거**하고 구조체 자리에 재도입 방지 주석을 남겼다.
- **결과** — 조건 완화이므로 기존에 돌던 경우는 그대로. 부수적으로 "video3에서 `set_fmt`만 해도 video4가 `0x0313`을 못 받던" 경로가 해소.

### 1-4. `max9296_enable` — peer 레지스터 교차 기록과 fsync_gpio election 삭제

- **목적** — 각 인스턴스가 자기 디시리얼라이저만 프로그램하도록. **이 세션 변경 중 유일하게 현재 동작 중인 듀얼 경로를 건드리는 항목.**
- **근거** — 기존에는 FSYNC GPIO 소유자(`max9296_0`)가 peer의 `0x0313`·AE/AWB까지 대신 썼다. 문제 둘:
  (a) FSYNC GPIO가 없는 `max9296_1`은 아무것도 못 쓰고 빠져나갔고,
  (b) 두 스레드가 같은 AP1302 레지스터를 무락으로 다퉜다.
  `maxim_ops_i2c_write(sensor->shared.sensor, ...)` 호출은 수정 후 파일 전체에서 0건(grep 확인). `maxim_ops_i2c_write`는 항상 자기 `i2c_client->adapter`를 쓰므로 i2c2/i2c3는 물리적으로 분리되어 버스 경합도 없다.
- **수정** — self-only 기록 + 기록 블록을 `sensor->lock` 아래에서 생존 재확인 후 수행 + `stream_on`은 실제로 서비스한 패스에서만 소비.
- **테스트/결과** — 4채널(video3+video4) 동시 스트리밍 회귀 확인 **완료**(사용자 실기). 값과 레지스터는 이전과 동일하고 기록 주체만 바뀜.

### 1-5. `max9296_s_stream(off)` — `stream_on` 취소

- **목적** — 3초 미만 짧은 스트림이 남긴 stale 요청이 나중에 소비돼 유휴 채널의 CSI 출력을 켜던 문제 차단.
- **근거** — `stream_on`은 one-shot인데 `s_stream(0)`이 이를 지우지 않았다. 1-3의 게이트 완화가 그 stale 값을 도달 가능하게 만들었다(내가 만든 회귀).
- **수정** — stream-off 경로에서 `stream_on = 0`.

### 1-6. FSYNC 주기 fps 추종 (`low_fps` 재계산)

- **목적** — 런타임 fps 변경을 하드웨어에 실제 반영.
- **근거** — `max9296_s_frame_interval()`은 **하드웨어에 아무것도 쓰지 않는다**(`sensor->fps`·`frame_interval`·`pixel_rate`만 갱신). 따라서 fps가 하드웨어에 닿는 **유일한 경로는 FSYNC 펄스 주기**인데, 기존 `if (low == 0)` 가드는 스레드 수명(probe~remove) 중 1회만 계산했다.
  ```
  120fps → low =  7333 µs
   60fps → low = 15666 µs
   20fps → low = 49000 µs   ← 기존 코드에서는 절대 반영 안 됨
  ```
- **수정** — 가드를 `if (low_fps != <fps>)`로 교체(3곳). `sensor->fps`는 0이 될 수 없어(probe 120, `s_frame_interval`이 1 미만 거부) 첫 진입은 항상 계산되고, 이후 변경은 다음 펄스부터 적용.
- **로그** — `pr_notice_once` 2개를 제거하고 재계산 지점 안으로 로그를 옮겨 **fps가 바뀔 때마다 정확히 한 번** 찍히게 했다. 경로 구분: `single fps :`(peer 없음) / `dual fps :`(양쪽 다) / `side fps :`(한쪽만). `%d` → `%u` 정정.
- **결과** — 실측 로그로 `dual fps : 20, low : 49000, high : 1000` 전환 확인.

### 1-7. probe 에러 경로 정리

- **근거** — `mutex_init`이 3987행에 있어 그보다 앞선 에러 경로가 공통 정리 라벨을 쓸 수 없었다. `init_controls` 실패 시 `shared_init` kthread가 정리되지 않아 devm 해제 메모리를 참조(UAF)하고, 미초기화 mutex에 `mutex_destroy`가 걸리고, 그 경로에서 **probe가 0(성공)을 반환**했다.
- **수정** — `mutex_init`을 `devm_kzalloc` 직후로 이동 / `kthread_run` 실패 시 `ret = PTR_ERR()` 설정 후 공통 tail로 / `media_entity_pads_init` 실패의 bare `return ret` → `goto entity_cleanup` / shared kthread 정리를 `entity_cleanup`으로 이동 / `of_node_put` 추가(에러 tail + `remove()`).

### 1-8. 주석·변수 정정 (동작 변화 없음)

| 항목 | 정정 내용 |
|---|---|
| 시리얼라이저 주소 | `0x40`/`0x60`은 **듀얼 전용 매핑**이며 단일에는 리맵이 없어 어느 채널이든 `0x40`. 주석에 혼용 없도록 명시. |
| `max9296.c:1341-1343` | "Right 모드에서 Link B를 0x40에 매핑"은 사실과 다름. Right 모드의 활성 링크는 **Link A**(실측). `0x40`인 이유는 링크와 무관하게 단일 구성에 리맵이 없어서다. |
| `link_a_err`/`link_b_err` | GMSL PHY가 아니라 **짝수/홀수 채널 플래그**. 이 이름 때문에 두 번 헛짚었다. |
| `static restart_cnt` | 함수 스코프 static → 지역 변수(인스턴스 간 공유 제거). |

**실측 근거** (단일 ch1 장비, i2c2):
```
i2ctransfer -f -y -a 2 w2@0x48 0x00 0x13 r1  →  0xda   CTRL3: LINK_MODE=01(Link A), LOCKED, ERROR 없음, CMU_LOCKED
i2ctransfer -f -y -a 2 w2@0x48 0x00 0x2f r1  →  0x06   RX3: SYNC_LOCKED_A|WBLOCK_A (B쪽 비트 0x60 전무)
```
→ `_R` 테이블(`LINK_CFG=1`)이 로드됐고 그것이 Link A. 따라서 "채널 1 = Link B"라는 코드 상수 주석은 하드웨어 사실이 아니며, 채널 귀속 로직(`slave_to_global_ch`/`ch_ctrl_applies`/`load_regs`)은 **모두 정상**이다. 앞서 MEDIUM으로 올렸던 "라벨 반대" 지적을 철회했다.

### 1-9. 검증 방법과 결과

| 수단 | 내용 |
|---|---|
| 크로스빌드 | `./make-for-imx8` 26회. 최종 에러 0, 경고 20개(기존과 동일 — 늘지 않음). HEAD와 대조해 "미사용 함수 경고 증가"가 패치 무관임을 확인. |
| 독립 검증 워크플로우 | 6라운드 8회 실행, 약 50 에이전트. 렌즈: refcount / deadlock·lock-order / coverage / scenarios / streamon / sequence |
| 검증이 잡은 **자체 회귀 4건** | ① stale `stream_on`으로 유휴 채널 CSI 활성화 ② 조기 소비로 요청 영구 유실 ③ `remove()`의 unbind 영구 블록 ④ refcount 미반환. **전부 닫음** |
| 최종 라운드 | `ship_it` / 코드 결함 0건 (주석 근거 오류 1건만 나와 정정) |
| 실기 | 4채널(video3+video4) 동시 스트리밍 정상 — 사용자 확인 완료 |

---

## 2. 커밋 `5ae703f` — DTS pinctrl 그룹을 실제 사용 핀에 정합

- **목적** — pinctrl 그룹이 예전 GPIO 배치(powerdown=GPIO3_IO19, reset=GPIO1_IO06) 시절 내용 그대로였고 `*-gpios`만 옮겨져, 실제 사용 핀 `GPIO1_IO08`/`IO09`가 **어떤 선택된 그룹에도 없었다**.
- **근거** — 패드 리셋 기본값이 ALT0=GPIO라 동작은 했지만 `SW_PAD_CTL`이 프로그램되지 않고, pinctrl 코어가 패드를 claim하지 않아 **충돌도 감지되지 않는다**. 이 핀들이 리셋이 아니라 **전원 인에이블**이라는 정정으로 우선순위가 올라갔다.
- **수정**
  - `csi0_pwn`: GPIO1_IO01 → **GPIO1_IO09**
  - `csi0_rst`: GPIO1_IO06 → **GPIO1_IO01**
  - `csi1_pwn`: SAI5_RXFS/GPIO3_IO19 → **GPIO1_IO08**
  - `csi1_rst` / `csi1_fsync` 그룹 삭제, `max9296_1`의 `pinctrl-0`을 `csi1_pwn` 하나로. (`max9296_1`은 reset/fsync를 요청하지 않는다 — 보드 공용이며 `max9296_0` 소유)
  - 삭제한 그룹이 잡고 있던 `SAI5_RXC`는 `&micfil`의 PDM_CLK → **PDM 오디오를 켜는 순간 probe 순서로 승자가 갈리던 잠재 충돌이 함께 사라짐**
  - `IO08`/`IO09`를 mux하지만 참조되지 않는 `pinctrl_mipi_dsi_en` / `pinctrl_i2c2_synaptics_dsx_io`에 select 금지 경고 주석 추가
- **테스트 방법** — `dtc` 컴파일 후 **디컴파일 대조**. 같은 매크로를 쓰는 기존 그룹과 바이트 일치 검사.
- **결과** — `csi0_pwn = 0x38/0x298`(=IO09), `csi1_pwn = 0x34/0x294`(=IO08) 바이트 일치.
  요청 GPIO와 mux 커버리지 완전 일치, 활성 그룹 간 패드 중복 0, dangling 참조 0.
  사용자가 컴파일한 `docs/imx8mp-evk-test.dtb`도 같은 방식으로 검증 — **정상 반영 확인**.
- **보강** — 세션 후반 `docs/imx8mp.dtsi` 확보로 i2c alias와 CSI 클럭 정의가 확정되어 DTS 검증의 마지막 공백이 닫혔다.

---

## 3. PR #10 — 리뷰 라운드 결과

`/jhw:ship` 실행. 4개 리뷰어 전부 응답 완료, `Build and Test` success. **블로킹 지적이 있어 머지하지 않았다**(`--merge` 미지정).

| 리뷰어 | 지적 | 판정 |
|---|---|---|
| Claude 리뷰 | HIGH×1, MEDIUM×4, LOW×2 | FEEDBACK |
| Codex 앱 | inline **P1**×1 | FEEDBACK |
| Gemini 리뷰 | HIGH×1, MEDIUM×4, LOW×1 | FEEDBACK (diff 50000자 초과로 **부분 커버리지** — DTS 1673줄 신규 추가 때문) |
| OpenCode 리뷰 | 다수 | FEEDBACK |

### 유효한 지적 (미반영 — 후속 작업)

1. **Codex P1 · refcount 스코프** — 두 MAX9296이 `shared.sensor == NULL`(fsync 공유 핸들 없음)로 독립 전원 레일을 쓰는 구성에서, 모듈 전역 카운터가 둘을 한 자원으로 취급해 두 번째 인스턴스의 power-on이 통째로 스킵된다. Claude MEDIUM·Gemini MEDIUM도 독립적으로 동일 지적. **일반성 후퇴가 맞다.** 이 보드는 두 노드 모두 `fsync,shared` + 핸들이 있어 동작은 동일하지만, 카운터를 **공유 그룹 단위**로 좁히면 이 보드 동작 불변인 채 일반성이 복원된다.
2. **Gemini HIGH · power-on 실패 시 refcount 롤백 누락** — `max9296_set_power_on()`이 현재 항상 0을 반환하므로 오늘은 도달 불가. 다만 3줄짜리 방어이므로 넣어둘 가치 있음. 심각도는 과대평가.

### 반려·반박

- **Claude HIGH · `if (debug)` → `if (1)` 무조건 로그** — 사용자가 의도적으로 직접 수정한 부분(항상 출력). **declined**로 기록.
- **OpenCode "remove() Phase 0 동시 unbind 시 음수"** — 틀림. 해당 블록은 `max9296_power_lock` 안이고 각 디바이스는 자기 `power_count > 0`에 걸려 한 번만 감소. 리뷰어가 mutex를 뺀 채 인용했다.
- **OpenCode "`low_fps == 0` 나눗셈"** — 도달 불가. `low_fps`는 `sensor->fps`에서만 받고 그 값은 0이 될 수 없다.

### 리뷰가 잡아낸 진짜 기존 결함 (별도 이슈 권고)

`usb1grp` 노드 중복 — DTB 실물 확인됨:
```
1364: pinctrl_usb0_vbus: usb1grp {   ← USB2_PWR 핀 있음
1371: pinctrl_usb1_vbus: usb1grp {   ← 핀 전부 주석
→ dtc가 병합, 뒤쪽(빈 것)이 이김:  usb1grp { fsl,pins; phandle = <0x7f>; }
```
`&usb_dwc3_0`가 참조하는 `pinctrl_usb0_vbus`가 **빈 그룹**이고 `GPIO1_IO14__USB2_PWR`가 **한 번도 mux되지 않는다.** 기존 결함이나 커밋한 파일 안에 있음. 고치면 USB 전원 동작이 바뀌므로 카메라 PR에 섞지 않았다.

---

## 4. 런타임 fps 변경 실패 — 3층 분해

앱에서 60→20fps 변경 시 `not-negotiated (-4)`로 파이프라인 사망.

| 층 | 진단 | 조치 |
|---|---|---|
| **앱/브리지** | `GstV4l2Src` ↔ `/dev/videoN` 협상 실패. v4l2src는 **PLAYING 중 프레임레이트 재협상을 지원하지 않는다.** subdev는 20fps를 거부하지 않았다(`DEFAULT_FRAMERATE_FPS`=120 → 1~120 전부 advertise, `s_frame_interval` 수락) | 앱에서 소스를 READY로 내렸다 올려야 함 — **미조치(앱 영역)** |
| **드라이버** | 협상이 성공해도 레이트가 안 바뀐다 — `s_frame_interval`은 하드웨어에 아무것도 안 쓰고, 유일한 경로인 FSYNC 주기가 스레드 수명 중 1회만 계산됨 | **수정 완료** (1-6) |
| **재시작 지연** | 재시작 경로 누적 sleep ≈ 3.6~4.3초 동안 FSYNC 펄스 없음 → "뒤죽박죽하다 한참 뒤 안정화" | **미조치**(근거 확보됨, 방식 미결정) |

실측 타임라인 (fps 60→20, 로그 기준):

| 시각 | 사건 |
|---|---|
| 61.222 | fps=20 설정 |
| 61.233 | FSYNC 주기 49000µs 전환 |
| 61.224 / 61.334 | 양쪽 STREAMOFF → `restart=1`, `state.fsync = IDLE` |
| 61.799 / 62.199 | 양쪽 STREAMON (채널 간 **0.4초 편차**) |
| ~65.2 | `restart_cnt` 1s + 자기 fsync 1s + peer fsync 1s ≈ **3초 뒤** FSYNC 재개 |
| ~65.5 | enable 스레드 `msleep(300)` 후 `0x0313` 재기록 |

→ **61.2 → 65.5, 약 4.3초 FSYNC 공백.**

세 sleep은 중복이 아님을 확인했다(듀얼 분기는 두 값을 함께 RUNNING으로 만들지만 **단일측 분기는 한쪽만** 세팅하므로 각 검사가 의미를 가진다 — 앞서 "중복일 것"이라 한 판단은 틀렸다). 축소 방안 2개를 제시했고 사용자 결정 대기:
- (a) 가장 중복인 `restart_cnt` 1초 제거 → 4.3s → 3.3s
- (b) `fsync_restart_ms`/`fsync_settle_ms`를 `module_param`으로 노출해 재빌드 없이 하한 탐색 **(권장)**

---

## 5. ch0+ch1 듀얼 FHD 60fps 실패 추적

**증상** — ch0+ch1(i2c3, 3840×1080) 60fps에서 앱이 파일 생성을 못 해 재시작. 재시작·재부팅을 반복하면 결국 동작. ch0+ch3는 정상.

### 5-1. 확정된 사실 (실측)

| 항목 | 값 |
|---|---|
| 파이프라인 | `[max9296 2-0048] → [mxc-mipi-csi2.1] → [mxc_isi.1] → /dev/video4` (i2c2 쪽은 csi2.0 → isi.0 → video3) |
| GMSL 링크 | **정상** — CTRL3 `0xfa`(Splitter, LOCKED), RX3 `0x66`(A·B 양쪽 lock) |
| FSYNC | **60 Hz** (`low:15666 + high:1000` = 16.67ms) |
| 요청 | 60 fps (`calc_pixel_rate 248832000` = 3840×1080×60) |
| 실제 도착 | **19~20 fps** (ISI 인터럽트 199/10초) |
| CSI/ISI 커널 에러 | **없음** (ECC/CRC/overflow/underrun 전무) |
| 유일한 에러 | MCP4018 `-6`(ENXIO) — 정상 구성에서도 동일하게 발생 → 무관 |
| 관측된 루프 | 앱이 15초 대기 후 포기 → 재시작 → `chk_cam_operate`가 재부팅. **약 4분 주기 재부팅 루프** 실제 확인 |

### 5-2. 대역폭 계산 (`SERDES_3GBPS` 검증)

포맷 `MEDIA_BUS_FMT_UYVY8_2X8` = 16 bpp. 듀얼링크에서 카메라 1대 = GMSL 링크 1개.

| 구성 | GMSL 링크당 | 3Gbps 대비 | CSI-2 합계 | lane당(4레인) |
|---|---|---|---|---|
| 1280×720@30 듀얼 | 0.44 Gbps | 15% | 0.88 Gbps | 221 Mbps |
| 1920×1080@30 듀얼 | 0.99 Gbps | 33% | 1.99 Gbps | 498 Mbps |
| **1920×1080@60 듀얼** ← 실패 | **1.99 Gbps** | **66%** | **3.98 Gbps** | **995 Mbps** |
| 1920×1080@60 ch0+ch3 ← 정상 | **1.99 Gbps** | **66%** | 1.99 Gbps | 498 Mbps |

**3 Gbps를 넘지 않는다(66%).** 3840 폭은 디시리얼라이저 내부 합성 결과이며 **GMSL 링크를 지나지 않는다**(DT/VC 매핑 레지스터를 쓰고 tunnel mode `0x0330`은 건드리지 않음 → pixel mode). 마지막 두 행이 결정적 — 정상 구성과 실패 구성의 **GMSL 부하가 완전히 동일**하므로 SERDES 레이트는 비대칭을 설명하지 못한다.

부수 발견 — MAX9295A REG1 `TX_RATE[3:2]`: `01`=3Gbps, `10`=6Gbps. 기본값은 **CXTP 핀 래치**로 정해진다(Coax=6Gbps, STP=3Gbps). 드라이버는 `{0x0001, 0x04}` = 3Gbps **강제**. 보드가 Coax면 절반 속도로 쓰는 셈이므로 `CTRL1(0x0011)` bit0/bit2 확인 가치가 있다(마진 66%→33%).

### 5-3. 가설 정정 이력 — 제기 → 반증

이 세션의 절반이 이 표다. 같은 길을 다시 걷지 않기 위해 남긴다.

| # | 가설 | 근거 | 어떻게 반증됐나 |
|---|---|---|---|
| 1 | `SERDES_3GBPS` 제한 | 3Gbps 강제 설정 존재 | 계산: 정상 구성도 동일 66%. 비대칭 설명 못 함 → 우선순위 하락 |
| 2 | `mipi_csi_1` 픽셀 클럭 266 MHz (`csi_0`는 500 MHz) | 3840×1080@60 = 248.8 MPix/s = 266MHz의 **93.5%**, 다른 모드는 전부 절반 이하. 비대칭·모드별 성패를 모두 설명 | 사용자가 **500 MHz로 변경 후에도 재현** → 배제 |
| 3 | `csis-hs-settle = <13>` 고정 | 498/995 Mbps/lane에 같은 값 | 4~30 스윕 전부 무효 → 배제 |
| 4 | MAX9296 `0x0320` PHY 레이트 | 듀얼 테이블에 값이 다름 | `0x26` 추가·실측 확인(`r1 → 0x26`)했으나 **여전히 재현** → 배제 |
| 5 | ISI 채널 1은 폭 2048 초과 불가 (체인 버퍼 부재) | `isi_chain` 프로퍼티가 `isi_0`에만 있고 체인 버퍼 주소가 `0x32e02000`(= isi_1 베이스) → 구조적으로 채널 0만 체인 가능. 로컬 커널 커밋 `51e1d0ddb1d0`이 `ISI_2K 2048U → 4096U`로 올려 `chain_buf()` 조건(`3840 > 4096` = 거짓)과 `-EINVAL` 가드를 **동시에 무력화** | **같은 경로로 3840@30이 29.22fps 완주**(60프레임 = 497,664,000 바이트 정확 일치) → 폭 제한이면 30fps도 깨져야 함 → **철회** |
| 6 | 프레임레이트 경계가 30~40 사이 | 30fps 정상 / 40fps 프레임 0 | 클린 부팅 후 **40fps를 먼저** 시도하니 나옴 → **철회** |

`ISI channel[1] is busy` 로그는 폭 제한이 아니라 **채널 점유 충돌**(`m2m_enabled || is_streaming`) 조건이었다. `&isi_0`에 `m2m_device { status = "okay" }`가 있다.

또한 가설 5의 부산물로, ch0/ch1을 ISI 채널 0에 물리는 방법이 확인됐다 — ISI 입력은 `CHNL_CTRL.SRC_INPUT` 크로스바이며 채널 0에 대한 제약이 없다. dtsi의 `interface = <IN_PORT, SUB_IN, OUT>`에서 `isi_0`을 `<3 0 2>`, `isi_1`을 `<2 0 2>`로 맞바꾸면 된다(단 `/dev/videoN` 번호가 바뀌고, 체인 버퍼가 하나뿐이라 ch2+ch3가 3840을 못 쓰게 된다). **가설 5가 철회되어 현재 이 조치의 근거는 없다.**

### 5-4. 세션 종료 시점의 가설 (미확정)

클린 부팅 후 순차 시험:

| 순서 | 설정 | 결과 |
|---|---|---|
| [1] 클린 부팅 후 첫 시도 | 40 fps | **19.98 fps** (프레임 나옴) |
| [2] 리로드 없이 이어서 | 30 fps | **프레임 0** |
| [3] 리로드 없이 이어서 | 40 fps | **프레임 0** |

→ **"모듈 로드 후 첫 스트림만 동작하고 두 번째부터 프레임이 안 나온다"**로 재해석. 드라이버 코드와 겹치는 지점:

```c
/* max9296_s_stream(1) */
if (sensor->restart == 0) { max9296_set_mode(); ... 펌웨어 로드 ... }
else { /* 두 번째부터: set_mode·펌웨어 전부 건너뜀 */ max9296_apply_cached_controls(); sensor->restart = 0; }
```
`max9296_load_regs()`는 probe 생애에 한 번만 실행된다. STREAMOFF가 `restart = 1`을 세우면 다음 STREAMON은 초기화를 건너뛰고 `0x0313`만 쓴다. 그 사이 `set_stream_mipi(off)`로 `0x0313=0x00`이 들어갔고 FSYNC도 IDLE로 리셋된 상태다.

**단, 이 가설은 모순을 남긴다** — 매번 `rmmod`/`modprobe`로 리로드한 31·33·35 fps 시험이 **전부 0**이었다. 리로드가 재부팅만큼 상태를 초기화하지 못하는 것으로 보이지만 미확인. ch2/ch3 대조 시험을 시작한 지점에서 세션이 종료됐다.

### 5-5. 테스트 방법 (재사용 가능)

앱을 거치지 않고 하드웨어 카운터·원시 캡처로 측정. gstApp 파이프라인에는 `videorate`와 캡스 협상이 있어 하류에서 본 fps는 소스의 진짜 레이트가 아니다.

```bash
# 0) 앱 정지 — gstApp은 죽으면 시스템이 자동 재시작하므로 데몬을 내려야 한다
systemctl stop cam-operate.service   # 재부팅까지 유지하려면 disable
pkill -9 gstApp

# 1) 소스 레이트 vs 전달 레이트 분리 측정 (하드웨어 카운터)
A=$(grep 32e02000.isi /proc/interrupts | awk '{print $2}')   # ISI  = 메모리에 기록된 프레임
C=$(grep 32e50000.csi /proc/interrupts | awk '{print $2}')   # CSI2 = 디시리얼라이저가 보낸 프레임
sleep 10; # 재측정 후 차분 / 10

# 2) fps 설정 후 원시 캡처
media-ctl -V '"max9296 2-0048":0 [fmt:UYVY8_2X8/3840x1080@1/40]'
media-ctl -p | grep -oE '3840x1080@1/[0-9]+'                 # 실제 반영 확인
v4l2-ctl -d /dev/video4 --set-fmt-video=width=3840,height=1080,pixelformat=RGBP \
         --stream-mmap --stream-count=90                     # 실측 fps 출력

# 3) 프레임 완전성 검증 — 파일 크기 정확 일치 + 행별 비영 픽셀 비율
#    (잘린 프레임/라인버퍼 문제는 행 단위 분포에서 드러난다)

# 4) 디시리얼라이저 상태
i2ctransfer -f -y -a 2 w2@0x48 0x00 0x13 r1   # CTRL3 LINK_MODE/LOCK
i2ctransfer -f -y -a 2 w2@0x48 0x00 0x2f r1   # RX3   링크별 lock
i2ctransfer -f -y -a 2 w2@0x48 0x03 0x13 r1   # 0x0313 CSI out

# 5) 마무리 — 반드시 원복
systemctl enable --now cam-operate.service
```

**주의** — 캡처 시 협상 포맷을 확인해야 한다. `/dev/video4`는 UYVY 미지원으로 **RGB565(RGBP)로 협상**되며, 바이트 오프셋을 잘못 잡으면 "프레임이 거의 전부 0"으로 오독한다(실제로 한 번 오독했다).

---

## 6. 미해결 / 후속 작업

### 우선순위 높음

| 항목 | 상태 |
|---|---|
| **ch0+ch1 듀얼 고fps 실패** | 원인 미확정. 최신 가설은 "모듈 로드 후 첫 스트림만 동작"이나 리로드 시험과 모순. ch2/ch3 대조 시험 미완 |
| **PR #10 블로킹 지적 2건** | refcount를 공유 그룹 단위로 축소 + power-on 실패 롤백 3줄. 미반영 |
| **상대 채널 펌웨어 로드 구간 FSYNC 정지** | dual 분기에 단일측 폴백이 없어 수 초간 스트리밍 중인 쪽 FSYNC 정지. 잔여 항목 중 최우선 |
| **MCP4018 `-6`(ENXIO)** | 양 채널 모두 NAK. DMA write(`0x3270`)는 성공하므로 GMSL·AP1302는 정상 → MFP4 게이트 또는 `0x2F` 주소 변환 문제로 추정. 미착수 |

### 우선순위 낮음

- `s_stream(0)`이 peer의 `state.fsync`까지 지워 듀얼에서 한쪽 정지 시 약 2초 FSYNC 공백
- 재시작 FSYNC 공백 3.6~4.3초 축소 — `module_param` 노출 방식 권장, 결정 대기
- `state.power`가 write-only 죽은 상태(읽는 곳 0). 무해하나 정리 여지
- 한쪽 노드만 unbind→rebind 후 open 시 그 데시리얼라이저 전원 미투입(운용 모델 밖, 베이스라인 동일)
- `current_mode` vs `last_mode` 불일치 5곳 — 한 모듈 로드 안에서 레이아웃을 바꿔야 발현. 재로드 필수 운용에서는 미발현
- 위생: 경고 20개(미사용 함수 15, `2894` unused-var 2, `2956` decl-after-stmt 2, `3231` format-extra-args 1), `debug` 기본값 0 vs 문서 1, `link_a_err` 변수명
- `usb1grp` 중복 → `GPIO1_IO14__USB2_PWR` 미mux (별도 이슈/PR)
- `CTRL1(0x0011)` bit0/bit2로 Coax/STP 판정 → Coax면 `SERDES_3GBPS` 주석 처리로 마진 2배

### 확인 필요한 잔여 상태

| 항목 | 내용 |
|---|---|
| **`cam-operate` 서비스** | 세션 종료 직전(14:11) `disable` 후 재부팅했고 14:39 재부팅 후 원복 기록이 없었으나, **2026-08-11 재확인 결과 `enabled` + `active`, gstApp 기동 중 — 해소됨** |
| **`chk_cam_connect.sh`** | 사용자가 이전 ch23 링크 에러 반복 때문에 주석 처리한 상태. 원복 여부 확인 필요 |
| **미커밋 변경** | `max9296.c`에 `{0x00, 0x0320, 2, 0x26, 1, 10}` 1줄 (720p 듀얼 테이블). 가설 4 실험 잔재이며 **효과 없음으로 판정됨** → 되돌릴지 결정 필요 |

---

## 7. 후속 세션 결과 (참고 — 이 세션 범위 밖)

2026-08-11 세션이 fps 상한의 실제 원인을 확정했다(`docs/fps-limit-analysis.md`, 커밋 `20fa29d`). 위 5절의 가설들과 배치되지 않고 보완한다.

**AP1302의 1080p 모드가 센서 판독과 출력 대역 양쪽 모두 30fps급으로 설정돼 있다. 관문이 두 개이고 둘 다 넘어야 한다.**

- **(가) 센서 판독** — 라인타임 26.27µs × 1080 = `SENSOR_FRAME_TIME` 28,368µs > 60fps 주기 16,667µs. 트리거 배수로 떨어져 19.8~21.8fps가 된다. `PREVIEW_LINE_TIME`(R0x201C)를 13.0µs로 쓰면 14,063µs로 실제 내려간다(실측, 런타임 설정 가능).
- **(나) 출력 대역** — AP1302 MIPI 출력이 300 Mbps/lane × 4 = 1.2 Gbps로 설정돼 FHD 실효 상한 28fps. FHD60은 1,990 Mbps 필요. 펌웨어 blob `v4l-ap1302-ar0234.fw` 오프셋 `0x11` 한 바이트(`0x2c` → `0xf4`)로 `HINF_MIPI_FREQ` 300.0 → 499.2 Mbps 변경 확인(실측).

→ **5절에서 관측된 19~20fps는 (가)의 트리거 배수 낙하와 정확히 일치한다.** 즉 CSI/ISI/GMSL 하류가 아니라 AP1302 상류가 병목이었다. 5-3의 가설 1~5가 전부 하류였던 이유가 여기서 설명된다.

즉시 60fps가 필요하면 HD 1280×720(55.5fps 실측)이 유일한 경로다. 남은 난관은 `MAX9296 0x0320`과 SoC `csis-hs-settle` — 초기화 시점에만 적용되어 런타임 조작으로 검증할 수 없고 정확한 값도 미확정이다.

또한 보드 진단 도구 4종이 추가됐다(커밋 `f38d766`, `tools/`): `cam_hard_reset.sh`(CSI2까지 내렸다 올리는 하드 리셋 — `rmmod`/`modprobe`만으로는 SoC 빌트인 `mxc-mipi-csi2-sam`·ISI가 복구되지 않는다), `cam_fps_probe.sh`(CSI2 Frame Start vs ISI 인터럽트 분리 계수), `cam_fps_matrix.sh`, `cam_fps_watch.sh`.

> **5-4의 "두 번째 스트림이 죽는다"가 위 (가)/(나)로 설명되는지는 별도 확인이 필요하다.** 레이트 상한과 재시작 실패는 다른 현상일 수 있다.
