# 배럴/기하 왜곡 보정 — 조사 및 의사결정 종합

- 작성일: 2026-06-05
- 상태: 의사결정 종합 (구현 전, 렌즈 우선 검토 단계)
- 범위: 카메라 파이프라인 전반(렌즈 → 센서 → ISP → SoC → 앱). 구현 spec(특히 D-GPU)은 추후 `gstApp` 프로젝트에 별도 작성 예정
- 관련 프로젝트
  - 하드웨어/커널: 본 repo (`max9296`) — MAX9296 GMSL2 디시리얼라이저 드라이버
  - 애플리케이션: 형제 프로젝트 `gstApp` — C++ GStreamer 녹화/RTSP 앱

---

## 1. 문제 정의

광각 렌즈(현재 109° FOV)에서 발생하는 **배럴(기하) 왜곡**을 보정한다. ISP(AP1302) 내부, GStreamer/SoC, 후처리 등 가능한 경로를 비교하고 우선순위를 정한다.

## 2. 확인된 시스템 구성 (실측 근거)

- **플랫폼**: NXP i.MX8M Plus EVK, Linux 5.10.35 (NXP BSP, Yocto hardknott)
- **GPU**: Vivante/VeriSilicon **GC7000UL**(3D, OpenGL ES 3.1 / OpenCL 1.2 / Vulkan) + **GC520L**(2D, G2D API = `imxvideoconvert_g2d`). NPU 별도.
- **전용 dewarp HW**: Vivante **DWE**. DT 노드 `dewarp: dwe@32e30000`, `compatible = "fsl,imx8mp-dwe"`, **`status = "disabled"`**. 같은 전원도메인 `ispdwp_pd`, 클럭(core/axi/ahb) 정의됨.
- **카메라 토폴로지**: 각 카메라 모듈 = [AR0234CS 센서 + AP1302 ISP + MAX9295 GMSL2 직렬화]. **MAX9296 디시리얼라이저가 카메라 2개를 한 프레임에 side-by-side(2W×H)로 합쳐 1개 MIPI CSI**로 전송. CSI 2개 → **최대 4채널**.
- **앱 파이프라인**(`gstApp` `videoBin.cpp` 기준):
  - crop_en: `v4l2src → watchdog → imxvideoconvert_g2d → capsfilter(2W×H) → teeCrop`
  - `addCrop(dir)`: `teeCrop → videocrop(좌/우 분리) → [timeoverlay] → imxvideoconvert_g2d → tee → record/rtsp/capture 소비자`
  - 이미 g2d(GPU 2D) + 인코딩 + RTSP + 녹화로 부하 존재
- **렌즈**: 현재 109° 광각. **화각은 고정 요구사항 아님**(양산 렌즈 스펙일 뿐 변경 가능). 초점거리 "19mm" 표기는 1/2.6" 센서 화각과 불일치 → 소싱 기준은 화각·이미지서클·마운트로.
- **용도**: 보기 + 가벼운 분석 **혼합**.

## 3. 레지스터 조사 결과 (직접 확인)

| 문서 | dewarp/GDC/LDC/geometric 레지스터 | 비고 |
|---|---|---|
| **AP1302 RR** `AND9230-D` | **0건** | LSC/GAMMA/DPC는 존재 → 검색 방법 유효성 확인됨. dewarp은 실제 부재 |
| **AR0234 RR** `AND9812-D` | **0건** | 센서는 윈도잉(X/Y_ADDR)·노출·게인·companding·mirror만. 구조적으로 dewarp 불가 |

- AP1302 RR 서문: "펌웨어 업데이트로 새 레지스터가 추가될 수 있고, 최신 정의는 DevWare에 있다" → 특수 펌웨어 빌드가 dewarp을 가질 *이론적* 여지는 남으나 baseline엔 없음.
- 센서가 dewarp을 못 하는 이유: 센서는 포토다이오드 배열 + 행단위 리드아웃이라 **공간 리샘플링(프레임 버퍼+보간) 수단이 없음**. → **왜곡 보정의 최초 가능 지점은 ISP(AP1302)**.
- 참고: AR0234는 글로벌 셔터 → 롤링셔터 모션 왜곡은 없음(시간축 현상, 광학 배럴 왜곡과 무관).

## 4. 후보 방식

| ID | 방식 | 연산 주체 | 종류 | CPU/GPU 부하 |
|---|---|---|---|---|
| 렌즈 | 저왜곡 ~85–90° 정사영 렌즈 교체 | 광학 | — | 0 |
| A | AP1302 ISP 내부 dewarp | 카메라 ISP 칩 | HW(칩 내장) | 0 |
| B | i.MX8MP DWE HW(ISI→DWE) | SoC 전용블록 | HW(전용 실리콘) | 0 |
| D-GPU | OpenCV 맵 + GLES 셰이더 remap | GC7000 GPU | HW 가속(프로그래머블) | GPU 사용 |
| D-CPU | OpenCV `remap`(NEON) | A53 CPU | 순수 SW | CPU 사용 |

> ISI는 보정기가 아니라 **캡처 엔진**(CSC/스케일/크롭만, dewarp 불가). "ISI→DWE"는 ISI가 DRAM에 받고 DWE(전용 HW)가 dewarp. B는 GStreamer element로 제어하므로 통합 방식만 SW와 닮았을 뿐 **연산은 HW**.

## 5. 우선순위 — 두 축 구분 (중요)

"아키텍처 우수성"과 "실행 우선순위"는 다르다.

| 축 | 순위 |
|---|---|
| **① 아키텍처 우수성** (되기만 하면 최적 위치) | 렌즈 ≈ **A(AP1302)** > B(DWE) > D-GPU > D-CPU |
| **② 실행 우선순위** (지금 착수 가능 × 성공 확률 × 외부 의존성 없음) | 렌즈 > **D-GPU** > B > **A(AP1302)** > D-CPU |

**채택 = ② 실행 우선순위.** 이유:

| 순위 | 방식 | 실행 관점 이유 |
|---|---|---|
| 1 | 렌즈 | 패킹·CSI·채널수·부하 전부 무관, 런타임 0. 화각 고정 아님 → 가장 적은 노력의 정답 |
| 2 | D-GPU(GLES) | 드라이버 작업 없이 **오늘 착수 가능**, 전부 자체 통제. 합본도 `x<W→좌 LUT / x≥W→우 LUT`로 seam 무문제 |
| 3 | B(DWE) | 장기적으로 가장 깔끔(HW, 0 부하). 단 5.10 BSP 드라이버 부재 → 백포팅 리스크. **GLES가 부하 초과 입증 후** 투자 |
| 4 | A(AP1302) | **아키텍처는 2위지만 막혀 있음**: RR에 레지스터 0건 + onsemi NDA 펌웨어(외부 의존·일정 리스크). **onsemi가 dewarp 펌웨어 확인 시 즉시 2위로 승격** |
| 5 | D-CPU | golden 생성/장애 fallback 전용. 4채널+녹화+RTSP+인코딩 경합, thermal·tail latency 위험 |

**핵심**: A는 버린 게 아니라 *게이트 대기*. onsemi 문의를 병행(비용 0)하고, "yes"면 렌즈 다음으로 재정렬한다.

## 6. 방식별 방법 & 함정

### 렌즈
- "보정"이 아니라 "회피". 후보 렌즈로 동일 타깃 촬영 → 중심/주변 MTF, 비네팅, usable FOV, 분석 ROI 픽셀밀도, 야간 노출 여유 비교.
- 스펙의 "왜곡률 낮음"만 보지 말고 **AP1302 ISP 후 실제 영상의 직선 잔차·주변부 해상도** 확인.
- 트레이드오프: 정사영 광각은 가장자리 **원근 스트레칭** + 비네팅 증가, 비용·크기 상승.
- 소싱 기준: 이미지서클 1/2.6", 마운트(M12/S-mount 추정), 2.3MP MTF, IR-cut.

### D-GPU (GLES) — 현 BSP 최우선 SW 경로
- 삽입 위치 후보 ①: **합본 2W×H 수신 후 crop 전 1패스**(좌/우 동시, 셰이더에서 `x<W`/`x≥W` 분기).
- 삽입 위치 후보 ②: `addCrop()` 안, **videocrop 직후 / timeoverlay 직전**(채널별 맵). ⚠️ overlay가 같이 휘지 않도록 **dewarp을 overlay 앞**에 둘 것.
- per-pixel LUT 셰이더: OpenCV `map1/map2`를 RG32F 텍스처(uMap)로 → `texture(src, texture(uMap, vUV).rg)`.
- ⚠️ **NV12 직접 처리 함정**: Y/UV 평면 각각 remap + UV 2×2 서브샘플 좌표 정합. **PoC는 RGBA로 정확성 검증 → 양산은 NV12 zero-copy**.
- ⚠️ `gldownload`로 CPU 왕복 금지 → **DMABUF/EGLImage 기반 custom `GstBaseTransform`** 권장.
- 부하: GC7000 ↔ 기존 g2d/인코딩/RTSP 경합 **반드시 측정**(FPS/latency/thermal).

### B (DWE) — 조건부 HW 경로
- 메인라인 `dw100` 드라이버(compatible `nxp,imx8mp-dwe`) 백포팅 + DT enable. (BSP 노드는 `fsl,imx8mp-dwe` → compatible 정합 필요.)
- DT에서 clock/reset/power-domain(`ispdwp_pd`)/IRQ/register enable → V4L2 m2m 단일 채널부터.
- 구조: **crop 좌/우 → DWE → downstream**(합본 1회 적용은 16×16 메시가 중앙 seam에서 좌/우 모델을 섞을 위험). DWE m2m엔 **실제 크롭된 버퍼** 필요.
- 109° 주변부 오차 + **4채널 동시** FPS/latency/queue depth/thermal 실측.

### A (AP1302) — onsemi 게이트
- onsemi에 **고정 질문**(아래 §10) 송부. 레지스터 문서에 없으면 내부 펌웨어 feature → 양산 일정에 보수적 반영.

### D-CPU
- `initUndistortRectifyMap`+`remap`으로 golden 출력 생성용. 운영 경로면 해상도/FPS 제한 명시. Vivante OpenCL(UMat)은 불안정 → 비권장.

## 7. 결정적 통찰 2가지 (Codex 보강)

1. **진짜 리스크는 "보정 위치"가 아니라 "캘리브레이션 기준 영상"**: AP1302가 scaling/crop/digital zoom/WDR/stabilization/LSC를 바꾸면 보정 맵이 어긋남. **해상도·crop·flip·binning·ISP 출력 포맷을 양산 모드로 고정한 상태**에서 캘리브레이션할 것.
2. **"분석 vs 보기" 분리가 더 나은 아키텍처일 수 있음**: rectify된 영상을 분석에 넣으면 interpolation blur + 주변부 stretch로 feature 품질 저하 가능. 대안 = **디스플레이/RTSP/녹화만 보정, 분석은 원본 distorted + intrinsic(K,D) 모델**. 보정 비용도 필요한 소비자에만 부과. (렌즈 교체로 가면 이 고민 대부분 소멸)

## 8. 캘리브레이션 절차 (D·B·A 공통 자산)

1. AP1302 설정 고정(해상도/crop/flip/FPS/scaling/WDR/AE = 양산 모드)
2. 합본 프레임에서 좌/우 crop 후 **카메라별** 캘리브레이션. Charuco 또는 고품질 checkerboard.
3. 109°면 **Brown-Conrady와 fisheye 모델 둘 다** 돌려 재투영오차·직선잔차 낮은 쪽 채택.
4. 중심/네 모서리/다양한 거리·기울기로 **30–50장+**.
5. 카메라별 `K, D, image size, ROI` 저장(좌우 동일 렌즈라도 계수 공유 X).
6. **최종 출력 해상도 기준 LUT 사전 생성**(런타임 매프레임 계산 금지). D=map1/map2 텍스처, B=16×16 vertex blob — **동일 K,D에서 생성 → D↔B 전환 시 자산 재사용**.
7. 검증 = 재투영오차 + 직선잔차 + seam 인접부 + 주변부 blur/stretch + FPS/latency/thermal.

## 9. 최종 권장 & 실행 순서

- **방향**: 렌즈 우선(화각 고정 아님 + 혼합 용도 → 1–2% 잔류 허용 가능성 큼) → 잔류 시 **D-GPU 합본 LUT** → 부하 초과 시에만 **B(DWE)**. A는 onsemi 확인 전까지 비주류, D-CPU는 fallback.
- **실행 순서**
  1. 즉시: 저왜곡 85–90° 렌즈 후보 실측 착수
  2. 병행: GLES LUT PoC(현 렌즈로 품질 + GPU 부하 측정)
  3. 병행: onsemi 문의(A 게이트 확정) — yes면 우선순위 재정렬
  4. 조건부: GLES가 부하 한계 초과 입증 시 DWE 백포팅 스파이크

### Action checklist
- [ ] 저왜곡 ~85–90° 렌즈 후보 2–3종 소싱(1/2.6" 이미지서클, M12/S-mount, 2.3MP MTF) → 실측 비교
- [ ] AP1302 양산 설정 freeze 정의(해상도/crop/flip/포맷)
- [ ] OpenCV 캘리브레이션 환경 구축(Charuco, 카메라별 K/D)
- [ ] GLES dewarp PoC(RGBA, 좌/우 분기 LUT) + GPU 부하/FPS/thermal 측정
- [ ] onsemi 문의서 송부(§10)
- [ ] (조건부) 메인라인 dw100 백포팅 타당성 스파이크 — DT `fsl,imx8mp-dwe` enable + m2m 단채널

## 10. onsemi 문의 항목 (A 게이트 확정용)

현재 센서+펌웨어 빌드에서:
1. LDC/GDC(기하 dewarp) 지원 여부
2. 보정 모델(방사형 다항식 / grid)
3. LUT/grid 해상도
4. 계수 업데이트 방식(레지스터/펌웨어 blob)
5. per-module 캘리브레이션 저장 위치
6. latency 변화
7. 다른 ISP 기능(LSC/WDR/scaling)과의 상호작용

## 11. 미해결/검증 필요
- 현 NXP 5.10 BSP에 GStreamer GL 요소(`glupload/glshader/gldownload`) 동작 확인 — 사용자: 존재 확인됨(타깃 실동작 측정은 PoC에서)
- RG32F 부동소수 텍스처 지원 여부(미지원 시 16-bit 양자화 맵 폴백)
- DWE 4채널 throughput 한계(특히 60fps×4)

## 부록 A. 멀티모델 교차검토(ccg) 기록
- **Codex(gpt-5.5)**: 본 결론과 거의 완전 일치(우선순위 렌즈>GLES>DWE>AP1302>CPU). §7 통찰 2건 보강.
- **Gemini**: HTTP 429(쿼터 소진)로 응답 실패 — 의사결정/UX 교차검증 미수령(공백은 작음, 필요 시 재시도).
- 아티팩트: `.omc/artifacts/ask/codex-*-2026-06-05T03-08-59*.md`

## 부록 B. 주요 출처
- onsemi AP1302 제품/RR(AND9230-D), AR0234 RR(AND9812-D)
- i.MX8MP DW100/DWE: LWN Articles/889434, Linux `dw100` 문서(메인라인 5.19+)
- OpenCV fisheye 카메라 모델 문서
