# MAX9296 병렬 prepare 보드 게이트 (v1)

`docs/superpowers/plans/2026-08-13-max9296-parallel-prepare.md` Task 5 Step 4 가
요구하는 판정을 실보드에서 수행하는 절차와 그 실측값이다. `tools/cam_prepare_gate.sh`
가 이 문서의 판정을 그대로 구현한다.

이 경로를 기동 스크립트에 활성화하려면 네 항목이 모두 통과해야 한다.

**현재 상태: 네 항목 전부 통과.** 2026-08-21, 드라이버 2.5 한 버전에서 측정했다.

| 게이트 | 측정일 | 드라이버 | 결과 |
| --- | --- | --- | --- |
| G1, G2, G3, G4 | 2026-08-21 | 2.5 (`41D8E9E128B7BB8873D14D7`) | 전부 통과 |

네 게이트를 한 번의 실행으로 함께 쟀다. 이전 판본은 G1~G3 을 2.4 에서, G4 만 2.5 에서
잰 상태였는데, 2.5 가 2.4 와 다른 곳이 prepare admission 게이트이고 **G3 가 바로 그
거부 경로를 시험하므로**(잘못된 요청 5종 거부, cancel, expiry) 버전을 맞춰 다시 쟀다.

계획서 Task 5 Step 4 의 전제조건은 충족됐다. 활성화 자체는 별도
판단 사항으로 남는다 - 이 게이트는 드라이버가 계약대로 동작한다는 것만 말하고,
배포 시점이나 앱 전환 절차는 다루지 않는다.

## 판정 기준

| 게이트 | 판정 방법 |
| --- | --- |
| G1 두 CSI 도메인의 펌웨어 구간이 겹치는가 | 커널이 찍는 `start_fw_load`/`end_fw_load` 의 인스턴스별 구간을 dmesg 타임스탬프로 재서 교집합이 양수인지 확인 |
| G2 STREAMON 에서 2차 펌웨어 다운로드가 없는가 | STREAMON 전후로 `start_fw_load` 건수를 세어 증가가 0 인지, lease 가 V4L2 로 이양(`lease=0`)됐는지 확인 |
| G3 cancel / expiry / 잘못된 요청 정리 | cancel 후 `IDLE`+재 prepare 성공(전원 누수 없음), 미사용 lease 60초 만료, 잘못된 요청 5종 거부 + 상태 불변 |
| G4 single / dual 반복 사이클 | 매 사이클 hard reset, 병렬 cold prepare, STREAMON을 수행. `CONSUMED`/`lease=0`/`match=1`/`worker_errno=0` 유지와 예상 펌웨어 다운로드 건수(dual 2, single 1)를 확인 |

## 실행

```sh
# 보드에서 (root). 감시 서비스를 내리고 하드 리셋을 걸므로 파괴적이다.
# 종료 시 trap 이 cam-operate.service 를 원복한다.
cam_prepare_gate.sh                    # 전 게이트, soak 100 사이클
cam_prepare_gate.sh -g G1,G2           # 일부만
cam_prepare_gate.sh -g G4 -c 20        # soak 사이클 수 지정
```

`cam_hard_reset.sh` 가 같은 디렉터리(또는 `/root/camtest/`)에 있어야 한다.

## 실측 (2026-08-21, pim-camera-v016, 드라이버 2.5 `41D8E9E128B7BB8873D14D7`)

### G1 — 통과

```
부팅 베이스라인(직렬)   I2C:2 20.67 -> 24.36s,  I2C:1 27.15 -> 30.83s,  겹침 0
2026-08-21 측정         겹침 3657.8ms   직렬 합 7345.1ms -> 실제 3687.3ms
```

짧은 구간이 긴 구간에 거의 완전히 포함된다. 직렬 합 7.35s 가 실제 3.69s 로 줄어
**49.8% 단축**. 양 도메인 `READY`/`lease=1`/`match=1`, 동일 `epoch`.
(2026-08-19 에 2.4 로 잰 값도 겹침 3647.7ms / 3659.9ms 로 동등했다.)

### G2 — 통과

STREAMON 구간 펌웨어 다운로드 0건. 양 도메인 `lease=0` 으로 V4L2 이양 확인.

### G3 — 통과

cancel 후 `state=IDLE lease=0` + 재 prepare 성공, 65초 후 `state=EXPIRED lease=0`,
잘못된 요청 5종(generation 0 / fps 0 / fps 121 / 미지원 해상도 / enable 7) 전부 거부하고
기존 상태 불변.

### G4 — 통과 (100/100)

```
dual   5~50/50   성공 50  실패 0
PASS   dual cold 50회 전부 통과 (리셋->prepare->STREAMON->lease 이양)
single 5~50/50   성공 50  실패 0
PASS   single cold 50회 전부 통과
게이트 전체 PASS
```

소요 약 64분(23:32:21 - 00:36:40). 각 사이클은 하드 리셋 -> 병렬 cold prepare ->
STREAMON -> 상태 검증이며, 사이클마다 펌웨어를 다시 내려받는지(dual 2건 / single 1건)와
`v4l2-ctl` 종료 상태를 함께 확인한다.

**측정 무결성** — 30초 간격 `cam-operate` 샘플 129 건 중 active 는 1 건이고 그것은
`go_cold` 이전의 첫 샘플이다. 샘플링은 샘플 시점만 보이지만, 이 판본은 스트림 종료
상태를 확인하므로 샘플 사이의 개입은 사이클 실패로 드러난다. 실패 0 건과 함께 보면
구간에 개입이 없었다고 볼 근거가 된다.

**검증 이력** — 최종 통과에 이르기까지 다섯 번의 실패·무효 시도를 폐기했다. 다음
사람이 같은 길을 걷지 않도록 최종 통과 결과와 함께 남긴다.

| 시도 | 결과 | 원인 |
| --- | --- | --- |
| 08-19 | dual 40/50 에서 보드 재부팅 | 이 스크립트의 EXIT trap 이 `cam-operate` 를 되살림 (PR #23 에서 수정) |
| 08-20 03:15 | dual 10/50 에서 폐기 | 다른 세션의 시험이 `cam-operate` 를 되살림 |
| 08-20 03:26 | **허위 통과** 100/100 | 하네스가 `v4l2-ctl` 종료 상태를 버려 스트림 실패를 성공으로 집계 |
| 08-20 08:24 | dual 50/50, single 49/50 실패 | warm 반복 설계 자체가 D-PHY 때문에 불가능 |
| 08-21 15:19 | dual 49/50 실패 | 순차 prepare (아래 1-2) |
| 08-21 23:32 | **100/100 통과** | 사이클마다 리셋 + 병렬 prepare + 스트림 상태 확인 |

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

### 1-2. 순차 prepare 는 수락되지만 두 번째 도메인이 스트림하지 못한다

dual 구성에서 두 노드에 **순차로** write 하면 양쪽 다 `state=READY` 를 돌려주고 펌웨어도
2 건 정상인데, 두 번째 도메인의 스트림이 프레임을 거의 받지 못한다.

```
seq  prepare(READY,READY) fw=2  video3=0  video4=124  ISI0+11  ISI2+2
par  prepare(READY,READY) fw=2  video3=0  video4=0    ISI0+11  ISI2+12
seq  prepare(READY,READY) fw=2  video3=0  video4=124  ISI0+11  ISI2+1
par  prepare(READY,READY) fw=2  video3=0  video4=0    ISI0+11  ISI2+13
```

재현율 100 percent 다. `docs/parallel-prepare-v1.md` 는 "두 도메인에 병렬 write 후
wait" 를 규정하므로 순차 write 는 애초에 규정된 사용법이 아니지만, **ABI 가 그것을
거부하지 않고 `READY` 를 돌려준다.** 상태는 정상인데 하드웨어는 아닌 조합이 하나 더
있는 셈이다(1 번 D-PHY 와 같은 부류).

원인은 미확인이다. 두 인스턴스가 물리 FSYNC 를 공유하고 첫 도메인이 cadence 를
예약한 뒤 두 번째가 붙는 구조라 그 인계가 의심되지만 **검증하지 않았다.**

게이트는 문서 규정대로 병렬 write 를 쓴다. 드라이버가 순차 write 를 거부해야 하는지,
아니면 문서에 명시만 하면 되는지는 별건으로 남는다.

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
