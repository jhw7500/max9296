# MAX9296 read-only health ABI v1

## 목적과 범위

이 단계는 복합적인 "카메라 에러" 한 개를 만들지 않고, 하드웨어 블록별 raw
evidence를 안전하게 수집한다. 자동 복구 정책은 포함하지 않는다.

물리 영상 경로는 다음과 같다.

```text
AR0234 Sensor -> AP1302 ISP -> MAX9295 Serializer
              -> long powered GMSL cable -> MAX9296 Deserializer
              -> i.MX8MP CSI2 / ISI / DMA -> GStreamer
```

`health_raw`가 직접 관측하는 범위는 DES, GMSL link, SER management endpoint,
ISP endpoint/HINF이다. AR0234 Sensor는 이번 shallow ABI에서 `UNKNOWN`으로
유지한다. CSI2 이후 블록은 `cam_fps_stack.sh`, gstApp 및 pim-package producer가
담당한다.

## 안전 계약

- 드라이버 안에 health timer, delayed work, 자동 reset을 추가하지 않는다.
- `/sys/bus/i2c/devices/*-0048/health_raw`를 읽을 때만 샘플링한다.
- 샘플은 기존 control mutex를 `mutex_trylock()`으로만 획득한다. STREAMON,
  control, firmware load, teardown과 경쟁하면 대기하지 않고 `busy:true`를
  반환한다.
- 각 I2C register는 한 번만 읽는다. retry와 printk를 하지 않는다.
- register write, power toggle, link reconfiguration, module reload를 하지 않는다.
- exporter는 busy sample을 publish하지 않고 이전 snapshot을 그대로 둔다.
  최종 aggregator가 producer age로 stale 여부를 결정해야 한다.
- continuous exporter는 일시적인 parse/I/O 오류에서도 종료하거나 output을
  덮어쓰지 않고 다음 cadence에 재시도한다. `--once`는 busy `75`, invalid raw
  data `65`로 구분한다.

## probe dependency

영상 data path와 control probe dependency는 같지 않다.

```text
MAX9296 local ID -> RX3 physical link
                         +-> MAX9295 management ID/config probe
                         `-> AP1302 endpoint probe -> later AR0234 deep probe
```

RX3가 up이면 MAX9295와 AP1302를 **병렬 branch**로 모두 시도한다. MAX9295 ID
read 실패가 AP1302 probe를 막아서는 안 된다.

- MAX9295 실패 + AP1302 ACK: `serializer=FAIL`. remote tunnel이 AP1302로
  독립 확인됐으므로 SER management/config failure로 귀속할 수 있다.
- MAX9295 ACK + AP1302 실패: `isp=FAIL`. remote tunnel이 MAX9295로 독립
  확인됐다.
- 둘 다 실패: `serializer/isp=UNKNOWN`, `REMOTE_PATH_UNAVAILABLE`. 케이블,
  remote power, tunnel, SER를 단정하지 않는다.
- AP1302 HINF가 증가하지 않음: `AMBIGUOUS_SENSOR_ISP_STALL`. 검증된 AR0234
  monotonic frame counter가 없으므로 Sensor와 ISP 중 하나로 단정하지 않는다.

MAX9296 local control read가 실패해도 SoC에서 별도로 수집하는 CSI2/ISI/DMA
evidence를 지우면 안 된다. 이 producer는 자기 범위만 판정한다.

## register evidence

| Block | Register | Expected/use |
| --- | --- | --- |
| MAX9296 DES | `R0x000D` | device ID `0x96` |
| MAX9296 GMSL | `RX3 R0x002F` | Link A `0x06`, Link B `0x60` |
| MAX9296 context | `CTRL3 R0x0013` | aggregate context only |
| MAX9295 SER | `R0x000D` | device ID `0x91` |
| AP1302 ISP | `R0x0002[15:8]` | HINF 8-bit frame counter |

`CTRL3 LOCKED`는 aggregate 상태이므로 채널별 cable presence 근거로 사용하지
않는다.

## dual-wide와 channel mask

bench 관측상 dual mode에서 odd channel(ch1/ch3)을 끊으면 RX3 Link A bits
`0x06`이 사라지고 `0x60`이 남는다. 따라서 local even channel은 PHY B,
local odd channel은 PHY A로 표시한다. single mode에서는 설정 채널 identity를
`enable`에서 얻고, 실제 up PHY는 A/B/AB로 별도 표시한다.

exporter는 다음 세 mask를 섞지 않는다.

- `configured_channel_mask`: 설정상 활성 채널
- `physical_present_mask`: RX3에서 물리 link가 관측된 채널
- `stream_domain_active_mask`: streaming 중 AP1302 HINF progression이 확인된 채널

dual-wide는 한 CSI 입력의 공유 frame domain이다. 한쪽 link가 끊기거나 HINF가
멈추면 두 채널 모두 `stream_domain_active_mask`에서 빠진다. 반대편 link의 raw
physical bit와 block evidence는 계속 보존한다.

## 사용법

한 번 확인:

```sh
cat /sys/bus/i2c/devices/1-0048/health_raw | jq .
cat /sys/bus/i2c/devices/2-0048/health_raw | jq .
```

두 인스턴스를 1초 주기로 export:

```sh
python3 tools/max9296_health_export.py \
  --input /sys/bus/i2c/devices/1-0048/health_raw \
  --input /sys/bus/i2c/devices/2-0048/health_raw \
  --output /run/pim-camera/max9296.json \
  --interval-ms 1000
```

output은 temp file을 fsync한 뒤 rename하고 mode `0640`으로 publish한다.
HINF는 8-bit counter이므로 운용 polling은 기본 1초를 유지한다. 긴 sampling
간격에서는 counter wrap 때문에 progression을 놓칠 수 있으며, 그 결과는
복구 트리거가 아니라 `UNKNOWN` evidence로만 사용해야 한다.

## 보드 인수 점검

1. 두 adapter에서 JSON이 `PAGE_SIZE`보다 작고 `jq` parse가 되는지 확인한다.
2. idle/STREAMON/control 변경 중 `health_raw` read latency p50/p99를 기록한다.
3. control이 busy일 때 `busy:true`가 즉시 나오며 exporter가 기존 output을
   덮어쓰지 않는지 확인한다.
4. dual ch0/ch1 및 ch2/ch3 cable을 한쪽씩 제거해 RX3 A/B와
   `physical_present_mask` mapping을 검증한다.
5. dual-wide 한쪽 제거 시 peer physical evidence는 남되 shared
   `stream_domain_active_mask`가 두 채널 모두 0이 되는지 확인한다.
6. MAX9295 ID만 실패시키고 AP1302 ACK가 남을 때만 SER FAIL인지 확인한다.
7. MAX9295/AP1302가 모두 NAK이면 SER FAIL counter가 증가하지 않고
   remote-path ambiguous인지 확인한다.
8. exporter 실행 중 module unbind/rebind를 반복해 UAF, hang, stale sysfs가 없는지
   확인한다.
9. 시험 전체에서 power toggle, reset, reinitialize 및 주기적 kernel log가 한 번도
   발생하지 않는지 journal과 GPIO trace로 확인한다.

보드 검증 전에는 이 evidence를 hard reset의 단독 근거로 사용하지 않는다.
