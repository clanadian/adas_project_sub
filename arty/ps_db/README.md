# Arty Z7-20 PS — DB backend

## Flow

```text
TCP server
→ 원본 bbox + 96×96×3 RGB UINT8 수신
→ q = round(pixel × 127 / 255)
→ 98×98×3 INT8 zero-padding
→ DDR buffer 설정
→ classifier_top start/done
→ 12×12×64 INT8
→ GAP/FC/argmax
├→ TCP response → Jetson overlay
└→ SafetyJudge/HazardLatch → UART1 → TurtleBot Raspberry Pi
```

## Components

| 컴포넌트 | 역할 | 상태 |
| --- | --- | --- |
| `tcp_roi_server` | persistent TCP server | 구현·테스트 |
| `roi_preprocessor` | UINT8→INT8, 1-pixel padding | 구현·테스트 |
| `adas_classifier_drv` | AXI-Lite와 coherent DMA 버퍼 소유 | 구현·실보드 검증 완료 |
| `classifier_device` | `/dev/adas_classifier` ioctl/mmap wrapper | 구현·실보드 검증 완료 |
| `classifier_contract` | PS↔PL 공용 자료형과 상수(헤더 전용) | 확정 |
| `classifier_buffers` | DDR 버퍼 크기·오프셋 상수와 배치 계산 | 구현·테스트 |
| `classifier_model` | Conv/FC 바이너리 로드·크기 검사 | 구현·테스트 |
| GAP/FC/argmax | PL 출력 후처리 | 구현·실보드 bit-exact 검증 완료 |
| `adas_classifier_confidence_ppm` | logits×logits_scale → softmax → confidence_ppm | 구현·golden 벡터 일치 확인 |
| `ps_safety_bridge` | bbox+분류 결과를 프레임별로 모아 안전 상태 판단 | 구현 (정규화·fallback 매핑 계층은 단위 테스트 없음) |
| `SafetyJudge` / `HazardLatch` (`common/`) | 거리·경로 판단과 정지 래치 | 구현·단위 테스트 (`test_safety_judge`) |
| `SafetyTransmitter` / `UartPort` | 20 ms 주기 3-byte 안전 프레임 송신 | 구현·단위 테스트 (`test_safety_transmitter`) |

**PL 을 구동하는 경로는 커널 드라이버 하나뿐이다.** 사용자 공간은
`/dev/adas_classifier` 만 열고, 레지스터와 DMA 버퍼는 전부 `adas_classifier_drv`
가 소유한다. 예전에는 `/dev/mem` 으로 AXI-Lite 를 직접 두드리는 bring-up 경로
(`pl_mmio`, `classifier_accelerator`)가 함께 있었으나 2026-08-25 에 제거했다.
PL 이 HP0 로 DDR 을 읽으므로 `dma_alloc_coherent()` 로 잡은 버퍼가 필요한데,
그 경로는 레지스터만 다룰 뿐 버퍼를 대신 만들어 주지 못해 대체 수단이 아니었다.

`실보드 검증`은 `ps_db_golden_test`(PL 출력 bit-exact 대조)와 실제 Jetson 카메라
연동 테스트로 확인한 것이다. 자세한 내용과 수치는
[`../../docs/reports/DB_ARTY_BRINGUP_REPORT.md`](../../docs/reports/DB_ARTY_BRINGUP_REPORT.md)에
있다.

## AXI-Lite map

| 영역 | 주소 | 내용 |
| --- | ---: | --- |
| arguments | `0x40000000` | input/weight/output DDR 주소 |
| execution | `0x40010000` | `ap_start/done`, requant, bias |

실제 input, weight, output은 드라이버가 `dma_alloc_coherent()`로 할당한 DDR에
있고 PL의 AXI master가 접근한다. 사용자 프로그램은 DMA 영역을 `mmap()`한다.

## Build and test

```bash
cmake -S arty/ps_db -B arty/ps_db/build
cmake --build arty/ps_db/build -j2
ctest --test-dir arty/ps_db/build --output-on-failure
```

실제 서버는 먼저 `adas_classifier_drv.ko`를 로드하여
`/dev/adas_classifier`가 생성된 상태에서 실행한다. 실행 인자는 다음 명령으로
확인한다.

```bash
./arty/ps_db/build/ps_classifier_server
```

최종 DB 보드에서 TurtleBot 제어까지 켜려면 UART1을 지정한다. 지정하지 않으면
분류 서버만 동작한다.

```bash
ADAS_UART_PORT=/dev/ttyPS1 ADAS_UART_BAUD=115200 \
./arty/ps_db/build/ps_classifier_server "*" 5000 \
  arty/models/roi_classifier_int8_db/export \
  6 1 1342756158 38 1322019071 35 1920779908 38 \
  2.9190799511495295e-05
```

UART0(`/dev/ttyPS0`)은 Linux 콘솔이다. UART1은 EMIO를 거쳐
JA1(`Y18`, TXD)·JA2(`Y19`, RXD)로 나온다. Raspberry Pi와 TX/RX를 교차하고
GND를 공통으로 연결한다.

제어 판단에는 confidence 임계값이 없다. 분류에 성공하면 confidence와
무관하게 argmax class를 쓴다 - confidence로 class를 버리면 같은 대상의
class가 프레임마다 흔들려 `HazardLatch`가 "사라졌다"고 오판하고, 표지판이
person으로 바뀌어 Stop 금지 정책이 우회된다. 화면 정리는 Jetson의
`ADAS_OVERLAY_MIN_CONFIDENCE_PPM`이 따로 담당한다.

분류 결과가 `background`여도 proposal score가 `ADAS_MIN_SCORE` 이상이고
bbox가 주행 경로 안에 있으며 `slow_height`를 넘으면 미확정 장애물로
`SLOW`까지만 낸다. 이 fallback은 `STOP`이나 정지 latch를 만들지 않는다.
통신·가속기 오류처럼 class 자체를 받지 못한 경우에는 기존 person
fail-safe 규칙을 적용한다.

위치·크기 판단도 최종 판단 주체인 Arty PS에서 환경변수로 조정한다.
모두 원본 프레임에 대한 `0..1` 비율이며 잘못된 값은 기본값으로 복귀한다.

| 환경변수 | 기본값 | 의미 |
| --- | ---: | --- |
| `ADAS_SIGN_SLOW_WIDTH` | `0.20` | 표지판 SLOW 최소 bbox 폭 (2026-08-21부터 높이 대신 폭 사용 — 카메라가 표지판을 올려다보는 각도라 높이는 실제 거리보다 작게 잡힘) |
| `ADAS_SLOW_HEIGHT` | `0.25` | 자동차·사람 SLOW 최소 높이 |
| `ADAS_STOP_HEIGHT` | `0.45` | 자동차·사람 STOP 최소 높이 |
| `ADAS_ZONE_Y_MIN` | `0.55` | 자동차·사람 bbox 아랫변의 최소 위치 |
| `ADAS_ZONE_X_MIN` | `0.25` | 주행 영역 왼쪽 경계 |
| `ADAS_ZONE_X_MAX` | `0.75` | 주행 영역 오른쪽 경계 |
| `ADAS_MIN_SCORE` | `0.25` | proposal objectness 최소값 |

실행 로그의 `safety judge:` 한 줄에 실제 적용값이 출력된다. 이 설정들은
Jetson이 아니라 `ps_classifier_server` 환경에 지정해야 한다.

```bash
ADAS_SIGN_SLOW_WIDTH=0.20 ADAS_SLOW_HEIGHT=0.25 \
ADAS_STOP_HEIGHT=0.45 ADAS_ZONE_Y_MIN=0.55 \
ADAS_ZONE_X_MIN=0.25 ADAS_ZONE_X_MAX=0.75 ADAS_MIN_SCORE=0.25 \
ADAS_UART_PORT=/dev/ttyPS1 ADAS_UART_BAUD=115200 \
./arty/ps_db/build/ps_classifier_server "*" 5000 <model-dir> \
  6 1 <rq0-mul> <rq0-shift> <rq1-mul> <rq1-shift> \
  <rq2-mul> <rq2-shift> <logits-scale>
```

배포용 INT8 모델과 golden은 다음 위치에 있다.

```text
arty/models/roi_classifier_int8_db/export/
```

실행 시 model directory에는 위 `export/` 경로를 전달한다. GAP은 144개 값의
합계를 그대로 FC에 넣으므로 현재 `gap-div` 실행 인자는 `1`이다.

마지막 인자 `logits-scale`은 `export/manifest.json`의 `logits_scale` 값을
그대로 전달한다 (예: `2.9190799511495295e-05`). confidence_ppm 계산에만
쓰이고 class_id(argmax) 자체에는 영향이 없다 - 이 값이 없어도 분류 결과는
같지만 confidence_ppm이 항상 0으로 나간다.

```bash
./arty/ps_db/build/ps_classifier_server "*" 5000 arty/models/roi_classifier_int8_db/export \
    6 1 1342756158 38 1322019071 35 1920779908 38 2.9190799511495295e-05
```

요청 한 건의 구간별 소요 시간(preprocess / pl_run / postprocess)은 실행 중
자동으로 수집되고, 연결이 끊길 때 세션 요약이 출력된다. `ADAS_PS_CSV`,
`ADAS_PS_REPORT_EVERY`, `ADAS_TCP_NODELAY` 사용법과 판정 절차는
[`../../docs/reports/FPS_MEASUREMENT_GUIDE.md`](../../docs/reports/FPS_MEASUREMENT_GUIDE.md)에
있다.

전체 데이터 계약은
[`../../docs/contracts/ROI_CLASSIFIER_CONTRACT.md`](../../docs/contracts/ROI_CLASSIFIER_CONTRACT.md),
시스템 개요는 저장소 최상위 `README.md`를 사용한다.

## Board golden test

DB bitstream과 DMA 경로는 실제 모델 export의 입력·최종 출력으로 검증한다.

```bash
mkdir -p /home/petalinux/golden_report
./ps_db_golden_test \
  /home/petalinux/arty_deploy_v2/model \
  /dev/adas_classifier \
  /home/petalinux/golden_report \
  2000
```

성공 시 `9216 bytes bit-exact`를 출력한다. 실패 시 report 디렉터리에
`actual_output.bin`과 `comparison.txt`를 남긴다.
