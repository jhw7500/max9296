# AR0234 레지스터 접근 방법

**작성일**: 2026-03-13  
**대상 시스템**: i.MX8MP + MAX9296 + AP1302 + AR0234  
**검증 방식**: 보드 실측 + AP1302 DMA 경유 raw register read/write

---

## 1. 결론

현재 시스템에서 AR0234 레지스터 접근은 **AP1302 DMA 경로**로 정상 동작한다.

- `0x3000` 읽기 결과가 `0x0A56`로 확인되어 AR0234 chip ID 접근이 검증되었다.
- `0x3070` test pattern 레지스터는 write/readback `PASS`가 확인되었다.
- `0x3036` clock divider 레지스터도 write/readback 및 restore가 정상 동작했다.
- 과거에 시도한 AP1302 `SIPM_0 advanced window` 방식은 현재 펌웨어 조합에서 안정적인 raw sensor access 경로가 아니다.

즉, **IMX8MP에서 I2C 명령으로 AR0234 레지스터를 읽고 쓰는 것은 가능하다.**

---

## 2. 시스템 경로

```text
i.MX8MP -> I2C -> AP1302 -> DMA-based sensor access -> AR0234
```

실제 영상 경로와 별개로, 레지스터 접근은 AP1302 내부 sensor access 경로를 통해 이루어진다.

### 2.1 AP1302 주소

- 채널 0 기준 기본 접근 주소: `0x11`
- AP1302 main 주소: `0x3c`

현재 DMA 기반 접근은 `0x11`에서도 정상 동작함을 확인했다.

### 2.2 SENSOR_SIP 확인값

채널 0, primary port 기준:

- `0x604a = 0xF320`
- `sensor_id = 0x10`
- `addr_16 = 1`
- `data_16 = 1`

즉, 현재 AR0234는 **slave `0x10`, 16-bit register, 16-bit data** 형식으로 접근된다.

---

## 3. 검증된 레지스터

### 3.1 Chip ID

```bash
./cam_ap1302_dma_verify.sh 0 0x3000
```

검증 결과:

- `0x3000 -> 0x0A56`

이는 실제 AR0234 chip ID와 일치한다.

### 3.2 Test Pattern

```bash
./cam_ap1302_dma_verify.sh 0 0x3070
./cam_ap1302_dma_verify.sh 0 0x3070 0x0001
./cam_ap1302_dma_verify.sh 0 0x3070 0x0000
```

검증 결과:

- 기본값 `0x0000`
- `0x0001` write 후 readback `PASS`
- 다시 `0x0000` write 후 readback `PASS`

### 3.3 Clock Divider

```bash
./cam_ap1302_dma_verify.sh 0 0x3036
./cam_ap1302_dma_verify.sh 0 0x3036 0x0008
```

검증 결과:

- `0x3036` 값 변경과 readback이 정상 동작했다.
- 이후 restore로 원래 값 복구도 확인했다.

### 3.4 LED Flash Control

```bash
./cam_ar0234_led_flash_read.sh
./cam_ar0234_led_flash_write.sh 0x0000
./cam_ar0234_led_flash_write.sh 0x0103
```

검증 결과:

- `0x3270` 읽기/쓰기 모두 정상
- `0x0000`으로 disable 확인
- `0x0103`으로 enable + delay 설정 가능

---

## 4. 권장 접근 방식

### 4.1 읽기/쓰기 기본 스크립트

#### 범용 DMA read/write

- `projects/pim-package-org/dist/pim/opt/pim/bin/cam_ap1302_dma_verify.sh`

예시:

```bash
# 읽기만
./cam_ap1302_dma_verify.sh 0 0x3000

# 쓰기 + readback verify
./cam_ap1302_dma_verify.sh 0 0x3070 0x0001
```

#### LED Flash 전용

- `projects/pim-package-org/dist/pim/opt/pim/bin/cam_ar0234_led_flash_read.sh`
- `projects/pim-package-org/dist/pim/opt/pim/bin/cam_ar0234_led_flash_write.sh`

예시:

```bash
./cam_ar0234_led_flash_read.sh
./cam_ar0234_led_flash_write.sh 0x0103
./cam_ar0234_led_flash_write.sh 0x0000
```

### 4.2 Clock 관련 dump/backup/restore

- `cam_ar0234_dma_clock_dump.sh`
- `cam_ar0234_dma_clock_safe.sh`

예시:

```bash
./cam_ar0234_dma_clock_safe.sh backup
./cam_ar0234_dma_clock_safe.sh set 0x3036 0x0008
./cam_ar0234_dma_clock_dump.sh
./cam_ar0234_dma_clock_safe.sh restore
```

---

## 5. 레지스터별 주의사항

### 5.1 단순 mirror readback이 아닌 레지스터

모든 레지스터가 “쓴 값 그대로” 읽히는 것은 아니다.

예시:

- `0x3270`에 `0x0103`을 썼을 때 `0x0303`으로 읽힌 경우가 있었다.

이 경우는 버스 오류가 아니라, 상태 비트나 기능 비트가 함께 반영되는 레지스터로 해석해야 한다.

LED Flash의 경우에는 아래 마스크 기준으로 보는 것이 안전하다.

- `bit8`: enable
- `bit7:0`: delay

즉, 검증 시에는 전체 16비트 일치보다 **의미 있는 비트 필드 일치**를 우선한다.

### 5.2 0x30BA

`0x30BA`는 clock divider 변경 시 함께 변하는 경우가 확인되었다.

- `0x3036` 변경 시 `0x30BA`가 `0x7622 -> 0x7602`로 변함
- `0x302a` 변경 시 `0x30BA`가 `0x7622 -> 0x7621`로 변함

따라서 `0x30BA`는 고정 설정값이 아니라, 내부 상태나 파생 설정의 영향을 받는 값으로 보는 것이 안전하다.

---

## 6. PIXCLK 확인과 관련한 현재 해석

현재 읽힌 값:

- `0x31AE = 0x0204`

공개 AR0234 드라이버는 스트리밍 시작 시 아래와 같이 설정한다.

```c
AR0234_REG_SERIAL_FORMAT = (0x0200 | num_data_lanes)
```

즉 현재 값 `0x0204`는 **4-lane serial/MIPI 출력 쪽과 일치**한다.

따라서 현재 설정에서는 divider만 바꿔도 외부 `parallel PIXCLK` 핀이 바로 관측된다고 단정할 수 없다.

정리하면:

- AR0234 레지스터 접근 자체는 정상
- clock 관련 레지스터 변경도 정상
- 하지만 현재 출력 인터페이스는 MIPI 쪽일 가능성이 높음
- 외부 PIXCLK 핀 관측은 센서 출력 모드와 보드 연결 조건을 함께 봐야 함

---

## 7. 빠른 확인 절차

### 7.1 AR0234 접근 확인

```bash
./cam_ap1302_dma_verify.sh 0 0x3000
./cam_ap1302_dma_verify.sh 0 0x3070 0x0001
./cam_ap1302_dma_verify.sh 0 0x3070 0x0000
```

### 7.2 LED Flash 확인

```bash
./cam_ar0234_led_flash_read.sh
./cam_ar0234_led_flash_write.sh 0x0000
./cam_ar0234_led_flash_write.sh 0x0103
```

### 7.3 Clock Divider 확인

```bash
./cam_ar0234_dma_clock_safe.sh backup
./cam_ar0234_dma_clock_safe.sh set 0x3036 0x0008
./cam_ar0234_dma_clock_dump.sh
./cam_ar0234_dma_clock_safe.sh restore
```

---

## 8. 최종 결론

현재 시스템에서 AR0234 레지스터 접근의 정답 경로는 **AP1302 DMA 기반 raw register access**이다.

검증이 끝난 항목은 다음과 같다.

- AR0234 chip ID read 정상
- AR0234 test pattern register write/readback 정상
- AR0234 clock divider register write/readback/restore 정상
- AR0234 LED flash control read/write 정상

따라서 IMX8MP에서 I2C 명령으로 AR0234 레지스터를 제어하는 것은 **정상적으로 동작한다**.

단, `PIXCLK` 핀 실측은 레지스터 접근 검증과 별개의 문제이며, 현재 출력 인터페이스가 MIPI 모드일 가능성이 높으므로 센서 출력 모드와 보드 라우팅을 함께 고려해야 한다.
