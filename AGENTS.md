# AGENTS.md — max9296 에이전트 작업 지침

Claude Code 와 Codex 가 공유하는 저장소 지침이다. Claude 는 `CLAUDE.md` 의 `@AGENTS.md`
import 로 이 파일을 읽는다. 사실의 정본은 `README.md` 와 각 문서이고, 이 파일은
**에이전트가 틀리기 쉬운 것만** 적는다. README 와 어긋나면 README 를 따르고 이 파일을 고친다.

## 무엇인가

i.MX8MP 보드용 GMSL2 역직렬화기(MAX9296) + AP1302 ISP 카메라의 리눅스 커널 외부 모듈이다.
소스는 `max9296.c` 하나와 정책 헤더 셋(`max9296_360p_policy.h`, `max9296_exposure_policy.h`,
`max9296_pair_health.h`)이고, 산출물은 `max9296.ko` 다. `tools/` 는 보드에서 돌리는
측정·복구 스크립트, `tests/` 는 보드 없이 호스트에서 도는 테스트다.

## 명령

```bash
cp .env.example .env            # 처음 한 번. 호스트별 경로를 채운다 (README §빌드)
./make-for-imx8                 # 빌드 (Yocto SDK). CI 는 make KERNEL_SRC=... 를 직접 부른다
bash tests/run_health_tests.sh  # 테스트 전체. 보드 불필요. cc·python3·git·jq·rg 필요
./update_bin.sh                 # 빌드된 .ko 를 형제 저장소 pim-package-jhw 에 배포 (README §배포)
```

- 테스트는 위 하나가 진입점이다. 개별 테스트와 방식(픽스처 / 정적 소스 검사 / C 단위)은
  README §테스트 표를 본다.
- `run_360p_readout_compare_test.sh` 가 찍는 `ERROR: production restore failed` 는 실패 경로를
  일부러 태우는 픽스처다. 러너가 `PASS` 로 끝나면 통과다.
- `update_bin.sh` 는 다른 저장소의 작업 트리와 매니페스트를 바꾼다. 사용자가 배포를 요청한
  경우에만 실행한다.

## 커밋하지 않는 것

전부 `.gitignore` 에 있다. 스테이징 전에 `git status` 로 확인한다.

- `.env` — 호스트별 경로. 정본은 `.env.example` 이며 새 키를 추가하면 그 파일을 고친다.
- 빌드 산출물 — `*.ko`, `*.o`, `*.mod*`, `.*.cmd`, `Module.symvers`, `modules.order`
- `compile_commands.json`, `.cache/` — 빌드가 만드는 clangd DB. 호스트 경로를 담는다.
- `HANDOFF.*.md` — 세션 체크포인트. 내부 호스트·경로 정보를 담는다.
- `artifacts/board-*/raw/`, `artifacts/board-*/**/backup/`, `artifacts/board-*/**/edgeconf-*.json`
  — 원시 로그와 백업. 자격증명이 들어갈 수 있다. 커밋하는 것은 검토된 요약·증적만이다.
  다른 위치의 edgeconf 는 ignore 되지 않으므로 직접 확인한다.

## 실행 비트를 바꾸지 않는다

`tests/build_360p_candidates_test.sh` 가 추적 파일 14개의 git 실행 비트(100755)를 게이트한다.
목록의 정본은 그 테스트 파일 머리의 `for executable in` 블록이다 — `tools/` 의 스크립트 4개
(`build_360p_candidates.sh`, `cam_360p_resource.sh`, `uyvy_frame_check.py`, `rgb565_frame_check.py`)와
`tests/fixtures/` 의 가짜 명령 10개다. `tools/` 의 나머지 파일은 게이트 밖이고 mode 가 섞여
있다 — `run_360p_readout_compare.sh` 와 `max9296_health_export.py` 는 100755, `cam_fps_*.sh` 넷과
`cam_hard_reset.sh`·`cam_prepare_gate.sh` 는 100644 이며 `bash` 로 부른다. 어느 쪽이든 기존 mode 를 바꾸지 않고, 스크립트를 고친 뒤 `git diff --summary`
로 mode 변경이 없는지 본다. 새 스크립트를 게이트 목록에 넣을 때만 `chmod +x` 와 목록 추가를
함께 한다.

## 파괴적 도구

`tools/cam_hard_reset.sh` 는 max9296 모듈과 CSI2 를 재적재하고 `-s` 를 주면 `cam-operate.service`
를 정지시킨다. `tools/cam_prepare_gate.sh` 는 서비스를 정지시킨 뒤 하드 리셋을 건다. 둘 다
운영 중인 보드의 캡처를 끊으므로 사용자의 명시적 요청 없이 보드에서 실행하지 않는다.

**같은 basename 의 다른 파일이 있다.** pim-package 가 배포하는 `/opt/pim/bin/cam_hard_reset.sh`
는 이 저장소의 도구가 아니라 deprecated 호환 래퍼이고 `-s`/`-S` 동작이 다르다. 보드에서
어느 경로의 파일인지 확인한 뒤에만 그 동작을 서술한다 (README §문서 지도 경고).

## 하드웨어 주장은 측정 전까지 가설이다

이 저장소의 문서는 보드 실측을 담는다. 그 기준을 지킨다.

- 측정하지 않은 FPS·노출·레지스터 동작·복구 결과는 "추정" 으로 표기하고, 측정값과 섞어 쓰지
  않는다. 수치는 어느 보드·어느 명령·어느 로그에서 나왔는지와 함께 적는다.
- 커널 로그 건수를 계수 근거로 쓰기 전에 `printk_ratelimited` 여부를 확인한다 (README §알려진 제약).
- 소스를 읽어 "이렇게 동작할 것이다" 라고 쓸 때는 함수 이름과 위치를 함께 적어 독자가 반증할 수
  있게 한다.
- 실기 측정을 담은 변경은 자동 리뷰가 검증 불가로 판정할 수 있다. 측정 로그를 `artifacts/` 에
  증적으로 두고 문서에서 가리킨다.

## 정본 문서

| 주제 | 정본 |
|---|---|
| 버전 이력 | `CHANGELOG.md` (Keep a Changelog, 항목은 한국어). 동작 변경은 여기에 먼저 적는다 |
| V4L2 컨트롤 | `V4L2_CTRL_GUIDE.md` |
| sysfs·모듈 파라미터 | `README.md` §런타임 인터페이스 |
| health ABI | `docs/health-raw-v1.md` |
| 병렬 prepare ABI | `docs/parallel-prepare-v1.md` |
| 빌드·테스트·배포·제약 | `README.md` |

`docs/v4l2-controls-guide.md` 는 요약본이다. 상세를 고칠 때는 `V4L2_CTRL_GUIDE.md` 를 고친다.

## 언어와 커밋

- 커밋 메시지, 문서, 스크립트·Makefile 주석은 한국어로 쓴다. `max9296.c` 의 주석은 기존대로
  영문이다 — 그 톤을 따른다.
- 커밋 제목은 `type: 설명` 형식이다 — `fix:`, `docs:`, `feat:`, `test:` 등. 이슈가 있으면
  `(#이슈)` 를, 리뷰 반영 커밋은 `(#이슈 리뷰 N)` 또는 `(#이슈 PR 리뷰 N)` 을 붙이고, 이슈가
  없으면 `(리뷰 N)` 만 붙인다. 최근 `git log` 가 예다.
- 커밋 제목에는 **무엇을 왜** 바꿨는지 적는다. 기존 로그가 그 톤의 예다.
- 변경은 요청 범위에 한정한다. 주변 코드의 리팩토링·주석 추가·스타일 정리는 하지 않는다.

## CI 와 자동 리뷰

- `build-test.yml` — Ubuntu 커널 헤더로 `make KERNEL_SRC=...` 빌드, `modinfo`, 심볼, sparse.
- `contract-test.yml` — `bash tests/run_health_tests.sh`.
- `shellcheck.yml` — 셸 스크립트 정적 검사. 경고를 억제할 때는 `# shellcheck disable=` 뒤에
  이유를 적는다 (예: `make-for-imx8` 의 SC1090).
- PR 에는 자동 리뷰가 붙는다. 워크플로가 있는 것은 Claude·Gemini·opencode 이고, Codex 는
  워크플로 없이 GitHub App 으로 리뷰를 남긴다. Claude·Gemini 코멘트는 제자리에서 갱신되므로
  새 코멘트가 아니라 기존 코멘트의 변경을 본다. Codex 리뷰는 라운드마다 새로 생긴다.
