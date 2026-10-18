# #110 보드 측정 — AP1302 기동 확인의 거짓 실패와 소요 (2026-10-18)

이슈 #110 의 AC2(정상 보드에서 실제 전원 주기 10회 이상, 매번 검사 실행 로그, 거짓 실패 없음)와
AC5(확인 읽기로 늘어나는 prepare 소요)를 쟀다. AC1·AC3(문제 카메라에서의 검출과 검출 빌드의 재현)은
문제 카메라 보드(192.168.214.6)가 필요해 **여기서 다루지 않는다**.

## 환경

| 항목 | 값 |
|---|---|
| 보드 | pim-camera-v016, 192.168.214.4 (보드 레지스트리 `pim`, exclusive 점유로 측정) |
| 커널 | 5.10.35-lts-5.10.y+g2fce14defc04 |
| 설정 | 두 인스턴스 모두 single mode, `table=right width=1920 height=1080 fps=15 enable=2` (1-0048 → 채널 3, 2-0048 → 채널 1). 검사 주소는 0x3c |
| 원본 모듈 | srcversion `7F99CB433CAC065390A1E71`, sha256 `e6decd0e0f93…` (`/opt/pim/driver/max9296.ko` 와 같음) |
| 새 모듈 | srcversion `546181C5BA2B42CBC2B4094`, sha256 `cfa7752e4d79…` — 커밋 `0c11979` 의 `./make-for-imx8` 빌드 |
| 교체 위치 | `/lib/modules/$(uname -r)/kernel/drivers/media/i2c/max9296.ko` (`modinfo -n max9296` 이 가리키는 곳, `updates/` 없음) |
| 전원 주기 | `systemctl reboot` (S/W 재부팅). 매 부팅의 콜드 초기화는 `start_fw_load` 줄과 `isp_boot.epoch_current:true` 로 확인 |
| 시각 | 09:38–09:54 UTC |

## 절차와 판정

`v110.sh` 가 원본 모듈로 10회 재부팅한 뒤 새 모듈로 바꿔 10회 재부팅하고, 마지막에 원본으로 되돌려
재부팅해 확인한다. 매 부팅마다 새 boot id 와 `cam_state=healthy` 를 기다린 뒤 `dmesg`, 두 인스턴스의
`prepare`·`health_raw`, srcversion 을 모은다. 보드 저널은 `Storage=volatile`, `RuntimeMaxUse=1M` 이라
커널 줄이 금방 밀려나므로 링 버퍼(`dmesg`)를 쓴다.

부팅 하나는 아래를 모두 만족해야 통과다(`a110.py` 의 `verdict()`).

- `cam_state=healthy`, 두 인스턴스 prepare `errno=0`, 다운로드 표식(`end_fw_load`·`loaded … firmware`)이 있다.
- 새 모듈이면 추가로: 인스턴스마다 `AP1302 boot check passed` 줄이 1개 이상, `failed` 줄 0,
  `health_raw` 의 `isp_boot` 가 `OK`·`epoch_current:true`·`stage:post_config` 다.

AC5 구간(인스턴스별, dmesg 타임스탬프):

| 구간 | 내용 |
|---|---|
| `dl` | `start_fw_load` → `end_fw_load` (펌웨어 전송, 대조 구간) |
| `d1` | `end_fw_load` → `loaded … firmware` (`end_fw_load` 의 100 ms 대기. 새 모듈은 여기에 post_download 읽기 1회) |
| `d3` | 첫 `preview addr=` → `AP1302 boot check passed` (새 모듈만. 프리뷰 설정 쓰기와 post_config 읽기 1회) |

pre_download 읽기 앞에는 두 모듈 공통의 커널 줄이 없어 직접 잴 수 없다(`max9296_set_mode` 줄은
`debug` 일 때만 찍힌다). 그래서 읽기 1회 비용을 `d1` 의 차이로 보고 세 읽기의 합을 추정한다.

### 양성 대조

`python3 -I a110.py --selftest` 가 합성 입력 다섯 가지 — 정상, 실패 줄, `isp_boot` FAILED, prepare
`errno=-5`, 표식 누락 — 에서 각각 기대한 판정을 낸다(`SELFTEST PASS`). 실제 dmesg 표식 파싱은 첫
시도(아래)의 원본 부팅 로그에서 `dl`·`d1` 이 추출되는 것으로 확인했다. 실기에서 검사가 실패하는 양성
대조(문제 카메라)는 이 측정에 없다 — 그 검출 능력은 호스트 테스트(`tests/max9296_isp_boot_test.py`)와
.6 에서의 AC1·AC3 몫이다.

## 결과

| 구간 | 부팅 | 판정 |
|---|---|---|
| 원본 모듈 | 10 | 10/10 healthy, `errno=0` |
| 새 모듈 | 10 | 10/10 통과 — 매 부팅 통과 줄 2(인스턴스마다 1), 실패 줄 0, `isp_boot` `OK/current/post_config/1920` 두 인스턴스, `errno=0`, healthy |
| 복구 | 1 | srcversion `7F99CB433CAC065390A1E71`, healthy |

| 구간 | 원본 (n=20) | 새 모듈 (n=20) |
|---|---|---|
| `dl` | 중앙 3850.1 ms (3765.4–3931.9) | 중앙 3878.1 ms (3823.3–4041.1) |
| `d1` | 중앙 106.8 ms (104.5–111.5) | 중앙 107.6 ms (105.8–113.2) |
| `d3` | — | 중앙 4.9 ms (4.4–8.7) |

- **AC2**: 새 모듈 실제 전원 주기 10회에서 거짓 실패 0, 매 부팅 검사 실행 로그가 있다. 단일 모드
  (0x3c) 경로에서의 결과이며, 다운로드 전·후 단계값 자체는 기록하지 않았다(통과 줄은 설정 후 값만 찍는다).
  설정 후 값 `0x0780`(1920)은 펌웨어 기본값과 같아, 이 설정에서 설정 후 단계는 "설정 미반영" 을 가리지
  못한다(`docs/health-raw-v1.md` 한계와 같다).
- **AC5**: `d1` 중앙값 차이 +0.8 ms. 두 분포의 범위가 겹쳐 읽기 1회 비용은 `msleep` 지터 수준이라고만
  말할 수 있다. 콜드 초기화당 세 읽기의 증가는 약 2–3 ms 로 **추정**한다(`dl` 의 차이는 전송 자체의
  흔들림이며 읽기와 무관한 구간이다).

## 측정 도구 결함 (첫 시도, 결과에 넣지 않음)

09:36 의 첫 시도는 원본 1회차에서 멈췄다. `/tmp/cam_state/state` 가 줄바꿈 없이 끝나 수집 파일에서
다음 줄과 붙었고(`healthy== 1-0048`), 판정이 이를 비정상으로 읽었다. 모듈 교체 전이라 보드는 원본
그대로였다. 수집을 값마다 `printf "%s\n"` 로 끝내도록 고친 뒤 처음부터 다시 쟀다
(`logs/aborted-093635-summary.txt`).

## 파일

| 경로 | 내용 |
|---|---|
| `v110.sh` | 보드 측정 스크립트 (인자: 새 `.ko`, 새 srcversion, 출력 디렉터리). 측정 뒤 ShellCheck SC2034 때문에 횟수만 세는 두 대기 루프의 변수 이름을 `t` 에서 `_` 로 바꿨다 — 측정에 쓴 파일과는 그 두 줄과 `local` 선언만 다르다 |
| `a110.py` | 판정·집계 분석기 (`--selftest` 양성 대조) |
| `logs/summary.txt` | 측정 진행 기록 |
| `logs/analysis.txt` | 아래 재현 명령의 출력 |
| `logs/{orig,new}-N/`, `logs/restore/` | 부팅별 `state.txt`(srcversion, cam_state, prepare, health_raw)와 `dmesg.txt`(`[I2C:n]`·`[RST:n]`·`max9296 N-0048:` 줄만 거른 것) |
| `raw/` | 거르기 전 원본(미공개, gitignore) |

## 재현

```bash
cd artifacts/board-20261018-issue110
python3 -I a110.py --selftest      # SELFTEST PASS
python3 -I a110.py logs            # BOOT_VERDICT PASS (bad boots 0), logs/analysis.txt 와 같다
```
