# AP1302 SIPM 레지스터 R/W 분석 보고서

**작성일**: 2026-03-13
**드라이버**: max9296.c (MAX9296 GMSL2 + AP1302 ISP)
**커널**: Linux 5.10.35 (NXP BSP, iMX8MP)

---

## 1. 시스템 구성

```
iMX8MP ──I2C──> MAX9296 ──GMSL2──> MAX9295 ──I2C──> AP1302 ──SIPM──> AR0234
                (deser)              (ser)            (ISP)            (sensor)
```

- MAX9296: GMSL2 디시리얼라이저 (I2C 터널 제공)
- AP1302: ISP, 센서 I2C 중계 (SIPM = Sensor Interface Pass-through Mode)
- AR0234CS: 이미지 센서 (I2C 주소 0x10)

### I2C 주소 매핑

| I2C 어댑터 | AP1302 CH0 | AP1302 CH1 | 논리 채널 |
|------------|-----------|-----------|-----------|
| I2C:2 | 0x11 | 0x12 | ch0, ch1 |
| I2C:1 | 0x11 | 0x12 | ch2, ch3 |

---

## 2. AP1302 SIPM 레지스터 구조

### 2.1 접근 방식: Advanced Register Window

AP1302의 SIPM 레지스터는 물리 주소 0x00290000에 위치하며,
ADVANCED_BASE (0xF038) 레지스터를 통해 간접 접근한다.

```
ADVANCED_BASE (0xF038) = 0x00290000 설정 후:
  0xE000 → 물리 0x00290000 (SIPM_GO)
  0xE004 → 물리 0x00290004 (SIPM_CTL)
  0xE008 → 물리 0x00290008 (SIPM_ADR)
  0xE00C → 물리 0x0029000C (SIPM_DW)
  0xE010 → 물리 0x00290010 (SIPM_DR)
  0xE014 → 물리 0x00290014 (SIPM_DIV)
```

참조: AND9230-D (AP1302 RR) 116페이지, Table 13 ADV_SYSTEM

### 2.2 레지스터 필드

**SIPM_GO (0xE000)**: 트랜잭션 트리거
- bit[0]: SIPM_0 GO (1=시작, 완료 시 자동 클리어)
- bit[1]: SIPM_1 GO (미지원 — 테스트 결과 클리어되지 않음)

**SIPM_CTL (0xE004)**: 트랜잭션 제어
- bit[31]: 방향 (1=읽기, 0=쓰기)
- bits[30:24]: I2C 슬레이브 주소 (AR0234 = 0x10)
- bit[16]: 읽기 시 1 설정 필요
- bits[15:8]: 레지스터 주소 바이트 수 (2)
- bits[7:0]: 데이터 바이트 수 (2)

**SIPM_ADR (0xE008)**: 타겟 센서 레지스터 주소

**SIPM_DW (0xE00C)**: 데이터 워드
- 쓰기 시: 센서에 쓸 데이터
- 읽기 시: 센서에서 읽은 결과 (하위 16비트)

**SIPM_DR (0xE010)**: 데이터 읽기 (사용되지 않음 — 결과는 DW에 저장)

---

## 3. 핵심 발견: 32비트 접근 필수

### 3.1 문제

모든 SIPM 레지스터는 **32비트 폭**이다.
`maxim_ops_i2c_read()`의 `val_byte` 파라미터가 1 또는 2이면
빅엔디안 MSB만 읽어서 데이터가 항상 0으로 보인다.

### 3.2 증거

| 접근 크기 | GO 읽기 결과 | DW 읽기 결과 | 판정 |
|-----------|-------------|-------------|------|
| 1바이트 (val_byte=1) | 0x00 (MSB) | - | GO 항상 0으로 오판 |
| 2바이트 (val_byte=2) | 0x0000 (상위 16bit) | 0x0000 (상위 16bit) | 데이터 없음으로 오판 |
| **4바이트 (val_byte=4)** | **0x00000001** | **0x0000005C** | **정상** |

### 3.3 예시 (실제 로그)

2바이트 접근 (잘못됨):
```
SIPM read: ap=0x11 reg=0x3000 DW=0x0000 DR=0x0000 ADR=0x0000
```

4바이트 접근 (올바름):
```
SIPM read: ap=0x11 reg=0x3000 GO=0x00000000 DW=0x0000005c DR=0x00000000 ADR=0x00000020 poll=7
```

### 3.4 적용

모든 SIPM 관련 I2C 접근을 4바이트로 통일:
```c
// 이전 (잘못됨)
maxim_ops_i2c_write(sensor, ap_addr, 0xe008, reg_addr, 2, 2);  // ADR
maxim_ops_i2c_write(sensor, ap_addr, 0xe000, 1, 2, 1);         // GO
maxim_ops_i2c_read(sensor, ap_addr, 0xe000, 2, 1, &go_val);    // GO 읽기
maxim_ops_i2c_read(sensor, ap_addr, 0xe00c, 2, 2, &dw_val);    // DW 읽기

// 이후 (올바름)
maxim_ops_i2c_write(sensor, ap_addr, 0xe008, reg_addr, 2, 4);  // ADR
maxim_ops_i2c_write(sensor, ap_addr, 0xe000, 1, 2, 4);         // GO
maxim_ops_i2c_read(sensor, ap_addr, 0xe000, 2, 4, &go_val);    // GO 읽기
maxim_ops_i2c_read(sensor, ap_addr, 0xe00c, 2, 4, &dw_val);    // DW 읽기
```

---

## 4. SIPM 쓰기: 정상 동작

### 4.1 시퀀스

```
1. ADVANCED_BASE(0xF038) = 0x00290000   ← SIPM_0 페이지 선택
2. CTL(0xE004) = (slave<<24)|(2<<8)|2   ← 쓰기 모드, 2바이트 주소/데이터
3. ADR(0xE008) = reg_addr               ← 타겟 레지스터
4. DW(0xE00C) = data                    ← 쓸 데이터
5. GO(0xE000) = 1                       ← 트랜잭션 시작
6. msleep(10)                           ← 완료 대기
```

### 4.2 CTL 값

```c
// 쓰기: bit31=0
(AR0234_I2C_ADDR << 24) | (0 << 16) | (2 << 8) | 2  // = 0x10000202

// 읽기: bit31=1, bit16=1
(1u << 31) | (AR0234_I2C_ADDR << 24) | (1 << 16) | (2 << 8) | 2  // = 0x90010202
```

### 4.3 V4L2 컨트롤

```bash
# CH0 쓰기: [31:16]=reg_addr, [15:0]=data
v4l2-ctl -d /dev/v4l-subdevX -c sipm_reg_write_ch0=0x301A10D8
```

---

## 5. SIPM 읽기: AP1302 FW 경쟁 조건

### 5.1 문제

AP1302 FW가 자율적으로 SIPM_0을 사용하여 AR0234 센서를 모니터링한다.
주로 R0x0020 (RESET_REGISTER 또는 상태 레지스터)을 주기적으로 읽는다.

우리가 ADR에 0x3000을 쓰더라도, GO가 트리거되기 전에
FW가 ADR을 0x0020으로 덮어쓴다.

### 5.2 증거 (32비트 접근 후 로그)

```
ap=0x11 reg=0x3000 DW=0x0000005c ADR=0x00000020 poll=7   ← ADR이 0x0020
ap=0x12 reg=0x3000 DW=0x00007e20 ADR=0x00000020 poll=1   ← FW가 읽은 R0x0020 값
ap=0x11 reg=0x3270 GO=0x00000001 ADR=0x00000070 poll=40   ← timeout, ADR=0x0070
```

- ADR readback: 항상 0x0020 또는 0x0070 (FW 타겟)
- DW 값: FW가 읽은 AR0234 R0x0020/R0x0070의 값
- 우리가 요청한 레지스터의 값이 아님

### 5.3 결론

SIPM_0 읽기는 AP1302 FW의 자율적 SIPM 사용과 경쟁하여
원하는 레지스터를 안정적으로 읽을 수 없다.

---

## 6. 시도한 접근법과 결과

| # | 접근법 | 결과 | 실패 원인 |
|---|--------|------|-----------|
| 1 | SIPM_0 직접, 2바이트 접근 | 부분 성공 (값 불안정) | 2바이트 읽기로 데이터 손실 + ADR 경쟁 |
| 2 | 2-phase GO (latch + clear) | 부분 성공 | fast path에서 stale DW 반환 |
| 3 | No-latch drain 루프 | 개선 미미 | 근본적 ADR 오염 미해결 |
| 4 | ADR readback sync | 효과 없음 | ADR 항상 0x0000 반환 (2바이트 접근 문제) |
| 5 | ADVANCED_BASE 반복 설정 | 효과 없음 / 악화 | SIPM 트랜잭션 중단 유발 |
| 6 | SIPM_1 (0xE018+, GO bit1) | 전부 타임아웃 | SIPM_1 미지원 또는 GO 인코딩 오류 |
| 7 | DMA 기반 (0x60A0-0x60AC) | 전부 타임아웃 | DMA가 GMSL2 터널 통과 불가 |
| 8 | **SIPM_0 + 32비트 접근** | **데이터 반환** | ADR 경쟁은 여전하지만 GO/DW 정상 읽기 |

---

## 7. 현재 구현 상태

### 7.1 함수 목록

| 함수 | 용도 | 상태 |
|------|------|------|
| `max9296_sipm_write_reg()` | 범용 레지스터 쓰기 | 정상 (32비트) |
| `max9296_sipm_write_led_flash()` | LED flash 전용 쓰기 | 정상 (32비트) |
| `max9296_sipm_read_reg()` | 범용 레지스터 읽기 | 동작하나 FW 경쟁으로 불안정 |
| `max9296_sipm_read_led_flash()` | LED flash 읽기 래퍼 | read_reg 호출 |

### 7.2 V4L2 컨트롤

| CID | 이름 | 타입 | 플래그 |
|-----|------|------|--------|
| `V4L2_CID_SIPM_REG_WRITE_CH0` (0x101C) | SIPM Reg Write CH{0,2} | INTEGER | - |
| `V4L2_CID_SIPM_REG_WRITE_CH1` (0x101D) | SIPM Reg Write CH{1,3} | INTEGER | - |
| `V4L2_CID_SIPM_REG_READ_CH0` (0x101E) | SIPM Reg Read CH{0,2} | INTEGER | VOLATILE, EXECUTE_ON_WRITE |
| `V4L2_CID_SIPM_REG_READ_CH1` (0x101F) | SIPM Reg Read CH{1,3} | INTEGER | VOLATILE, EXECUTE_ON_WRITE |

인코딩: `[31:16] = 레지스터 주소, [15:0] = 데이터`

---

## 8. 다음 단계: AR0234 PIXCLK GPIO 테스트

### 8.1 목적

SIPM 쓰기가 AR0234 핀까지 물리적으로 도달하는지 오실로스코프로 검증.

### 8.2 AR0234 R0x301A (RESET_REGISTER) 비트

```
bit[2]  STREAM        — 스트리밍 시작 (PIXCLK 클럭 출력)
bit[6]  DRIVE_PINS    — 출력 핀 드라이버 활성화
bit[7]  PARALLEL_EN   — 패러럴 인터페이스 (PIXCLK) 활성화
bit[11] FORCED_PLL_ON — PLL 강제 활성화
bit[12] SMIA_SER_DIS  — MIPI 시리얼라이저 비활성화
```

### 8.3 테스트 시퀀스

```bash
DEV=/dev/v4l-subdevX   # 디바이스 번호 확인 필요

# 1. Hi-Z → LOW (PIXCLK 핀 구동 시작, 클럭 없음)
v4l2-ctl -d $DEV -c sipm_reg_write_ch0=0x301A10D8
# R0x301A = 0x10D8: SMIA_SER_DIS + DRIVE_PINS + PARALLEL_EN + STREAM=0

# 2. LOW → 클럭 출력 (STREAM 시작)
v4l2-ctl -d $DEV -c sipm_reg_write_ch0=0x301A10DC
# R0x301A = 0x10DC: + STREAM=1

# 3. 원복 (Hi-Z)
v4l2-ctl -d $DEV -c sipm_reg_write_ch0=0x301A1058
# R0x301A = 0x1058: PARALLEL_EN=0 → PIXCLK Hi-Z
```

### 8.4 추후 과제

- AP1302 FW 일시 중지 방법 조사 (SIPM 읽기 안정화)
- AP1302 캐시 레지스터를 통한 간접 센서 데이터 읽기
- SIPM 읽기 안정화 후 진단 로그 정리
