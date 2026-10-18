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
- 각 I2C register는 한 번만 읽는다. retry를 하지 않는다.
- printk는 **하나의 예외만** 허용한다 — dual-wide pair 판정이 직전 값에서
  바뀔 때 남기는 change-only 한 줄이다(아래 `pair` 항목). 샘플마다 찍지
  않으므로 정상 상태에서는 무음이다(예외는 아래 `pair` 항목의 "하한은 설정 fps
  기준" — 전달률이 설정의 절반 아래이고 독자가 그 사이 주기로 읽으면 정상 쌍도
  복구 NOTICE와 fault WARNING을 반복할 수 있다). 이 줄이 커널 로그로 나가는 이유는 정지가
  userspace watchdog을 건드려 소비자가 죽어도 증거가 journald에 남아야 하기
  때문이다. 그 외 경로는 여전히 printk를 하지 않는다.
- 그 한 줄은 **control mutex를 놓은 뒤** 찍는다. process context의 printk는
  console lock을 잡고 밀린 버퍼를 통째로 내보낼 수 있는데, health_raw는 0444라
  이 경로의 시점을 비특권 독자가 고른다 — 같은 mutex를 STREAMON이 기다린다.
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

### `pair` — dual-wide 쌍 판정 (schema 1에 가산)

최상위에 `"pair":{"status":...,"sample_gap_ms":...}`가 있다. schema 번호는 1
그대로다 — 기존 필수 키를 바꾸지 않는 가산 필드이고, exporter는 필수 키만
검사하므로 이 필드를 모르는 소비자도 그대로 동작한다.

| status | 뜻 |
| --- | --- |
| `ALIGNED` | 두 채널 HINF가 모두 진행 |
| `DIVERGENT` | **한쪽만 정지** — 합성 와이드 프레임이 성립하지 않는 서명 (이슈 #65). 36 초 지속한 포착과 약 5 초 지속한 포착이 있고, 이 판정 없이 `BOTH_STALLED` 로 직행한 동결도 있다 (아래 실기 측정) |
| `BOTH_STALLED` | 양쪽 정지. **이슈 #65 동결에서도 나온다** — 두 채널이 함께 멈추는 양상 (아래 실기 측정) |
| `NOT_APPLICABLE` | **판정하지 않음. 정상이라는 뜻이 아니다** |

`NOT_APPLICABLE`이 나오는 경우는 넷이다 — single mode, 비streaming, 채널 상태
미확정, 그리고 **간격을 판정할 수 없을 때**. 마지막은 다시 둘로 갈린다.

1. **샘플 간격이 판정 가능 구간 밖**이다. 하한은 2 프레임 주기, 상한은 **255 프레임
   주기에서 1 ms를 뺀 값**이다. HINF가 8비트라 256 프레임이면 카운터가 제자리로
   돌아오는데, tick의 위상을 알 수 없으므로 255.96 주기짜리 간격도 위상에 따라 256
   경계를 지난다 — 그래서 상한이 256이 아니라 255이고, 두 타임스탬프가 모두 ms 절단이라
   1 ms를 더 뺀다. 실제 값: 30 fps에서 67~8,499 ms, 120 fps에서 17~2,124 ms.
2. **baseline을 잡은 뒤 프레임 레이트가 바뀌었다.** classifier는 한 구간을 한
   속도로 환산하므로 섞인 구간은 판정할 수 없다. 이 경우 거부하고 baseline을
   재시드한다.

`sample_gap_ms`는 그 판정에 쓰인 실제 간격이지만 **거부 사유를 다 설명하지는
않는다**: 2번에서는 `sample_gap_ms`가 구간 안에 있는데도 `NOT_APPLICABLE`이 나오고,
두 채널이 모두 유효한 상태일 수 있다. 그 값만으로 "너무 빠른 샘플러"와 "정말 판정
불가"를 구분하는 것은 레이트가 바뀌지 않은 동안에만 성립한다. JSON은 레이트를
내보내지 않으므로, 레이트를 바꾼 직후의 `NOT_APPLICABLE` 한 샘플은 정상이다.

**공유 baseline이 이 절의 전제다.** health_raw는 on-demand이고 드라이버는 모든
독자에게 baseline 하나를 쓴다. **너무 짧은** 간격은 baseline을 갱신하지 않고
고정하므로 그 baseline은 계속 나이를 먹고, 먼저 2 프레임 문턱을 넘긴 독자가 그
구간을 소비하고 다시 심는다. (창 밖으로 **너무 긴** 간격은 반대로 그 자리에서
재시드한다 — 그러지 않으면 이후 모든 샘플이 창 밖에 머문다.) 여기서 따라오는 것들:

- **빠른 독자가 느린 독자의 `pair`를 가린다.** 두 프레임 주기보다 빠르게 도는
  프로세스가 있으면 판정 가능 샘플을 사실상 그쪽이 다 가져가고, 1 Hz exporter를
  포함한 느린 독자는 자기 JSON에서 `NOT_APPLICABLE`만 본다. 판정과 커널 로그는
  계속 살아 있다 — 빠른 독자가 그 판정을 내리고 있다. 막으려면 드라이버 주기
  샘플링이 필요한데 이 ABI가 금지하므로(위 안전 계약), 방어하는 대신
  `sample_gap_ms`로 드러낸다.
- **판정 전환은 초당 한 번이 아니라 두 프레임 주기당 한 번까지** 일어날 수 있다
  (120 fps에서 약 59/s). routine 전환의 1초 게이트는 그 때문에 있다 — 자세한 것은
  아래 "커널 로그 한 줄".
- **하한은 `설정` fps 기준이다.** 실제 전달률이 설정값의 절반 아래로 떨어지면 두
  설정 프레임 주기가 실프레임 한 장을 보장하지 못해 정상 채널이 `STALLED`로 읽힌다.
  드라이버는 전달률을 볼 수 없어 이 경우를 구분하지 못한다 — 반복되는
  `BOTH_STALLED`를 고장으로 읽기 전에 실제 fps를 먼저 확인한다. 이때 **판정이
  독자의 주기에 따라 달라진다**: 30 fps 설정·2 fps 전달에서 1초 주기로 읽으면
  `ALIGNED`(무음)이고, 67 ms~500 ms 사이로 읽으면 `BOTH_STALLED`/`ALIGNED`가
  번갈아 나온다. 복구 NOTICE가 1초 게이트를 지난 직후 첫 fault WARNING은 즉시
  나오므로 두 줄이 붙을 수 있고, 이 쌍이 장치당 평균 최대 초당 한 번 반복된다. 아래 보드
  인수 항목 9의 예외가 이 경우다.

  같은 조건에서 **진짜 한쪽 정지가 `BOTH_STALLED`로 보고될 수 있다.** 정상인 쪽의
  실프레임 주기보다 짧게 읽으면 그 채널도 대부분의 창에서 안 움직인 것으로 읽히기
  때문이다. 호스트에서 판정 로직을 그대로 돌려 잰 값(60초, ch1만 실제로 정지, 설정
  30 fps):

  | 실전달 | 읽기 주기 | DIVERGENT | BOTH_STALLED |
  |---:|---:|---:|---:|
  | 30 fps | 1000 ms | 60 | 0 |
  | 30 fps | 100 ms | 600 | 0 |
  | 2 fps | 1000 ms | 60 | 0 |
  | 2 fps | 200 ms | 120 | 180 |
  | 2 fps | 100 ms | 120 | 480 |

  **서명이 사라지지는 않는다** — `DIVERGENT`도 계속 나오므로 로그에는 두 판정이
  섞여 보인다. 그래도 `BOTH_STALLED`가 다수가 되므로, 한쪽 정지를 양쪽 정지로 오독할
  수 있다. 판정 자체가 fps 를 쓰지 않는다는 점(카운터가 움직였는가만 본다)에 비추면
  이 열화는 순전히 **하한이 설정 fps 를 실제 주기로 가정하는 데서** 온다.

  **위 표는 한쪽 정지가 지속된다고 전제한다. 실기 동결은 그 전제를 만족할 때도 있고
  아닐 때도 있다.** `max9296_pair_health.h` 머리말이 기록한 포착
  (`poll_20260907_120018_slow_t7`)은 ch B 의 HINF 가 **36 초** 고정된 채 ch A 가 계속
  진행한 경우로, 지속된 `DIVERGENT` 다. 반면 듀얼와이드 640x360@120 에서 새로 관측한
  동결 4 건은 `BOTH_STALLED` 4/4, `DIVERGENT` 1/4 였다 — 두 채널 HINF 가 같은 샘플 창
  안에서 멈추면 `BOTH_STALLED` 로 직행하고(3/4), 한쪽이 먼저 멈추면 `DIVERGENT` 를
  거친다(1/4, 약 5 초). HINF 는 채널별 ISP 출력이므로 두 양상은 "한쪽 ISP 만 멈췄나"로
  갈리는 것으로 보인다 (추정).

  귀결은 하나다: **두 판정 모두 이슈 #65 의 서명이므로 함께 본다.** 한쪽만 감시하면
  다른 양상을 놓친다. 관측된 `DIVERGENT` 지속은 약 5 초와 36 초이고 나머지 3 건은 이
  판정 없이 `BOTH_STALLED` 로 직행했으므로, 판정이 얼마나 오래 유지되는지로 양상을
  예측하지 않는다. 직행한 3 건이 **1 초보다 짧은 한쪽 정지를 지났는지 두 채널이 정말
  동시에 멈췄는지는 1 초 해상도로 갈라지지 않는다** — 어느 쪽도 관측되지 않았다.

  `BOTH_STALLED` 는 **스트림 시작 직후 1 샘플짜리 과도 상태로도 나온다** — 같은 측정에서
  세 번 나왔고 셋 다 1~2 읽기 만에 `ALIGNED` 로 복구했다. 그 셋은 선행 판정이
  `NOT_APPLICABLE` 이었고, 동결 넷은 `ALIGNED` 또는 `DIVERGENT` 였으며 복구하지 않았다.
  classifier 가 baseline 재시드 뒤 `BOTH_STALLED` 를 강제하는 것은 아니지만, **단일
  샘플로 경보하면 스트림 재기동에서 오경보할 수 있다** — 소비자는 지속을 본다. 측정
  기록과 한계는 `artifacts/board-20261002-freeze-verdict/README.md` 에 있고, 두 양상의
  비율도 이 판별자도 표본이 작으므로 일반화하지 않는다.

**같은 gate가 `channels[].isp.hinf_progress`에는 없다.** 그 값은 여전히
`health.hinf_count[]`를 모든 독자가 갱신하는 baseline과 비교해 나오므로, 빠른
독자가 도는 동안 **`pair`는 `NOT_APPLICABLE`(판정 거부)인데 두 채널
`hinf_progress`는 `NO`(정지)로 읽히는 문서**가 나올 수 있다. 하필 exporter가
전파하는 쪽이 gate 없는 그 필드다 — `stream_domain_active_mask`가 0이 되는 근거도
거기서 온다. 두 필드를 같이 읽는 소비자는 이 비대칭을 알고 있어야 한다.

### 커널 로그 한 줄

판정이 직전 **출력된** 값에서 바뀔 때만 로그 후보가 된다. 복구와 이미 fault인 상태 사이의
전환은 장치별로 1초에 한 줄까지 나간다. 단, `NOT_APPLICABLE`/`ALIGNED`에서
`DIVERGENT`/`BOTH_STALLED`로 처음 들어가는 WARNING은 직전 정상 로그가 1초 예산을
썼어도 즉시 나간다. 이것을 routine 게이트에 보류하면 바로 뒤의 watchdog STREAMOFF가
보류 상태를 지워, 지속된 정지인데도 journald에 아무 증거가 남지 않기 때문이다. 따라서
복구 한 줄과 다음 fault 한 줄이 1초보다 짧은 간격으로 붙는 것은 계약에 맞다.

게이트에 걸려 참은 줄은 판정 상태를 전진시키지 않으므로 다음 판정 가능 샘플에서
재시도되고, 참은 횟수는 다음에 나가는 줄에 `held=`로 붙는다. `held=`는 **마지막으로
나간 줄 이후 참은 전환의 수**다 — 같은 판정이 유지되는 동안 판정 가능 샘플이 여러 번
들어와도 한 번만 센다. 전원 off나 스트림 시작·정지가 그 카운터를 지운다.

**참은 복구나 fault 종류 변경이 그 안에서 되돌아오면 영영 기록되지 않는다.** 그 경우를
본 독자의 JSON에는 남지만 커널 로그에는 남지 않는다. 최초 fault 진입은 보류하지 않으므로
이 제한에 포함되지 않고, fault 종류 변경이 유실될 때도 앞선 fault WARNING은 이미 남아
있다. 나머지 전환에서는 `health_raw`를 읽는 쪽이 증거의 1차 보관자다.

**exporter는 이 필드를 전파하지 않는다.** camera-health-v1 문서는 명명된 키로
자기 문서를 새로 만들며 `pair`를 읽지 않는다. 현재 이 판정을 보는 경로는
`health_raw` 직접 읽기와 커널 로그 두 가지다.

dual-wide는 한 CSI 입력의 공유 frame domain이다. 한쪽 link가 끊기거나 HINF가
멈추면 두 채널 모두 `stream_domain_active_mask`에서 빠진다. 반대편 link의 raw
physical bit와 block evidence는 계속 보존한다.

### `isp_boot` — AP1302 기동 확인 결과 (schema 1에 가산, #110)

최상위 마지막에 `"isp_boot":{"status":...,"epoch_current":...,"stage":...,"channels":[...]}`가
있다. `pair`와 같은 가산 필드라 schema 번호는 1 그대로다.

**이 필드는 샘플러가 읽은 값이 아니다.** 마지막 콜드 초기화가 prepare 경로에서 기록한
결과를 `sensor->lock` 아래에서 복사해 낼 뿐이고, 이 필드 때문에 ISP를 읽지 않는다.
위 안전 계약은 그대로다.

펌웨어는 브로드캐스트 주소 하나로 두 ISP에 함께 내려가므로, 쓰기가 모두 ACK되어도 한 ISP가
기동하지 않을 수 있다. 그래서 콜드 초기화는 ISP마다 R0x2000(`PREVIEW_WIDTH`)을 세 지점에서
한 번씩 읽는다.

| stage | 위치 | 실패로 보는 값 |
| --- | --- | --- |
| `pre_download` | 펌웨어 다운로드 시작 직전 | 읽기 오류, `0` |
| `post_download` | 다운로드 끝(`0x6002` 쓰기와 100 ms 대기) 뒤 | 읽기 오류, `0`, ROM 값 `640`(0x0280) |
| `post_config` | 프리뷰 설정 뒤 | 읽기 오류, 써 넣은 ISP당 폭과 다른 값 |

`post_download`·`post_config`는 실패로 판정될 읽기만 10 ms 간격으로 최대 10번 다시 읽는다.
정상 ISP는 지점마다 한 번만 읽힌다. 한 단계는 검사 대상 ISP를 모두 판정한 뒤 실패한다.

| status | 뜻 |
| --- | --- |
| `OK` | 세 단계를 모두 통과 |
| `FAILED` | `stage`에서 ISP 하나 이상이 거부됨. 그 prepare/STREAMON은 0이 아닌 errno로 끝났다 |
| `INCOMPLETE` | 다른 오류로 `post_config` 전에 시도가 끝남. `stage`는 마지막으로 통과한 단계, 없으면 `none` |
| `NOT_RUN` | 이 인스턴스에서 콜드 초기화가 아직 돌지 않았다. `stage`는 `none` |

`epoch_current`는 결과가 현재 보드 전원 epoch의 것인지를 말한다. 결과는 콜드 시도가
시작될 때만 지워지므로, 같은 epoch의 `ESTALE` 거부나 재사용 prepare 뒤에도 실제로 돈
시도의 결과가 남는다. 하드 리셋 뒤 다음 콜드 초기화 전에는 이전 결과가
`epoch_current:false`로 보인다 — 현재 실패로 읽지 않는다.

`channels[]` 항목은 `{"channel":N,"checked":bool,"errno":E,"value":V}`다. `channel`
번호는 위 `channels[]`와 같다. `checked:false`는 그 채널 ISP를 읽지 않았다는
뜻이다 — single mode의 비활성 채널이거나, 시도가 `pre_download` 검사에 닿기 전에 끝난 경우다. `value`는 마지막으로 읽은 R0x2000(10진)이고, 읽지
못했으면 `null`이다.

한계:

- `post_config`는 ISP당 폭이 1920(펌웨어 기본값과 같다 — 이슈 #110 인용, 미검증)이나
  640(ROM 값)인 모드에서는 설정 미반영을 가리지 못한다. 기동 실패는 앞 단계가 잡는다.
- `firmware=` 모듈 파라미터로 기본 폭이 640인 펌웨어를 쓰면 `post_download`가 매번 실패한다.
- single mode(주소 0x3c)는 192.168.214.4 의 1920x1080 설정에서 10 부팅 모두 통과했다
  (`artifacts/board-20261018-issue110/`). 다운로드 전·후 단계값 자체는 기록하지 않았고, dual-wide
  (0x11/0x12)는 실기로 돌리지 않았다.

커널 로그: 거부된 ISP마다 `AP1302 boot check failed: channel N stage S addr 0xAA R0x2000
0xVVVV errno E` 한 줄(err), 통과하면 `post_config` 뒤 검사한 ISP마다
`AP1302 boot check passed: ...` 한 줄(notice)이 나간다. 콜드 초기화 로그에 통과 줄이
없으면 검사가 돌지 않은 것이다.

**exporter는 이 필드를 전파하지 않는다.** 현재 이 결과를 보는 경로는 `health_raw` 직접
읽기와 커널 로그 두 가지다.

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
9. 시험 전체에서 power toggle, reset, reinitialize 및 **주기적** kernel log가 한
   번도 발생하지 않는지 journal과 GPIO trace로 확인한다. 위 안전 계약이 허용한
   pair 판정 줄은 예외다. 정상 스트림에서 그 줄이 반복해 나오면 **먼저 실제 fps와
   읽기 주기를 확인한다** — 전달률이 설정의 절반 아래이고 읽기 주기가 2 설정 프레임
   주기와 1 실프레임 주기 사이면 게이트가 정상 동작해도 반복된다(`pair` 항목 참조).
   그 조건이 아닌데 반복되면 change-only 게이트가 깨진 것이므로 실패로 본다.

보드 검증 전에는 이 evidence를 hard reset의 단독 근거로 사용하지 않는다.
