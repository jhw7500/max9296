# max9296 — i.MX8MP GMSL2 카메라 드라이버

MAX9296 GMSL2 Deserializer + AP1302 ISP + AR0234 센서 카메라를 i.MX8MP에
연결하는 Linux V4L2 subdev 드라이버다. NXP BSP 커널 5.10.35(Yocto hardknott)용
아웃오브트리 모듈로 빌드한다.

```
AR0234 Sensor -> AP1302 ISP -> MAX9295 Serializer
              -> GMSL 케이블 -> MAX9296 Deserializer
              -> i.MX8MP CSI2 / ISI / DMA -> GStreamer
```

| 항목 | 값 |
|---|---|
| 드라이버 버전 | 2.12 (`SW_VERSION`, `max9296.c:46`) |
| 대상 커널 | linux-imx 5.10.35 (NXP BSP) |
| vermagic | `5.10.35-lts-5.10.y+g2fce14defc04 SMP preempt mod_unload modversions aarch64` |
| DT compatible | `maxim,max9296` |
| 라이선스 | GPL |
| 미디어 포맷 | `MEDIA_BUS_FMT_UYVY8_2X8` |
| 필요 펌웨어 | `v4l-ap1302-ar0234.fw` |

---

## 처음 오셨다면

1. **쓰는 법** → [`V4L2_CTRL_GUIDE.md`](V4L2_CTRL_GUIDE.md) — 런타임 제어의 중심 문서다.
   subdev 노드 ↔ 채널 매핑, 컨트롤 ↔ AP1302 레지스터 매핑, 고정점 스케일, 적용 절차가 있다.
2. **뭐가 바뀌었나** → [`CHANGELOG.md`](CHANGELOG.md)
3. **왜 그렇게 됐나** → `docs/` 아래 ABI 규격과 보드 실측 문서 (§문서 지도)

---

## 빌드

```bash
./make-for-imx8            # 빌드
./make-for-imx8 clean      # 정리
```

Yocto SDK 환경을 source한 뒤 `ARCH=arm64 CROSS_COMPILE=aarch64-poky-linux-`로
모듈을 빌드한다. 경로 전제는 셋인데 **override 방법이 서로 다르다.**

| 변수 | 기본값 | 바꾸는 법 |
|---|---|---|
| `SDK_LOC` | `/shared/fsl-imx-xwayland/5.10-hardknott` | 환경변수 (`make-for-imx8:4`) |
| `SDK_NAME` | `cortexa53-crypto-poky-linux` | 환경변수 (`make-for-imx8:5`) |
| `KERNEL_SRC` | `/opt/desktop/build-desktop/.../linux-imx-5.10.35+git999` | **환경변수 안 먹는다** — 아래 참조 |

`make-for-imx8:15`는 `KERNEL_SRC`를 조건 없이 대입한 뒤 `:20`에서 `make`에 명시로
넘긴다. 따라서 래퍼를 쓰는 한 환경변수는 무시된다. 다른 커널 트리를 쓰려면 그 줄을
직접 고치거나, 래퍼를 건너뛰고 `Makefile`의 `?=`(`Makefile:1`)를 쓴다.

```bash
KERNEL_SRC=/다른/커널/트리 make ARCH=arm64 CROSS_COMPILE=aarch64-poky-linux-
```

SDK 환경설정 파일(`${SDK_LOC}/environment-setup-${SDK_NAME}`)이 없으면 즉시
중단하고 확인할 경로를 알려준다.

산출물은 저장소 루트의 `max9296.ko`다. 빌드 산출물은 모두 `.gitignore` 대상이라
커밋되지 않는다.

**부수 효과 — clangd:** 빌드가 성공하면 `.cmd` 파일만 파싱해서
`compile_commands.json`을 자동 갱신한다(컴파일 없음, 약 0.05초). 호스트 종속
경로를 담으므로 커밋하지 않고 각자 빌드할 때 생성된다. clangd가 GCC 전용 플래그를
무시하려면 저장소의 `.clangd`도 함께 필요하다.

갱신을 건너뛰는 경우가 몇 가지 있다 — 인자에 `clean`/`distclean`/`mrproper`/`realclean`이
섞였을 때, 빌드가 실패했을 때, `gen_compile_commands.py`를 못 찾았을 때(`CC_GEN`으로
지정 가능). 이때 기존 DB가 있으면 "직전 성공 빌드 기준이라 낡았을 수 있다"고 알리고,
DB가 아예 없으면 조용히 넘어간다.

**현재 빌드 상태:** 깨끗한 체크아웃에서 error 0 / warning 15. 경고는 전부
`-Wunused-function`(미사용 정적 함수)이다.

---

## 테스트

```bash
bash tests/run_health_tests.sh
```

이 하나가 진입점이고 11개 테스트 파일을 모두 돌린다. **보드가 필요 없다.** 대신
호스트에 `cc`, `python3`, `git`, `jq`, `rg`(ripgrep)가 있어야 한다.

보드 없이 도는 방식은 테스트마다 다르다 — 일부는 `tests/fixtures/`의 가짜
`v4l2-ctl`/`media-ctl`/`i2ctransfer`/`perf`를 `PATH`에 얹고, 일부는 `MAX9296_COMPARE_*`
환경변수로 스텁을 주입하며, 일부는 `max9296.c`를 **텍스트로 읽어 정적 검사**한다.

| 테스트 | 방식 | 검증 대상 |
|---|---|---|
| `run_360p_policy_test.sh` | C 단위 테스트 | `max9296_360p_policy.h` 41 checks — 모드별 FPS 상한(360p 120 / HD 60 / FHD 30), 고속 수동 노출이 거부 없이 경고하는지, HD 상한 상향이 360p 고속 preview 경로로 새지 않는지, full-FOV ROI 정규화. 제한 빌드(`-DMAX9296_360P_MAX_FPS=30U -DMAX9296_HD_MAX_FPS=30U`)로 2차 컴파일까지 돈다 |
| `max9296_prepare_test.py` | **정적 소스 검사** | prepare 생명주기·전원 소유권 계약이 소스에 있는지. 하드웨어 동시성은 실기 테스트가 권위임을 자체 docstring이 명시한다 |
| `max9296_probe_cleanup_test.py` | **정적 소스 검사** | probe 게시의 원자성, V4L2 등록이 마지막인지 (소스 오프셋 비교) |
| `max9296_360p_zoom_exposure_test.py` | **정적 소스 검사** | 360p 모드, 노출 경고, 공통 zoom factor 계약 |
| `max9296_health_export_test.py` | 픽스처 | `health_raw` 익스포터 24 checks |
| `build_360p_candidates_test.sh` | mock repo 빌드 | 후보 빌드 3회 + 기존 `max9296.ko`의 sha256 바이트 보존. **추적 파일 14개의 git 실행 비트(100755)도 게이트한다** |
| `cam_fps_stack_mode_test.sh` | PATH 픽스처 | 계층별 FPS 모드 분류·readback + `pass120` 판정 (예: 115.0 → `loss_pct=4.2 pass120=0`) |
| `cam_360p_resource_test.sh` | PATH 픽스처 | 리소스 수집 capability와 메트릭 계약 (DDR PMU 유무, dmesg 링버퍼 wrap) |
| `run_360p_readout_compare_test.sh` | 환경변수 스텁 | 비교 러너가 운영 상태를 복원하는지 |
| `uyvy_frame_check_test.py` / `rgb565_frame_check_test.py` | 합성 프레임 | 유효성·**green 지배(>0.80)**·stride·절단 검사 |

두 가지 주의:

- `run_360p_readout_compare_test.sh` 실행 중 나오는 `ERROR: production restore failed`는
  **실패 경로를 일부러 태우는 픽스처**다(문자열 출처는
  `tools/run_360p_readout_compare.sh`). 러너 자체는 `PASS`로 끝난다.
- `build_360p_candidates_test.sh`가 실행 비트를 게이트하므로, `tools/*.sh`나
  `tests/fixtures/*`의 `+x`가 빠진 채 커밋되면 이 테스트가 깨진다.

---

## 런타임 인터페이스

### 모듈 파라미터

| 파라미터 | 타입 | 권한 | 초기값 | 설명 |
|---|---|---|---|---|
| `firmware` | `charp` | `0444` | `""` | 로드할 펌웨어 이미지. 비어 있으면 `v4l-ap1302-ar0234.fw`로 폴백한다 |
| `debug` | `int` | `0644` | `0` | 디버그 메시지 |

> `modinfo`는 `debug`를 `default: 1`로 표시하지만 **실제 초기값은 0**이다
> (`max9296.c:53` `static int debug;`, 다른 대입 없음). `MODULE_PARM_DESC` 문자열이
> 낡았다.

### sysfs (`/sys/bus/i2c/devices/<adapter>-0048/`)

`<adapter>`는 Linux i2c 어댑터 번호다 (DT의 `&i2c2` → 어댑터 1).

| 속성 | 권한 | 설명 |
|---|---|---|
| `prepare` | rw | GStreamer가 V4L2 그래프를 열기 전 한 도메인을 초기화하는 블로킹 요청 → [`docs/parallel-prepare-v1.md`](docs/parallel-prepare-v1.md) |
| `health_raw` | r | 하드웨어 블록별 raw evidence (읽을 때만 샘플링) → [`docs/health-raw-v1.md`](docs/health-raw-v1.md) |
| `link_status` | r | **끊긴 채널의 비트마스크** (bit0=ch0 … bit3=ch3). `0`=전부 정상, `-1`=미확인. 값이 클수록 좋은 게 아니다 |
| `enable` | rw | 채널 enable 마스크. 값이 바뀌면 prepare fingerprint를 stale로 표시한다 |
| `rotate` | rw | 값을 저장·조회만 하고 드라이버가 소비하지 않는다. **실제 회전은 이 노브가 아니라 `hflip_chX`/`vflip_chX` 컨트롤이 담당**한다 (아래 참조) |

`prepare` 성공은 SERDES 테이블·AP1302 펌웨어·후속 설정이 끝났다는 뜻일 뿐이다.
프레임이 흐른다거나 GMSL 링크가 정상이라는 뜻이 **아니다**. CSI 출력이나 FSYNC를
켜지도 않는다.

### V4L2 컨트롤

공통 컨트롤(`crop_enable`, `exp_time`, `dz`, `dz_x`, `dz_y`, `hue`,
`power_line_frequency`)과 채널별 ISP 컨트롤(`ae_on_chX`, `gain_chX`,
`exp_time_chX`, `brightness_chX`, `led_flash_chX` …)이 있다.
전체 목록과 레지스터 매핑, 고정점 스케일(256=1.0 / 4096=1.0)은
[`V4L2_CTRL_GUIDE.md` §3](V4L2_CTRL_GUIDE.md)에 있다.

**회전**은 `hflip_chX`/`vflip_chX` 두 컨트롤로 한다. 드라이버가 둘을 합쳐
AP1302 `ROTATION(0x100c)`에 쓴다 — `0`=없음, `1`=H, `2`=V, `3`=180°.
**임의 각도(90°/270°)는 지원하지 않는다.** sysfs `rotate`는 이 경로와 무관하다.

```bash
v4l2-ctl -d /dev/v4l-subdev2 -c hflip_ch0=1,vflip_ch0=1   # ch0 180°
```

### 해상도와 FPS

| 카메라당 출력 | single 폭 | dual 폭 | `max_fps` | 노출 쓰기 안전 상한 |
|---:|---:|---:|---:|---:|
| 1920x1080 | 1920x1080 | 3840x1080 | 30 | 30 |
| 1280x720 | 1280x720 | 2560x720 | 60 | 30 |
| 640x360 | 640x360 | 1280x360 | 120 | 30 |

`cam_width`/`cam_height`가 출력 크기를 정하고, `crop_enable`/`dz`는 그 안에서
디지털 확대·중심 조준만 한다 — 출력 해상도를 바꾸지 않는다.

> 위 `max_fps`는 **드라이버가 허용하는 요청 상한**이다. 그 FPS가 실제로 전달된다는
> 보장도, 상위 스택이 그 조합을 지원한다는 보장도 아니다. 640x360의 120이 요청
> 상한이지 실측은 113~115인 것과 같은 성격이다.

---

## 배포

`update_bin.sh`는 빌드된 `max9296.ko`를 사내 배포 패키지 저장소에 복사하고
바이너리 매니페스트를 맞춘다. 기본 대상은 이 저장소와 나란히 있는 `pim-package-jhw`
이며 `--pim-dir` 또는 `PIM_PACKAGE_DIR`로 바꾼다. 그 저장소가 없으면 쓸 일이 없다.
옵션은 `./update_bin.sh --help`로 확인한다.

---

## 문서 지도

| 문서 | 내용 |
|---|---|
| [`V4L2_CTRL_GUIDE.md`](V4L2_CTRL_GUIDE.md) | 런타임 제어 가이드 (중심 문서) |
| [`CHANGELOG.md`](CHANGELOG.md) | 변경 이력 (Keep a Changelog / SemVer) — 버전 이력의 정본 |
| [`RELEASE_NOTES.md`](RELEASE_NOTES.md) | 릴리즈 요약 (일부 버전만 수록) |
| [`docs/v4l2-controls-guide.md`](docs/v4l2-controls-guide.md) | 컨트롤 요약본 (318줄). 같은 주제를 다루므로 상세는 위 `V4L2_CTRL_GUIDE.md`를 본다 |
| [`docs/reports/`](docs/reports/) | 날짜별 검토 결과 보고 |
| [`docs/health-raw-v1.md`](docs/health-raw-v1.md) | 읽기 전용 health ABI v1 |
| [`docs/parallel-prepare-v1.md`](docs/parallel-prepare-v1.md) | 병렬 prepare ABI v1 (영문) |
| [`docs/prepare-board-gate-v1.md`](docs/prepare-board-gate-v1.md) | prepare 보드 게이트 G1~G4 실측 (전부 통과) |
| [`docs/360p-readout-120fps-validation.md`](docs/360p-readout-120fps-validation.md) | 640x360 readout / crop / 120 FPS 보드 검증 |
| [`docs/fps-limit-analysis.md`](docs/fps-limit-analysis.md) | FHD 60fps가 안 되는 이유 (실측 분석) |
| `docs/imx8mp-evk.dts`, `docs/imx8mp.dtsi` | 참고용 디바이스 트리 |
| [`dw100-spike/REPORT.md`](dw100-spike/REPORT.md) | i.MX8MP Dewarp(dw100) 백포팅 타당성 스파이크 — 결론: 타당 |
| `artifacts/` | 보드 자격 시험의 검토된 증적 (원시 로그·백업은 제외 — 위 §알려진 제약) |
| `.github/workflows/` | CI. `build-test.yml`이 커널 모듈 빌드·`modinfo`·심볼·정적 분석을 돌린다 |

`tools/` 12개는 보드에서 쓰는 측정·복구 스크립트다 — 계층별 FPS 측정
(`cam_fps_stack.sh`, `cam_fps_probe.sh`, `cam_fps_matrix.sh`, `cam_fps_watch.sh`),
파이프라인 초기화(`cam_hard_reset.sh`), prepare 게이트 실측(`cam_prepare_gate.sh`),
리소스 샘플링(`cam_360p_resource.sh`), 360p 후보 빌드(`build_360p_candidates.sh`),
readout 비교(`run_360p_readout_compare.sh`), health 익스포트
(`max9296_health_export.py`), 프레임 검사(`uyvy_frame_check.py`,
`rgb565_frame_check.py`).

> ⚠️ `cam_hard_reset.sh`와 `cam_prepare_gate.sh`는 **파괴적이다.**
> `cam_hard_reset.sh`는 `cam-operate.service`를 정지시키고, 종료 코드 2는
> "복구 불가(모듈 refcnt 음수 — 재부팅 필요)"를 뜻한다. `cam_prepare_gate.sh`는
> 스스로 헤더에 파괴적 시험임을 명시한다. 운영 중인 보드에서 무심코 돌리지 말 것.

---

## 알려진 제약

- **640x360의 120 FPS는 요청 상한이지 전달 보장이 아니다.** 실측은 113~115 FPS이고
  운영 기본 선택은 640x360@30이다. 120 FPS fragment는 활성 채널의
  `LED_FLASH_CONTROL(0x3270)` delay를 0으로 설정해야 한다.
- **노출(`EXP_TIME 0x500c`) 안전 상한은 모든 모드에서 30 FPS다.** 그 위의 모드-유효
  FPS에서는 거부하지 않고 `action=write` 경고를 남긴 뒤 쓴다. 경고 구간은
  `안전 상한 < fps ≤ 모드 상한`이므로 640x360의 31~120 FPS와 1280x720의 31~60 FPS다.
  1920x1080은 모드 상한도 30이라 이 구간이 없다 — 30 이하는 경고 없이 쓰고 31 이상은
  `-EINVAL`이다.
- **`exp_time` 값 자체에는 상한이 없다** (`0 ~ INT_MAX`, 기본 10000). frame period
  초과를 알리는 `over_period`는 위 경고 안에서만 계산되므로, 안전 상한 이하로 도는
  모드에서는 비정상적으로 긴 노출값도 경고 없이 그대로 나간다.
- 모드가 허용하지 않는 FPS, 0 FPS, 잘못된 검증 상한은 I2C 전에 `-EINVAL`로 거부한다.
- **1920x1080은 30 FPS를 넘길 수 없다.** 1080p 모드의 라인타임 26.27 us로는 60 FPS
  트리거 주기 안에 한 프레임을 못 읽어 AP1302가 정수 트리거 분주로 떨어지고, 요청을
  올릴수록 오히려 나빠진다(40 → 19.9, 60 → 19.8 FPS). 여는 데는 센서 라인타임과 ISP
  클럭 트리가 함께 정합된 벤더 펌웨어가 필요하다 — blob의 `HINF_MIPI_FREQ`만 올리면
  동반 PLL·분주비가 맞지 않아 ISP 출력이 0이 된다
  ([`docs/fps-limit-analysis.md`](docs/fps-limit-analysis.md)).
- SoC 정지 이력이 있는 수동 WB(`0x510a`) 쓰기는 구현하지 않았다.
- **보드 증적은 `artifacts/` 아래에 있지만 전부는 아니다.** `.gitignore`가
  `artifacts/board-*/raw/`, `**/backup/`, `**/edgeconf-*.json`을 제외한다 — 원시 로그와
  백업에 배포 경로·자격증명이 섞일 수 있어서다. 검토된 요약과 증적만 커밋한다.
- 버전 이력은 `CHANGELOG.md`가 정본이다. `RELEASE_NOTES.md`는 일부 버전만 담는다.
