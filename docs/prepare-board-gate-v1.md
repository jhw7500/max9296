# MAX9296 병렬 prepare 보드 게이트 (v1)

`docs/superpowers/plans/2026-08-13-max9296-parallel-prepare.md` Task 5 Step 4 가
요구하는 판정을 실보드에서 수행하는 절차와 그 실측값이다. `tools/cam_prepare_gate.sh`
가 이 문서의 판정을 그대로 구현한다.

이 경로를 기동 스크립트에 활성화하려면 네 항목이 모두 통과해야 한다.

**현재 상태: G1~G3 통과, G4 재측정 중.**

| 게이트 | 측정일 | 드라이버 | 결과 |
| --- | --- | --- | --- |
| G1, G2, G3 | 2026-08-20 | 2.5 (`41D8E9E128B7BB8873D14D7`) | 통과 |
| G4 | - | - | 재측정 중 |

2.5 가 2.4 와 다른 곳은 prepare admission 게이트와 전원 참조 인수다. **G3 는 바로 그
거부 경로를 시험한다**(잘못된 요청 5종 거부, cancel, expiry). 따라서 G1~G3 의 2.5
재검증은 남아 있는 작업이다 - 통과 사실 자체는 유효하나, 출시 버전에서 다시 확인된
것은 아니다.

계획서 Task 5 Step 4 의 전제조건은 위 단서와 함께 충족됐다. 활성화 자체는 별도
판단 사항으로 남는다 - 이 게이트는 드라이버가 계약대로 동작한다는 것만 말하고,
배포 시점이나 앱 전환 절차는 다루지 않는다.

## 판정 기준

| 게이트 | 판정 방법 |
| --- | --- |
| G1 두 CSI 도메인의 펌웨어 구간이 겹치는가 | 커널이 찍는 `start_fw_load`/`end_fw_load` 의 인스턴스별 구간을 dmesg 타임스탬프로 재서 교집합이 양수인지 확인 |
| G2 STREAMON 에서 2차 펌웨어 다운로드가 없는가 | STREAMON 전후로 `start_fw_load` 건수를 세어 증가가 0 인지, lease 가 V4L2 로 이양(`lease=0`)됐는지 확인 |
| G3 cancel / expiry / 잘못된 요청 정리 | cancel 후 `IDLE`+재 prepare 성공(전원 누수 없음), 미사용 lease 60초 만료, 잘못된 요청 5종 거부 + 상태 불변 |
| G4 single / dual 반복 사이클 | cold prepare 1회 후 warm 재사용 사이클 반복. 매 사이클 `CONSUMED`/`lease=0`/`match=1`/`worker_errno=0` 유지, 펌웨어 재다운로드 0건 |

## 실행

```sh
# 보드에서 (root). 감시 서비스를 내리고 하드 리셋을 걸므로 파괴적이다.
# 종료 시 trap 이 cam-operate.service 를 원복한다.
cam_prepare_gate.sh                    # 전 게이트, soak 100 사이클
cam_prepare_gate.sh -g G1,G2           # 일부만
cam_prepare_gate.sh -g G4 -c 20        # soak 사이클 수 지정
```

`cam_hard_reset.sh` 가 같은 디렉터리(또는 `/root/camtest/`)에 있어야 한다.

## 실측 (2026-08-19, pim-camera-v016, 드라이버 v2.4 `F547E08AF97249978B1EDF9`)

### G1 — 통과

```
부팅 베이스라인(직렬)   I2C:2 20.67 -> 24.36s,  I2C:1 27.15 -> 30.83s,  겹침 0
prepare 1회차           I2C:2 3659.6ms  I2C:1 3647.7ms  겹침 3647.7ms  전체 3659.6ms
prepare 2회차           I2C:2 3676.6ms  I2C:1 3659.9ms  겹침 3659.9ms  전체 3676.6ms
```

짧은 구간이 긴 구간에 완전히 포함된다. 직렬 합 7.31s 가 실제 3.66s 로 줄어 **49.9% 단축**
이며 2회 재현됐다. 양 도메인 `READY`/`lease=1`/`match=1`, 동일 `epoch`.

### G2 — 통과

STREAMON 구간 펌웨어 다운로드 0건. 양 도메인 `lease=0` 으로 V4L2 이양 확인.

### G3 — 통과

cancel 후 `state=IDLE lease=0` + 재 prepare 성공, 65초 후 `state=EXPIRED lease=0`,
잘못된 요청 5종(generation 0 / fps 0 / fps 121 / 미지원 해상도 / enable 7) 전부 거부하고
기존 상태 불변.

### G4 — 재측정 중

**이전에 기록했던 "100/100 통과" 는 무효다.** 그 측정을 돌린 하네스는 백그라운드
`v4l2-ctl` 을 `wait` 로만 거두고 종료 상태를 버렸다. 사이클 판정이 `prepare` 상태만
보았고, `prepare` 는 스트림이 실패해도 `CONSUMED` 로 정상이라 전 사이클이 통과로
집계됐다.

종료 상태를 확인하도록 고친 뒤 같은 조건에서 다시 돌리자 dual 50/50, single 49/50 이
STREAMON 실패였다. 원인은 아래 1 번의 D-PHY 로, 하드 리셋 없이 두 번째 스트림을 건
것이 문제였다.

G4 는 사이클마다 하드 리셋하는 방식으로 재설계했고 재측정 결과를 여기에 채운다.

## 이 하드웨어의 제약 — 게이트 설계가 이것들을 피해야 한다

### 1. 스트림을 두 번 걸려면 CSI2 를 재바인드해야 한다 (D-PHY)

한 번 스트림이 끝나면 CSI2 가 D-PHY 락을 놓는다. 재바인드 없이 다시 STREAMON 하면
호출은 성공하는데 **프레임이 하나도 오지 않는다**.

```
[리셋+prepare] 스트림 1   exit=0    ISI+11  CSI2+22
[리셋 없이]    스트림 2   exit=124  ISI+0   CSI2+0     <- v4l2-ctl timeout
[리셋+prepare] 스트림 3   exit=0    ISI+11  CSI2+22
```

이때 `prepare` 상태는 `CONSUMED` 로 정상이라, 상태만 보는 검사는 이 실패를 놓친다.
그래서 G4 는 사이클마다 `cam_hard_reset.sh` 로 CSI2 를 재바인드하고, `v4l2-ctl` 의
종료 상태를 반드시 확인한다.

`cam_hard_reset.sh` 헤더에 2026-08-11 자로 기록된 "STREAMON 은 성공하는데 CSI2 이벤트
카운터가 전부 0" 이 같은 현상이다.

> **정정** — 이 문서의 이전 판은 "하드 리셋 21.8초 > 워치독 15초라 사이클마다 리셋하면
> 워치독 리셋이 난다" 고 적었다. **틀렸다.** `watchdog` 데몬은 별도 RT 프로세스라
> 스크립트가 22초를 쓰든 계속 pet 한다(실측: 하드 리셋 전후 데몬 pid 동일, uptime 유지,
> 하루에 열 번 넘게 실행해도 재부팅 없음). 08-19 재부팅의 실제 원인은 아래 2 번이다.

### 1-1. 카메라가 오래 안 돌면 감시 스크립트가 재부팅한다

`chk_cam_operate.sh` 는 녹화 파일이 안 생기면 `init_cam.sh` 를 재시도하고, 한도를
넘기면 `reboot` 을 호출한다.

```
01:50:10 [err]   init_cam.sh (boot=3)
01:51:11 [err]   init_cam.sh (boot=4)
01:52:11 [emerg] rebooting because no start marker (boot=5)   <- chk_cam_operate.sh:1698
```

사용자 공간이 명시적으로 부른 `reboot` 이며 워치독과 무관하다. 시험 중에는
`cam-operate.service` 를 내려두면 이 경로가 돌지 않는다 - 실제로 43분 soak 동안
발생하지 않았다.

참고로 이 보드의 워치독은 두 개다. SoC 내장 WDOG(`fsl,imx8mp-wdt`, `imx2_wdt`)를
Debian `watchdog` 데몬이 15초로 물고 있고(`pim-package` postinst 가 주입), 별도로
`run-watchdog-custom.service` 가 데이터보드 UART 워치독을 70초로 건다
(`wdt_check 70 10 0`). 둘 다 시스템이 물렸을 때 동작하는 것이지 스크립트 실행
시간과는 무관하다.

### 2. 사용자 공간에서 전원을 내릴 수 없다

`v4l2-ctl` 이 종료해도 `s_power(0)` 이 호출되지 않는다. 벤더 캡처 드라이버
(`imx8-isi-cap.c`)에 `s_power, 0` 호출이 아예 없고 해제 경로는 `s_stream(0)` 까지만
간다. 그래서 스트림을 닫아도 `state=CONSUMED` 가 유지되고 `epoch` 도 진행되지 않는다.

**전원을 실제로 내리는 유일한 수단이 하드 리셋이다.** 그래서 G4 의 각 사이클은
하드 리셋으로 시작한다 - 그래야 `epoch` 가 진행되고 다음 tuple 을 자유롭게 쓸 수 있다.

warm 재사용(하드 리셋 없이 앱만 재시작) 은 이 하네스로 측정하지 않는다. `v4l2-ctl` 은
1 번의 D-PHY 문제로 두 번째 스트림을 못 받기 때문이다. warm 경로 자체는 gstApp 으로
확인됐다 - B7 10 사이클(`action=2`, fps 15.1 실측)과 gstApp prepare 빌드의
2026-08-20 03:09 로그(`action=2 primary_errno=0 before_state=4 after_state=4`).

### 3. 캡처 노드는 UYVY 를 받지 않는다

드라이버 media-bus 포맷은 UYVY(`code=0x2006`)지만 ISI capture 노드는 이를 거부한다.
`cam_fps_probe.sh` 와 동일하게 `RGBP`(RGB565)를 쓴다.

### 4. dmesg 링버퍼가 빨리 감긴다

이 드라이버는 모든 i2c write 를 `KERN_NOTICE` 로 찍는다. 펌웨어 건수를 전체 버퍼로
세면 이전 구간이 잘려 **음수 델타**가 나온다(50 사이클에서 실측). 사이클마다 짧은 창으로
세어 누적하고, 창의 시작점이 사라졌으면 랩으로 판정해 실패 처리한다.

## 연관 문서

- `docs/parallel-prepare-v1.md` — ABI 계약, 버스 매핑
- `docs/superpowers/plans/2026-08-13-max9296-parallel-prepare.md` — Task 5 게이트 정의
- `tools/cam_hard_reset.sh` — 게이트가 cold 확보에 쓰는 리셋 경로
