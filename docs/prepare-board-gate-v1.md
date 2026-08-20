# MAX9296 병렬 prepare 보드 게이트 (v1)

`docs/superpowers/plans/2026-08-13-max9296-parallel-prepare.md` Task 5 Step 4 가
요구하는 판정을 실보드에서 수행하는 절차와 그 실측값이다. `tools/cam_prepare_gate.sh`
가 이 문서의 판정을 그대로 구현한다.

이 경로를 기동 스크립트에 활성화하려면 네 항목이 모두 통과해야 한다.

**현재 상태: 네 항목 전부 통과.** 다만 한 버전에서 통과한 것이 아니다.

| 게이트 | 측정일 | 드라이버 |
| --- | --- | --- |
| G1, G2, G3 | 2026-08-19 | 2.4 (`F547E08AF97249978B1EDF9`) |
| G4 | 2026-08-20 | 2.5 (`41D8E9E128B7BB8873D14D7`) |

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

### G4 — 통과 (100/100)

2026-08-20, 드라이버 2.5(`41D8E9E128B7BB8873D14D7`) 기준.

```
PASS  dual cold prepare 펌웨어 2건
      dual 10~50/50   성공 50  실패 0
PASS  dual warm 50회 - 펌웨어 재다운로드 0건
PASS  dual warm 50회 전부 재사용 조건 유지 (CONSUMED/lease=0/match=1)
PASS  single cold prepare 펌웨어 1건
      single 10~50/50 성공 50  실패 0
PASS  single warm 50회 - 펌웨어 재다운로드 0건
PASS  single warm 50회 전부 재사용 조건 유지
게이트 전체 PASS
```

소요 43분(03:26:56 - 04:09:52).

**측정 무결성** — `cam-operate` 가 시험 도중 되살아나면 gstApp 이 카메라를 가져가
결과가 무의미해진다. 이 실행은 30초 간격으로 서비스 상태를 기록했고 **86 개 샘플이
모두 inactive** 였다.

이 샘플링이 보이는 것은 **샘플 시점의 비활성**이지 연속 비활성이 아니다. 30초 안에
떴다 사라진 활성은 놓친다. 다만 그런 개입이 있었다면 스트림이 실패하고, 사이클
판정이 이를 잡는다 - `warm_soak` 이 `v4l2-ctl` 종료 상태를 확인하기 때문이다.
샘플 86 건과 사이클 100 건의 무결함을 함께 놓고 보아야 하며, 어느 한쪽만으로는
"구간 내내 개입이 없었다" 를 단정할 수 없다.

앞선 두 번의 시도는 이 조건을 만족하지 못해 폐기했다. 2026-08-19 에는 이 스크립트
자신의 EXIT trap 이 서비스를 되살렸고(`flock` 과 조건부 복원으로 수정), 2026-08-20
03:18 에는 다른 세션의 시험이 되살렸다.

## 이 하드웨어의 제약 — 게이트 설계가 이것들을 피해야 한다

### 1. 사이클마다 하드 리셋하면 보드가 재부팅된다

`/etc/watchdog.conf` 의 `watchdog-timeout = 15` 인데 `cam_hard_reset.sh` 는 21.8초가
걸린다. 초안이 사이클마다 리셋하도록 짜여 있었고 **첫 사이클에서 보드가 워치독 리셋**
됐다(pstore 는 비어 있어 커널 패닉이 아니다). 그래서 G4 는 구성당 리셋 1회만 쓴다.

별도로 `run-watchdog-custom.service` 가 `wdt_check 70 10 0` 으로 데이터보드 UART 워치독
70초를 건다.

### 2. 사용자 공간에서 전원을 내릴 수 없다

`v4l2-ctl` 이 종료해도 `s_power(0)` 이 호출되지 않는다. 벤더 캡처 드라이버
(`imx8-isi-cap.c`)에 `s_power, 0` 호출이 아예 없고 해제 경로는 `s_stream(0)` 까지만
간다. 그래서 스트림을 닫아도 `state=CONSUMED` 가 유지되고 `epoch` 도 진행되지 않는다.

**전원을 실제로 내리는 유일한 수단이 하드 리셋이다.** G4 가 "cold 사이클 반복" 이
아니라 "cold 1회 + warm 재사용 반복" 인 이유다. warm 구간에서는 prepare write 를 하지
않는다 - 잔류 `power_count` 위에서 prepare store 는 계약상 거부되기 때문이며, gstApp
연동 설계도 warm 재사용을 "write 없이 상태만 확인" 으로 규정한다.

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
