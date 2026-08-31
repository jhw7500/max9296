# pim camera3 640x360@120 target evidence

검증일은 2026-08-31, 보드는 `pim-camera-v016`, 커널은
`5.10.35-lts-5.10.y+g2fce14defc04`다. 최종 설치 상태는
`pim-mp 0.6.3+jhw.camera3`, max9296 2.10
(`srcversion=8EBDAFE29DF1EA7734A71CB`)이다.

## 결론

- 드라이버 2.10은 일반 모듈에서 640x360의 120 FPS 요청을 수락한다.
- 초기 camera2 JSON은 활성 ch0의 AR0234 `LED_FLASH_CONTROL(0x3270)` delay를
  128(`0x0180`)로 써 CSI가 약 46 FPS까지 낮아졌다.
- 드라이버와 gstApp을 고정하고 활성 ch0의 `flash_delay`만 0(`0x0100`)으로 바꾼
  단일 변수 시험에서 CSI/ISI가 약 112.4~112.6 FPS로 회복됐다.
- camera3 DEB에 포함한 120 FPS fragment로 재시험한 값은 ch0/ch1
  sensor `117.5/118.9`, ISP `114.9/114.4`, CSI `113.3/113.1` FPS다. 센서에서
  CSI까지 최대 낙차는 `2.2/3.8%`, transport error는 0이다.
- 요청값 120에 대한 엄격 기준 118.8 FPS에는 미달하므로 실제 120 FPS 전달을
  보장하지 않는다. 현재 확인된 정상 전달 범위는 약 113~115 FPS다.
- 원래 640x360@30 JSON 복원 후 ch0/ch1은
  `29.9/29.9/29.8/29.9`, `30.0/29.9/29.7/29.8`
  (sensor/ISP/CSI/ISI)로 회귀 통과했다.

## 원인 분리

| 고정/변경 조합 | CSI 결과 | 해석 |
|---|---:|---|
| 현재 앱 + 현재 JSON(delay 128) | 약 45.6 FPS | 기준 저하 재현 |
| 과거 driver 2.9 + 현재 앱/JSON | 약 46.0 FPS | 2.10 변경 원인 아님 |
| 현재 driver/app + 과거 JSON(delay 0) | 114.6~115.0 FPS | JSON 영향 확인 |
| 과거 app + 현재 JSON(delay 128) | 약 14.4~14.5 FPS | 앱 교체만으로 회복 안 됨 |
| 과거 app + 과거 JSON(delay 0) | 113.6~114.0 FPS | JSON 영향 교차 확인 |
| 현재 driver/app/config + ch0 delay만 0 | 112.4~112.6 FPS | 활성 ch0 delay 단일 원인 확인 |

정규화한 현재/과거 120 FPS JSON의 유일한 차이는 ch0~ch3
`led_flash.flash_delay`였고, gstApp 소스상 LED가 활성인 ch0만 delay를 포함한
`0x0100 | delay`를 쓴다. disabled ch1은 delay 값과 관계없이 `0x0000`을 쓴다.

## 주요 파일

| 파일 | 내용 | SHA-256 |
|---|---|---|
| `fps-120.txt` | camera2, delay 128 저하 | `da25d3d9ee18945db7467f2027918cb1868ab1577179c98712e4eebb7459a97f` |
| `fps-old-driver-ab-120.txt` | 과거 driver 2.9 A/B | `dda5f15d33c20f9378912af55fdc1c35c6dc6a9044fb6dfa09cea0a796ccd407` |
| `fps-current-app-historical-config.txt` | 현재 앱 + 과거 JSON | `d4350a9abc8f6f2266c56217d82d9801c450f4126b2f8ed6981c5e8e2089e4aa` |
| `fps-historical-app-current-config.txt` | 과거 앱 + 현재 JSON | `175f96cfad135b9442b0ec5e1c5b10d4423a6177ad775c5b6eb8f9fe43359d3f` |
| `fps-current-config-ch0-flash-delay-0.txt` | ch0 delay 단일 변수 시험 | `d224b7804d161f960c5a67da755279f24524b909edbcbbc30a979be084da14cb` |
| `fps-camera3-120.txt` | 최종 DEB/fragment 120 FPS | `0f13f597dba67a97e64089e922a43662c03deb00a549eabba15eccc326c2d59d` |
| `resource-camera3-120.txt` | 30초 CPU/RSS/온도/dmesg | `a31eee8d269e5d7b87fdfdc041ede87b98deeb18aefb032c4874d14f645db098` |
| `exposure-guard-camera3-120.txt` | 120 FPS 수동 AE 거부 | `fb404d0cace65c2b0af1d1c580211555f8dd6d904468dfbda433dee90370a969` |
| `fps-camera3-final-30.txt` | 최종 30 FPS 회귀 | `90c64d1b5586fd8c5a670ad92958c1049b9152338f4768e52f0405e2b77b331f` |
| `final-state-camera3.txt` | 원복 후 package/driver/JSON/service | `fd8637c9ff47ef08d91c3a72237b24c455ea6f49faf3d4c8b540f004a7d546ea` |

`resource-camera3-120.txt`에서 `/dev/video4` format 조회는 실행 중인 gstApp의
exclusive 점유 때문에 `EBUSY`였지만, CPU/RSS/thermal/dmesg 계측은 완료됐다.
새 overflow/CRC/ECC/lost-frame/timeout은 0이고, 측정 도구 자체의 중복 open으로
ISI busy 로그 1건이 남았다.
