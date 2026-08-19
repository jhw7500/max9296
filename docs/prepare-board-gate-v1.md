# MAX9296 병렬 prepare 보드 게이트 (v1)

`docs/superpowers/plans/2026-08-13-max9296-parallel-prepare.md` Task 5 Step 4 가
요구하는 판정을 실보드에서 수행하는 절차와 그 실측값이다. `tools/cam_prepare_gate.sh`
가 이 문서의 판정을 그대로 구현한다.

이 경로를 기동 스크립트에 활성화하려면 네 항목이 모두 통과해야 한다.

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

### G4 — dual 50회 통과, single 은 미완(10/50)

```
PASS  dual cold prepare 펌웨어 2건
      dual 10~50/50  성공 50  실패 0
PASS  dual warm 50회 전부 재사용 조건 유지 (CONSUMED/lease=0/match=1)
PASS  single cold prepare 펌웨어 1건
      single 10/50  성공 10  실패 0        <- 보드 반환 요청으로 중단
```

**남은 작업**: single 40 사이클. 약 15분 소요.

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
