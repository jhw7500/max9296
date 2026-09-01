# AR0234 레지스터 접근 Quickstart

이 문서는 현재 보드에서 **검증된 명령만** 짧게 정리한 운영 절차다.

상세 배경은 `AR0234-register-access.md`를 본다.

기본 인자 순서는 **channel -> register -> value** 다.

- `ch0/ch1 -> i2c2`
- `ch2/ch3 -> i2c1`
- AP1302 주소는 `edgeconf_pim.json`을 우선 사용하고, 없으면 `i2cdetect`로 자동 결정한다.

---

## 1. 핵심 결론

- AR0234 레지스터 접근은 **AP1302 DMA 경로**로 정상 동작한다.
- `0x3000` read 결과는 `0x0A56`이다.
- `0x3070` test pattern write/readback은 `PASS`다.
- `0x3036` clock divider write/readback/restore도 `PASS`다.
- `0x3270` LED flash control read/write도 정상이다.

---

## 2. 기본 확인 명령

### AR0234 chip ID 확인

```bash
./cam_ap1302_dma_verify.sh 0 0x3000
```

기대값:

```text
value=0x0a56
```

### test pattern 확인

```bash
./cam_ap1302_dma_verify.sh 0 0x3070
./cam_ap1302_dma_verify.sh 0 0x3070 0x0001
./cam_ap1302_dma_verify.sh 0 0x3070 0x0000
```

---

## 3. LED Flash 제어

### 현재 상태 읽기

```bash
./cam_ar0234_led_flash_read.sh 0
```

출력 예시:

```text
raw=0x0103 masked=0x0103 enable=1 delay=3 extra=0x0000
```

### 끄기

```bash
./cam_ar0234_led_flash_write.sh 0 0x0000
```

### 켜기

```bash
./cam_ar0234_led_flash_write.sh 0 0x0103
```

의미:

- `bit8`: enable
- `bit7:0`: delay

즉 `0x0103`은 `enable=1, delay=3`이다.

---

## 4. Clock 레지스터 확인

### 현재 값 dump

```bash
./cam_ar0234_dma_clock_dump.sh 0
```

현재 확인된 기본값:

```text
0x31ae = 0x0204
0x302a = 0x0005
0x302c = 0x0001
0x302e = 0x0004
0x3030 = 0x004b
0x3036 = 0x000a
0x3038 = 0x0001
0x30ba = 0x7622
```

### 안전하게 변경하고 복구

```bash
./cam_ar0234_dma_clock_safe.sh backup 0
./cam_ar0234_dma_clock_safe.sh set 0 0x3036 0x0008
./cam_ar0234_dma_clock_dump.sh 0
./cam_ar0234_dma_clock_safe.sh restore 0
```

명령 구조:

- 읽기: `channel`, `register`
- 쓰기: `channel`, `register`, `value`
- clock safe: `backup <channel>`, `set <channel> <reg> <value>`, `restore <channel>`

## 5. 주의사항

### 0x3270은 단순 mirror가 아닐 수 있다

일부 상황에서는 `0x0103`을 써도 `0x0303`처럼 읽힐 수 있다.

이 경우는 버스 오류가 아니라, 상태 비트가 추가된 것으로 보고 아래 마스크 기준으로 판단한다.

```text
mask = 0x01ff
```

### PIXCLK 핀 실측은 별도 문제다

현재 읽힌 값은:

```text
0x31AE = 0x0204
```

공개 AR0234 드라이버 기준으로 이 값은 `4-lane serial/MIPI` 설정과 일치한다.

즉, 현재 레지스터 접근은 정상이어도 외부 `parallel PIXCLK` 핀이 바로 관측된다고 단정할 수는 없다.

---

## 6. 권장 순서

1. `0x3000`으로 chip ID 확인
2. `0x3070`으로 write/readback 확인
3. `0x3270`으로 LED flash 제어 확인
4. `0x3036`으로 clock divider 변경/복구 확인
5. 마지막으로 핀 실측 또는 출력 모드 해석 진행
