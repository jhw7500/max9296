# #110 보드 측정 — 문제 카메라에서의 AP1302 기동 실패 검출과 재현 (2026-10-19)

이슈 #110 의 AC1(유형 A·B 재현 부팅에서 prepare 가 0 이 아닌 errno 를 돌려주고 커널 로그와
`health_raw` 에 채널·단계가 남는다)과 AC3(검출 빌드에서도 문제 카메라의 실패가 재현된다 — 양성 대조)을
쟀다. dual-wide 경로(0x11/0x12)의 실기 확인도 겸한다. AC2·AC5(정상 보드, single mode)는
`artifacts/board-20261018-issue110/` 몫이다.

## 환경

| 항목 | 값 |
|---|---|
| 보드 | 192.168.214.6 (문제 카메라 장착). 보드 레지스트리에 없어 잠금 없이 사용자 승인으로 측정 |
| 커널 | 5.10.35-lts-5.10.y+g2fce14defc04 |
| 설정 | 두 인스턴스 모두 dual-wide, gstApp prepare `tuple=2560x720@15 enable=3`. 검사 주소 0x11/0x12, 설정 후 기대값 ISP당 0x0500(1280) |
| 원본 모듈 | srcversion `7F99CB433CAC065390A1E71` — `/opt/pim/driver/max9296.ko`(패키지 원본, .4 측정의 원본과 같은 srcversion) |
| 새 모듈 | srcversion `0938DCF9B88888E03FAF05B`, sha256 `ed0f2e54bb6b…` — master `172a76a`(PR #116 머지) 의 `./make-for-imx8` 빌드 |
| 측정 전 보드 모듈 | srcversion `898534AABBA3AA941B748B7`, sha256 `9f197c445f9f…` — 측정 뒤 이것으로 되돌려 확인했다 |
| 교체 위치 | `/lib/modules/5.10.35-lts-5.10.y+g2fce14defc04/kernel/drivers/media/i2c/max9296.ko` |
| 전원 주기 | `systemctl reboot` (S/W 재부팅). 매 부팅 가동 75 s 이후 수집 |
| 시각 | 2026-10-18 16:48–17:47 UTC (2026-10-19 01:48–02:47 KST) |

## 절차

`v110b.sh` 가 측정 전 모듈(srcversion·sha)과 패키지 원본의 srcversion 을 확인하고, 측정 전 모듈을
보드에 백업한 뒤 새 모듈 사본의 sha 를 대조한다. 그다음 원본으로 15회, 새 모듈로 15회 재부팅하고,
끝나면(중간에 멈춰도 `trap`) 측정 전 모듈로 되돌려 재부팅해 확인한다. 매 부팅 새 boot id 와 가동 75 s 를
기다린 뒤 다음을 모은다.

- `state.txt`: boot id, srcversion, uptime, `/tmp/cam_state/state`, 부팅 시각(UTC), 두 인스턴스의
  `prepare`·`health_raw`, **사용자 공간에서 읽은 ISP별 R0x2000**
  (`i2ctransfer -f -y <bus> w2@<addr> 0x20 0x00 r2`, bus 1 = 1-0048, bus 2 = 2-0048), CSI/ISI 인터럽트 수.
- `dmesg.txt`: 커널 링 버퍼. 보드 저널이 `RuntimeMaxUse=1M` 이라 링 버퍼를 쓴다.
- `prepare-log.txt`: `/var/log/cantops/local0.log` 의 gstApp `[MAX9296_PREPARE]` 마지막 30줄(KST).

R0x2000 은 검출 코드와 별개의 정답으로 쓴다. 0x0500 이면 설정까지 끝난 ISP, 0x0280 은 부트 ROM 값
(펌웨어 미기동), 0x0000 은 기동 전 상태다.

## 판정 (`a110c.py`)

- **정답 없음(`link_down`)**: 75 s 의 네 주소 읽기가 모두 실패하면 그 부팅은 정답이 없다. 실제로 그런
  부팅 6회는 모두 `No such device or address` 였고, 새 모듈 4회는 두 인스턴스 `isp_boot.epoch_current`
  가 `false`(마지막 콜드 초기화 뒤 전원 epoch 가 바뀜)였다. 그 가운데 new-2 의 2-0048 은 드라이버가
  63.3 s 에 두 ISP 를 0x0500 으로 통과시켰는데 77 s 읽기는 ENXIO 였다 — 이 시점의 읽기는 기동 상태가
  아니다. 링크가 왜 내려갔는지(두 번 실패한 뒤 카메라 경로가 꺼졌다는 기전)는 **추정**이다.
- **시도 대조(새 모듈)**: 인스턴스별로 `AP1302 boot check failed/passed` 줄을 2 s 간격으로 묶어 시도로
  보고(실패 줄이 있으면 F, 통과 줄만 있으면 P), 같은 부팅의 prepare 기록(`action=2` 재사용 제외)을
  `primary_errno` 로 F/P 로 바꿔 순서가 같아야 한다. 마지막 시도는 `health_raw` `isp_boot` 와 같아야
  한다 — F 면 `FAILED`, 단계 일치, 실패 채널의 errno·값이 커널 로그와 같고 마지막 prepare errno 가 0 이
  아님. P 면 `OK/post_config`, 모든 채널 errno 0, 값 0x0500.
- **놓침·거짓 실패**: 정답이 있는 인스턴스에서 정답은 기동 안 함인데 마지막 시도가 P 면 놓침,
  정답은 정상인데 `epoch_current:true` 로 `FAILED` 면 거짓 실패.
- **원본 모듈**: 검출이 없으므로 정답이 기동 안 함인데 그 인스턴스의 마지막 prepare 가 `errno=0` 이면
  침묵 실패로 센다.

### 양성 대조

`python3 -I a110c.py --selftest` 의 합성 입력 9건 중 결함이 있어야 할 6건 — 놓침(0x0280), 한 주소만
ENXIO 인 놓침, `health_raw` 불일치, 실패 시도의 prepare `errno=0`, 검사 줄 없음, 거짓 실패 — 이 각각
기대한 이슈로 판정된다. 한 주소만 ENXIO 인 경우가 `link_down` 으로 빠지지 않는 것도 여기서 본다.
정답 계측(75 s 읽기)의 검출 능력은 원본 모듈 단계에서 침묵 실패 11회를 잡은 것으로 실기에서 확인된다.

## 결과

| 단계 | 부팅 | 정답 있음 | 결과 |
|---|---|---|---|
| 원본 | 15 | 13 | 침묵 실패 11/13 — 1-0048 0x11 = 0x0280 10회, 2-0048 0x12 = 0x0000 2회(base-5 는 둘 다). base-1·4 는 1-0048 prepare 가 두 번 `errno=-6` 뒤 `link_down` |
| 새 모듈 | 15 | 11 | 실패 시도가 있는 부팅 6/15(모두 첫 시도), 실패 줄 10. 시도 대조 불일치 0, 놓침 0, 거짓 실패 0 |
| 복구 | 1 | — | srcversion `898534AABBA3AA941B748B7`, sha256 `9f197c445f9f…`, `cam_state=healthy` |

새 모듈의 실패 시도(전부 `errno -5`):

| 인스턴스 | 단계 | 주소 | R0x2000 | 횟수 | 부팅 |
|---|---|---|---|---|---|
| 1-0048 (채널 2) | post_download (유형 A) | 0x11 | 0x0280 | 7 | new-2·10·12 각 2(재시도도 실패), new-4 1(재시도 성공) |
| 2-0048 (채널 1) | pre_download (유형 B) | 0x12 | 0x0000 | 3 | new-6 2(재시도도 실패), new-9 1(재시도 성공) |

- 두 시도가 모두 실패한 4회는 그 인스턴스 `health_raw` 가 `FAILED`, 단계·채널·`errno -5`·값
  (640 또는 0)이 커널 로그와 같았다. 재시도로 회복한 new-4·9 는 `OK/post_config`·`epoch_current:true`
  다 — `isp_boot` 는 마지막 콜드 초기화를 싣는다(`docs/health-raw-v1.md`). 앞선 실패는 커널 로그와
  prepare 로그에 남는다.
- 통과 줄은 모두 `R0x2000 0x0500 (expected 0x0500)` 이다 — dual-wide 의 설정 후 단계가 실기에서 통과했다.
- 다운로드 전 단계는 ROM 값 0x0280 을 통과로 본다(`max9296_isp_boot_value_ok()` 의
  `PRE_DOWNLOAD` 는 `value != 0`). new-6 의 채널 0 이 그 예다.
- `/tmp/cam_state/state` 는 30회 모두 `healthy` 였다 — 두 시도가 모두 실패한 4회 포함. 그 값의 의미는
  이 측정의 범위 밖이다(jhw7500/pim-package-jhw#192).

성공한 콜드 prepare(`action=4`, `errno=0`)의 gstApp `elapsed_ms`:

| 인스턴스 | 원본 | 새 모듈 |
|---|---|---|
| 1-0048 | 중앙 12235 (12189–12275, n=13) | 중앙 12243.5 (12172–12299, n=14) |
| 2-0048 | 중앙 12231.5 (12185–12275, n=18) | 중앙 12232.0 (12192–12267, n=18) |

차이는 범위 안이다. 두 모듈은 판이 달라 이 차이를 확인 읽기 비용으로 볼 수 없다(AC5 는 .4 측정).

## AC 판정

- **AC1 충족**: 유형 A(다운로드 뒤 0x0280)·유형 B(다운로드 전 0x0000) 재현 부팅에서 prepare 가
  `errno=-5` 로 끝났고, 커널 로그에 채널·단계·주소·값이, 실패로 끝난 인스턴스의 `health_raw` 에 같은
  채널·단계가 남았다. 설정 후 단계의 실패는 실기에서 나오지 않았다(호스트 테스트로만 확인).
- **AC3 충족**: 검출 빌드에서도 15회 중 6회 실패가 재현됐다. 유형 A 비율은 원본 10/13(75 s 최종 상태),
  새 모듈 4/15(첫 시도)로 달랐다. 이슈가 경고한 "확인 읽기가 타이밍을 바꾼다" 와 같은 방향이지만,
  두 단계는 모듈 판과 계측이 다르고 같은 부팅의 짝 대조가 아니어서 원인을 가리지 못한다. 15회 표본으로는
  비율 차이 자체도 판정하지 않는다.
- **dual-wide**: 0x11/0x12 경로의 세 단계 검사가 실기에서 돌았다(AC4 에 남았던 공백).

## 측정 도구 결함 (첫 분석기, 결과에 넣지 않음)

첫 분석기 `a110b.py` 는 AC1 을 FAIL 로 냈다. 두 결함 때문이다. (1) 네 주소가 모두 ENXIO 인
`link_down` 부팅을 정답 "기동 안 함" 으로 셌다. (2) 정답상 실패 인스턴스의 모든 채널에 errno 를
요구해, 드라이버가 정상으로 본 짝 채널까지 누락으로 셌다. 고친 `a110c.py` 로 다시 판정했다. 분석기를
쓰지 않고 원자료를 직접 파싱한 별도 검증에서 위 수치(정답 없는 부팅, 침묵 실패, 실패 줄, 시도 순서,
`health_raw` 일치, `elapsed_ms` 중앙값, srcversion)가 모두 같았다. `a110b.py` 는 `raw/` 묶음에 있다.

## 한계

- 원본과 새 모듈은 서로 다른 부팅이다. 같은 부팅에서 원본이 놓친 실패를 새 모듈이 잡았다는 짝 증거는 없다.
  새 모듈이 잡은 두 ISP 가 원본에서 침묵 실패한 두 ISP 와 같다는 것은 정황이다.
- 가동 75 s 한 시점이다. 두 시도가 모두 실패한 4회에서 그 뒤 cam-operate 복구가 어떻게 끝났는지는 재지 않았다.

## 파일

| 경로 | 내용 |
|---|---|
| `v110b.sh` | 보드 측정 스크립트 (인자: 새 `.ko`, 새 srcversion, 출력 디렉터리; `N` 기본 15). 측정에 쓴 파일 그대로다 |
| `a110c.py` | 판정·집계 분석기 (`--selftest` 양성 대조) |
| `logs/summary.txt` | 측정 진행 기록 |
| `logs/analysis.txt` | 아래 재현 명령의 출력 |
| `logs/{base,new}-N/`, `logs/restore/` | 부팅별 `state.txt`, `prepare-log.txt`, `dmesg.txt`(`max9296 N-0048:`·`loaded … firmware`·`start_fw_load`·`end_fw_load` 줄만 거른 것) |
| `raw/` | 거르기 전 원본과 첫 분석기(미공개, gitignore) |

## 재현

```bash
cd artifacts/board-20261019-issue110-dual
python3 -I a110c.py --selftest      # SELFTEST PASS
python3 -I a110c.py logs            # AC1_VERDICT PASS, logs/analysis.txt 와 같다
```
