# #111 결함 A·B 수정 — 실기 검증 요약 (2026-10-17)

이슈 #111 의 Acceptance criteria 를 실기에서 확인했다. 결함 A 는 정지한 스트림의
`state.fsync` 가 RUNNING 으로 되살아나는 것, 결함 B 는 1-0048 이 멈춘 뒤 2-0048 단독
STREAMON 이 FSYNC 를 받지 못하는 것이다. 원시 로그는 같은 디렉터리의 `raw/`(gitignore,
미공개)에 있고, 이 문서와 두 스크립트가 커밋되는 증적이다.

## 환경

| 항목 | 값 |
|---|---|
| 보드 | `pim` (pim-camera-v016, 192.168.214.4, i.MX8MP, max9296 4ch) |
| 측정 모듈 | 수정 커밋 `ccf2ccd` 와 같은 코드 변경에 상태 전환 로그(T111)만 더한 계측 빌드, srcversion `D8CFC4AB5E88B2E0574E148` |
| 계측 내용 | `s_stream` 반환값과 두 인스턴스 스냅샷, FSYNC 분기 결정(변화 시만), `state.fsync` 의 RUNNING 전환(`max9296_fsync_pulse_current()` 잠금 안에서 직전 값·epoch 유효 여부를 잡고 잠금 밖에서 출력), enable 워커 게이트. 상태 대입 위치·순서는 수정본과 같다. 계측 코드는 커밋하지 않았다(로컬 브랜치 `debug/111-fix-trace`, 기반 `debug/111-fsync-trace` `5defc43`) |
| 측정 뒤 원복 | 원본 srcversion `7F99CB433CAC065390A1E71` 설치·재부팅, 두 버스 `CSI output enabled`, 4채널 `mp4,srt file chk ok` 확인 |
| 노드 | `video3` = 1-0048 (I2C:1, CSI0 `32e40000`), `video4` = 2-0048 (I2C:2, CSI1 `32e50000`) |

## 판정 방법과 양성 대조

판정은 `fix111_check.py` 가 `journalctl -o short-iso-precise` 캡처에서 한다.

- A1: 버스마다 `s_stream(0)` 반환 뒤 다음 `s_stream(1)` 반환 전의 RUNNING 전환, `STALE` 표시, 정지·시작 반환 스냅샷의 자기 `f`(= `state.fsync`)가 RUNNING 인 경우를 센다.
- A2: `s_stream(1) ret=0` 사이클마다 그 버스의 첫 `fsync->RUNNING` 이 첫 `CSI output enabled`(0x0313) 보다 먼저인지 본다.
- `--gap`: `v111.sh` 가 50 ms 간격으로 남긴 CSI IRQ 누계에서, 계속 도는 쪽의 증가 간 최대 공백을 상대 정지 뒤 4 s·재시작 뒤 6 s 구간별로 잰다.

양성 대조로 같은 분석기를 2026-10-16 수정 전 코드의 계측 로그 두 개에 먼저 돌렸다.

| 로그 (수정 전, srcversion `6963B356`) | STALE | 정지 반환 시 RUNNING | 시작 반환 시 RUNNING 승계 | 순서 위반 |
|---|---|---|---|---|
| boot `56cd3d09` | 22 | 22 | 9 | 9 |
| boot `5a00b6a6` | 26 | 26 | 13 | 13 |

"정지 중 RUNNING 전환" 지표는 수정 전 로그에서도 0 이었다(늦은 대입이 `s_stream(0)` 반환 로그보다 먼저 찍힌다). 그래서 판정은 `f` 와 순서 지표로 한다.

## 결과

| AC | 명령 | 결과 |
|---|---|---|
| A: 정지 순서 2종 × 10 | `v111.sh` A 단계 (cam-operate 정지, `v4l2-ctl` 두 노드 8 s 스트리밍 뒤 1..10 은 video3 먼저, 11..20 은 video4 먼저 정지) | 시작 40·정지 40, 정지 중 RUNNING 0, STALE 0, 정지·시작 반환 시 RUNNING 0, 순서 위반 0. 매 사이클 두 노드 14.64~14.98 fps |
| A: 첫 0x0313 이 첫 펄스 뒤 | 위 A 단계 + 아래 회귀·부팅 전 구간 | 순서 위반 0. commit→첫 RUNNING 1.00~1.51 s, commit→0x0313 1.33~1.86 s |
| B: 1-0048 정지 뒤 2-0048 단독 | `v111.sh` B1, `v111b.sh`, `v111c.sh` (1-0048 6 s 단독 뒤 정지 → 2-0048 단독 90 프레임) | 5/5 (깨끗한 부팅 5개에서 각 1회), 14.95~14.98 fps |
| 회귀: gstApp 정상 종료 재기동 | 보드 캠페인 `TERM` × 10 (스트리밍 20~40 s 뒤 gstApp·ord·vcm SIGTERM) | 10/10 두 버스 출력 활성화, `s_stream(1)` 오류 0, NO DATA 0 |
| 회귀: 강제 종료 재기동 | 같은 캠페인 `KILL` × 10 | 10/10 같음 |
| 회귀: 부팅 | 재부팅 10회 (boot `97a82151` `00ee40f4` `ed492050` `11d3ea7c` `271d0417` `3cdd1ba7` `0a0b94e5` `aca736ca` `f2fcd4d3` `7a020eba`) | 10/10 두 버스 `ret=0`, 순서 위반 0, NO DATA 0 |
| 회귀: 2-0048 정지 상태의 1-0048 단독 | `v111.sh` B 단계 앞부분 | 3/3, 55 프레임 14.95~14.96 fps |
| 정지 중 공유 FSYNC 펄스의 무해성 | 2-0048 단독 구간 뒤 `health_raw`, `v111b.sh` 의 1-0048 시작·재시작 | 1-0048 디시리얼라이저·ISP·링크 OK, 시작·재시작 4회 각 55 프레임 14.97~14.99 fps |

상대 정지·합류 중 스트리밍 쪽 공백 (`--gap`):

| 계속 도는 쪽 | 상대 정지 뒤 4 s 최대 공백 | 상대 재시작 뒤 6 s 최대 공백 |
|---|---|---|
| video3 (1-0048), video4 3회 정지·재시작 | 0.07~0.14 s | 1.01~1.08 s |
| video4 (2-0048), video3 3회 정지·재시작 (깨끗한 부팅) | 0.14 s | 1.02~1.08 s |

재시작 쪽 공백은 dual 분기 워밍업(1 s)이다. 부팅 때 두 쪽이 차례로 시작할 때 이미 있던 동작이다.

## 별도 관측 (이 변경과 무관, 원인 미규명)

같은 부팅에서 `v4l2-ctl` 단독 시험을 반복하면 2-0048 이 한 번 단독으로 성공한 뒤부터
CSI1 수신 IRQ 가 0 이 됐다. 그 동안 max9296 은 `s_stream(1) ret=0`, FSYNC 결정 `SIDE_PEER`,
RUNNING 전환, `CSI output enabled` 를 모두 실행했고, 2-0048 `health_raw` 는
`streaming:true`·두 채널 링크 OK·HINF 1 s 에 +15·`pair ALIGNED` 였다. MAX9296 MIPI 출력에서
i.MX CSIS 사이의 상태로 보인다. 수정 전 코드(2026-10-16)에서도 CSI0 에서 같은 모양이 나왔고,
gstApp 경로 20회와 부팅 10회에서는 나오지 않았으며 재부팅 뒤에는 정상이었다. 그래서 B 는
같은 부팅에서 반복하지 않고 깨끗한 부팅마다 1회 쟀다.

## 재현

- 보드에서: cam-operate·ord-operate 정지 뒤 `bash v111.sh` (결과는 `/root/t111/v111-<시각>/`).
  `journalctl -b -n all -f -o short-iso-precise` 를 먼저 띄워 둔다.
- 호스트에서: `python3 fix111_check.py <follow.log> [<since> <until>] [--gap <v111 결과 디렉터리>]`.
  마지막 줄 `JOURNAL_VERDICT PASS|FAIL`.
