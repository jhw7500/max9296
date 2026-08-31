# JHW Notion review payload — pending

승인일: 2026-08-29
report: `pim-driver-cam`
상태: **PENDING — 현재 세션에 JHW Notion MCP가 없어 저장되지 않음**

이 파일은 승인된 `/jhw:review` 후보 5개의 재개용 payload다. Notion 저장 도구가
연결되면 아래 DB와 본문을 그대로 사용하고, 성공 URL을 기록한 뒤 pending 상태를
해제한다.

## 1. Decision Log

제목: `640x360 production 정책 — KEEP + AP1302 scale + 30 FPS`

properties:

- report: `pim-driver-cam`
- status: `확정`
- achievement: `true`
- impact: `KEEP과 sensor mode 1 모두 640x360@120의 실제 AP/CSI/ISI가 약 113 FPS였고 mode 2는 120 FPS color 출력이 끊겨, 양산은 KEEP + AP1302 scale @30을 유지하고 unsafe exposure write를 사전 차단했다.`

content:

MAX9296 production의 카메라당 출력은 `640x360@30`, AP1302 sensor-mode
`KEEP`, `crop_enable=false`, `dz=100`으로 유지한다. KEEP에 120 FPS를 요청한
보드 실측은 AP/CSI/ISI 약 113~115 FPS로 엄격 기준 118.8 FPS를 통과하지 못했다.

HD와 640x360 KEEP 모두 AR0234에서 1920x1080 active window와
X/Y_ODD_INC=1을 유지하므로 현재 경로는 native sensor readout이 아니라 AP1302
resampler 축소다. 안전한 firmware sensor profile이 확인되기 전에는 직접 AR0234
window/timing을 production에서 설정하지 않는다.

현재 bootdata의 `SENSOR0_CONF_0..7`에는 readout mode 0/1/2만 매핑돼 있다.
mode 1은 약 960x600 color subsampling이지만 host 출력과 CPU가 KEEP과 같았고,
mode 2는 monochrome bit가 켜져 640x360@120 color pipeline의 CSI/ISI 출력이
0 FPS였다. 따라서 둘 다 production 후보에서 제외한다.

노출 `0x500c` 쓰기는 안전 상한 30 FPS를 넘으면 I2C 전에 거부하고, SoC 정지
이력이 있는 `0x510A` 수동 WB 쓰기는 사용하지 않는다. 사용자 digital crop은
기본 출력 해상도와 독립적으로 유지한다.

## 2. Projects

제목: `MAX9296 360p·digital crop·노출 안전성 구현 및 qualification 현황`

properties:

- report: `pim-driver-cam`
- status: `진행`

content:

max9296 브랜치 `feature/360p-zoom-exposure-safety`에는 `097265e..809d582`
18개 커밋으로 640x360 preview context, crop enable과 digital zoom tuple,
노출 안전 가드, sensor/AP/CSI/ISI 계측 도구, candidate builder와 보드 검증 문서를
구현했다.

gstApp 브랜치 `feature/max9296-360p-zoom`에는 `b9f4767`, `741b61a`,
`895d1f2`로 edgeconf hardware crop parsing, prepare 전 tuple 적용, single/dual
640x360 prepare target을 추가했다.

보드에는 640x360@30 production 구성을 배포하고 FHD/HD/360p runtime crop,
노출 guard, 최종 복원 상태를 검증했다. GitHub issue #41에서 AP1302 firmware
pointer table과 실제 mode 0/1/2를 확인하고 KEEP/SM01/SM02의 640x360@120
FPS·resource를 비교했다. exact full-FOV HD/640 profile은 현재 bootdata에 없어,
vendor 16:9 color profile 확보를 남은 작업으로 유지한다.

## 3. Knowledge Base

제목: `AR0234 sensor readout과 AP1302 scale·crop·digital zoom 구분`

properties:

- report: `pim-driver-cam`

content:

AR0234에는 임의 출력 WIDTH/HEIGHT scaler가 없다. `0x3002~0x3008` window,
`0x300A/0x300C` timing, `0x3040` READ_MODE, `0x30A2/0x30A6`
subsampling으로 readout을 구성한다. 문서화된 skip/bin 계열은 1/2/4/8/16배다.

AP1302의 `PREVIEW_WIDTH/HEIGHT(0x2000/0x2002)`는 출력 크기를 정하고
resampler가 전체 FOV를 축소할 수 있다. `DZ_TGT_FCT(0x1010)`과
`DZ_CENTER_X/Y(0x118C/0x118E)`는 ROI 일부를 선택해 같은 출력 크기로 다시
확대하는 digital zoom이다. `crop_enable`은 이 zoom tuple의 적용 여부다.

FHD와 HD의 AR0234 window는 모두 1920x1080으로 실측됐으며 HD는 AP1302에서
1280x720으로 축소된다. AP1302 firmware와 CROP_CTRL에 따라 digital zoom이 센서
window까지 바꿀 수 있으므로 `dz>100`에서 window가 작아진 사실만으로 native
sensor mode라고 판정하면 안 된다. 비교 기준은 crop false, dz=100으로 고정하고
window, READ_MODE, odd increment, timing과 FOV를 함께 확인한다.

현재 firmware의 `SENSOR0_CONF_0..7(0x60B0..0x60BE)` 값은 양 AP1302에서
`AECC AF10 AF90 0000 0000 0000 0000 A330`이다. 공식 정의상 앞 6개는 mode
0~5, 뒤 2개는 deselect/select event이므로 실제 readout mapping은 0/1/2뿐이다.
mode 1과 2는 모두 READ_MODE 0x3020, X/Y_ODD_INC=3으로 약 960x600을 만들고,
AR0234 `0x30B0` bit 7만 각각 clear/set한다. 이 bit는
`MONO_CHROME_OPERATION`이다.

## 4. Knowledge Base

제목: `MAX9296 녹색 화면 — RGBP/UYVY format mismatch 진단`

properties:

- report: `pim-driver-cam`

content:

dual 640x360 capture node의 실제 포맷은 `1280x360 RGBP`, bytesperline 2560,
sizeimage 921600이었다. 같은 raw 데이터를 UYVY로 해석하면 관측된 녹색/자홍색
화면이 재현됐고 RGB565 little-endian으로 해석하면 정상적인 어두운 영상이었다.

gstApp RTSP의 두 채널은 640x360으로 정상 decode됐으며 녹색 dominance가 없었다.
따라서 이 현상의 원인은 센서/ISP 색 손상이 아니라 소비자 측 pixel format
mismatch다. 재발 시 V4L2 fourcc, bytesperline, sizeimage, dual 분할 경계와
consumer caps를 먼저 대조한다.

## 5. References

제목: `MAX9296 360p qualification 증적 인덱스와 sensor-readout 후속 이슈`

properties:

- report: `pim-driver-cam`

content:

로컬 정본 인덱스:
`artifacts/board-20260828-qualification/README.md`

전체 판정:
`docs/360p-readout-120fps-validation.md`

설계 계약:
`docs/superpowers/specs/2026-08-28-max9296-360p-readout-120fps-design.md`

V4L2와 edgeconf 사용법:
`docs/v4l2-controls-guide.md`,
`docs/examples/edgeconf-max9296-640x360-fragment.json`

후속 조사:
https://github.com/jhw7500/max9296/issues/41

원시 증적은 final production smoke, KEEP 30/120 FPS, HD/FHD readout 비교,
sensor-mode 0~15 discovery, odd-increment 확인 및 decoded image로 구분해 보존한다.

issue #41 firmware/readout 추가 증적:
`artifacts/board-20260828-qualification/issue41-readout-809d582/`

- `firmware-mode-table-evidence.txt`: firmware provenance, pointer/table raw bytes
- `run2/`: KEEP/SM01/SM02 120 FPS, resource, 0x30B0, automatic restore 결과
