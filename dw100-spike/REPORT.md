# dw100(DWE) 백포팅 타당성 스파이크 — 결과 보고

- 작성일: 2026-06-05
- 대상: 메인라인 `dw100`(i.MX8MP Dewarp Engine) 드라이버를 NXP BSP **5.10.35**(Yocto hardknott, imx8mpevk)에서 사용 가능하게 만드는 백포팅
- 관련 설계: `docs/superpowers/specs/2026-06-05-barrel-distortion-correction-design.md` (방식 B = DWE HW 경로)
- 결론 한 줄: **타당함(FEASIBLE).** 코어 커널 무수정 + 드라이버 국소 변경(~40줄)만으로 5.10.35용 로더블 모듈(`dw100.ko`, vermagic 일치) 빌드 성공.

---

## 1. 검증 환경 (실측 근거)

| 항목 | 값 |
|---|---|
| 커널 빌드 트리(O=) | `/opt/.../linux-imx/5.10.35+git999-r0/linux-imx-5.10.35+git999` |
| 실제 소스 트리 | `/opt/desktop/build-desktop/workspace/sources/linux-imx` (auto-gen Makefile이 include) |
| 커널 버전 | 5.10.35 (`VERSION=5 PATCHLEVEL=10 SUBLEVEL=35`) |
| vermagic(타깃) | `5.10.35-lts-5.10.y+g2fce14defc04 SMP preempt mod_unload modversions aarch64` |
| 툴체인 | `aarch64-poky-linux-` (SDK `5.10-hardknott`) |
| dw100 소스 출처 | 메인라인 **v6.1** `drivers/media/platform/nxp/dw100/` (도입: 커널 6.0, 2022-07 패치) |

## 2. 출발점 (왜 "그대로는 불가"인가)

- 5.10.35 BSP에 **dw100/dewarp 드라이버 소스 0건** (실제 소스 트리 `find` 확인). `media/platform`에 `nxp/` 디렉토리 자체 없음.
- DT 노드는 존재하나 **비활성**: `dewarp: dwe@32e30000`, `compatible="fsl,imx8mp-dwe"`, `status="disabled"`, `power-domains=<&ispdwp_pd>`, clocks core/axi/ahb(`&media_blk_ctrl 19/20/21`), IRQ 100.
- 즉 노드만 있고 바인딩 드라이버가 없음 → `/dev/videoN` 미생성.

## 3. API 정합성 (5.10.35 ↔ dw100)

dw100이 쓰는 거의 모든 신규 API가 NXP BSP에 **이미 백포팅**돼 있음:

| API | 5.10.35 | dw100 사용 | 비고 |
|---|---|---|---|
| `CONFIG_V4L2_MEM2MEM_DEV` | =y | 코어 | m2m 프레임워크 존재 |
| `VIDEOBUF2_DMA_CONTIG` / `MEDIA_CONTROLLER` | =y | 코어 | OK |
| `pm_runtime_resume_and_get` | PRESENT | 3회 | (mainline 5.11+) BSP 백포트됨 |
| `devm_clk_bulk_get_all` | PRESENT | 1회 | DT 모든 클럭 자동 취득 |
| `v4l2_m2m_buf_copy_metadata` | PRESENT | 1회 | OK |
| `V4L2_CAP_IO_MC` | PRESENT | 0회 | 무관 |
| `vb2_queue_change_type` | **MISSING** | **0회** | 결손이지만 dw100 미사용 → 무관 |

## 4. 유일한 실제 갭 — 빌드가 정확히 4개 심볼에서 멈춤

1차 빌드(원본 그대로)는 **대부분 컴파일 성공 후** 아래 4곳에서만 실패:

```
dw100.c:412  error: 'v4l2_ctrl_type_op_validate' undeclared
dw100.c:413  error: 'v4l2_ctrl_type_op_log' undeclared
dw100.c:414  error: 'v4l2_ctrl_type_op_equal' undeclared
dw100.c:854  error: implicit declaration of 'v4l2_ctrl_modify_dimensions'
```

근본 원인 = **메인라인 5.16의 "v4l2-ctrls 동적 배열 + 노출형 표준 type ops" 기능군**이 5.10.35에 없음:
- 5.10.35는 `struct v4l2_ctrl_type_ops` **타입은 있으나** 표준 구현(`v4l2_ctrl_type_op_*`)을 드라이버에 노출 안 함(내부 `std_*` static, 13곳).
- `v4l2_ctrl_modify_range`는 있으나 **`v4l2_ctrl_modify_dimensions`(동적 배열 차원 변경) 없음**.
- `is_array`(N차원 고정 배열)는 지원. **동적(resize) 배열**·`V4L2_CTRL_FLAG_DYNAMIC_ARRAY`는 미지원.
- v4l2-ctrls.c가 아직 **단일 파일**(5.16의 4분할 전).

**왜 dw100이 이걸 쓰나**: dewarp 정점맵 컨트롤 `V4L2_CID_DW100_DEWARPING_16x16_VERTEX_MAP`이 해상도에 따라 정점 수가 바뀌는 **동적 U32 배열**. `s_fmt` 시 `v4l2_ctrl_modify_dimensions()`로 배열 크기를 재조정함.

## 5. 갭의 국소성 — 코어 무수정으로 빌드 통과 증명

이 갭은 **드라이버의 단일 커스텀 컨트롤에 국한**되며 플랫폼 전역 차단 요소가 아님. 스파이크에서 다음 국소 변경(`build/dw100.c`)만으로 **green 빌드** 달성:

1. 노출형 표준 type ops → 로컬 stub(`dw100_spike_ctrl_validate/_log/_equal`)로 대체 (5.10 시그니처 정합).
2. `v4l2_ctrl_modify_dimensions()` 호출 → 스킵(생성 시 dims 유지).
3. 신규 uapi 헤더 `<uapi/linux/dw100.h>` → 로컬 `dw100.h`로 자급(컨트롤 BASE `V4L2_CID_USER_BASE + 0x1190`, BSP 헤더와 충돌 없음 확인).

**빌드 결과**:
```
BUILD EXIT: 0
dw100.ko  694.7K  ELF 64-bit aarch64 relocatable
alias:    of:N*T*Cnxp,imx8mp-dw100        ← DT compatible 매칭
vermagic: 5.10.35-lts-...aarch64          ← 타깃 BSP 커널 일치 → 로드 가능
```

> ⚠️ 이 `dw100.ko`는 **타당성 증명용 아티팩트**(컴파일+링크+vermagic 일치)이며, 런타임 미검증 + 위 stub의 **기능 제약**(동적 resize 스킵 → 기본 LUT 해상도 외 정점맵 부정확) 때문에 **양산용 아님**.

## 6. 양산 백포팅 경로 (택1)

| 경로 | 내용 | 위험/노력 |
|---|---|---|
| **A. 5.16 v4l2-ctrls 동적배열 프레임워크 백포팅** | 노출 type ops + `modify_dimensions` + dyn-array를 코어에 이식 | 코어 `v4l2-ctrls.c`(전 V4L2 드라이버 공유) 수정 → **blast radius 큼**, 5.16의 4파일 분할까지 얽혀 충돌 해소 난이도 ↑. "정석"이나 카메라/코덱 스택 리그레션 위험 |
| **B. dw100 컨트롤을 5.10 API로 적응(국소)** | 정점맵을 **최대 해상도 고정 크기** 배열로 만들거나 `s_fmt`마다 컨트롤 destroy+recreate, 표준 type ops 자체 구현 | **코어 무수정·blast radius 0**. frozen BSP에 적합. 권장 출발점 |

## 7. DT enable 시 필요한 조정 (런타임 bring-up)

- **compatible 정합**: BSP `fsl,imx8mp-dwe` ↔ 드라이버 `nxp,imx8mp-dw100`. 둘 중 하나로 맞춤(드라이버 of_match에 `fsl,imx8mp-dwe` 추가 권장 — DT 비침습).
- `status = "okay"`로 변경.
- 클럭: BSP 노드 3개(core/axi/ahb). `devm_clk_bulk_get_all`이 names 무관하게 전부 취득 → 정합(메인라인 바인딩은 axi/ahb 2개만 명시하나 동작 무관).
- power-domain `<&ispdwp_pd>` 이미 정의됨 → 바인딩 시 활성. IRQ 100, reg 0x32e30000 0x10000 정합.

## 8. 산출물

- `dw100-spike/src/` — 메인라인 v6.1 원본(dw100.c, dw100_regs.h, Kconfig, Makefile, nxp,dw100.yaml, dw100_uapi.h)
- `dw100-spike/build/` — 5.10.35용 적응 소스 + `dw100.ko`(증명 아티팩트) + 로컬 `dw100.h`, `Makefile`
- 빌드 재현: `build/`에서 SDK env source 후 `make ARCH=arm64 CROSS_COMPILE=aarch64-poky-linux- KERNEL_SRC=<O=dir>`

## 9. 다음 액션 (설계 문서 §9와 정합 — DWE는 GLES 부하 초과 입증 후 투자)

1. 경로 B로 정점맵 컨트롤 적응 → 단일 채널 m2m 런타임 bring-up(`/dev/videoN` + identity map 패스스루).
2. DT compatible/status 패치, 단채널 dewarp 동작 확인.
3. 109° 주변부 오차 + **4채널 동시** FPS/latency/queue depth/thermal 실측.
