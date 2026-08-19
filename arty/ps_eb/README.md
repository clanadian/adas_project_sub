# Arty Z7-20 PS — EB backend

EB PL(엔진 3개, 6-op)용 Cortex-A9 Linux 애플리케이션.

TCP·전처리·후처리는 DB 판과 같은 코드를 쓰고, **가속기 제어와 커널 드라이버는
EB 계약으로 다시 구현했다.**

## Flow

```text
TCP server
→ 96×96×3 RGB UINT8 수신
→ q = round(pixel × 127 / 255)
→ 98×98×3 INT8 zero-padding
→ DMA 버퍼에 적재
→ conv0 → pool0 → conv1 → pool1 → conv2 → pool2   (엔진 3개, 기동 6회)
→ 12×12×64 INT8
→ GAP → FC → argmax → confidence_ppm
→ TCP response
```

## DB 판과 다른 점

같은 저장소에 두 PS 트리가 있는 이유가 여기 있다. **주소맵만 다른 것이 아니다.**

| | DB | EB |
| --- | --- | --- |
| PL IP | `classifier_top` 1개 | `conv_engine` / `conv0_engine` / `maxpool_engine` 3개 |
| 기동 | `ap_start` 1회 | **6회** (op 마다) |
| 중간 활성값 | PL 내부 | **DDR ping-pong 버퍼 2개** |
| bias | AXI-Lite 레지스터 | **DDR 주소** |
| 활성화 | ReLU (clamp 하한 0) | **LeakyReLU 13/128**, requant 이전, 레지스터로 on/off |
| requant | 반올림 없음 | 반올림 항 포함 |
| DMA span | `0x13000` | `0x5b000` (활성 버퍼 두 개가 대부분) |
| 드라이버 ABI | 1 | **2** |
| dtsi compatible | `adas,classifier-1.0` | `adas,classifier-eb-1.0` |

**가중치도 교환할 수 없다.** 산술이 달라 반대쪽 export 를 넣어도 오류 없이
동작하고 분류 결과만 조용히 틀린다 (`../models/README.md`).

ABI 버전과 `compatible` 문자열을 나눈 것이 유일한 자동 방어선이다 — DB
드라이버 위에서 EB 서버를 띄우면 `GET_INFO` 에서 걸린다.

## Components

| 컴포넌트 | 역할 | 상태 |
| --- | --- | --- |
| `tcp_roi_server` | persistent TCP server | 구현·테스트 |
| `roi_preprocessor` | UINT8→INT8, 1-pixel padding | 구현·테스트 |
| `adas_classifier_eb_hw.h` | 3-IP 주소맵·레지스터 오프셋·op 표 (커널과 공유) | 인계본에서 옮김 |
| `adas_classifier_drv` | AXI-Lite 3창, coherent DMA, **6-op 시퀀서** | 구현·문법 검증 |
| `classifier_device` | `/dev/adas_classifier` ioctl/mmap wrapper | 구현·빌드 확인 |
| `pl_mmio` | 초기 bring-up용 `/dev/mem` MMIO | 단위 테스트용으로 유지 |
| `classifier_accelerator` | 사용자 공간 6-op 시퀀서 (드라이버 없이 bring-up) | 구현·테스트 |
| `classifier_buffers` | ping-pong 포함 9개 버퍼 배치 | 구현·테스트 |
| `classifier_model` | Conv/FC 바이너리 로드·크기 검사 | 구현·테스트 |
| GAP/FC/argmax/confidence | PL 출력 후처리 | 구현·테스트 |
| `ps_eb_golden_test` | **단계별** 온보드 golden 대조 | 구현·보드 검증 완료 (6-op 전부 bit-exact) |
| `dummy_roi_service` | PL 없이 고정 결과 반환 | 임시·테스트 |

**보드 실측 완료** — 커널 드라이버 probe, 6-op 전부 golden bit-exact, TCP 서버
동작까지 확인됐다. 실측 로그는
[`../../docs/DB_EB_VERIFICATION_SUMMARY.md`](../../docs/DB_EB_VERIFICATION_SUMMARY.md),
사용 절차는
[`../../docs/ARTY_SD_BOOT_USAGE.md`](../../docs/ARTY_SD_BOOT_USAGE.md) §2 참고.

## AXI-Lite map

| 엔진 | 주소 | 실행하는 op |
| --- | ---: | --- |
| `conv_engine` | `0x40000000` | conv1, conv2 |
| `conv0_engine` | `0x40010000` | conv0 |
| `maxpool_engine` | `0x40020000` | pool0, pool1, pool2 |

⚠️ **세 엔진의 레지스터 오프셋이 서로 다르다** (`img_h` 가 `0x4c` / `0x40` /
`0x28`). 한 엔진의 오프셋을 다른 엔진에 쓰지 말 것. 정본은
`include/driver/adas_classifier_eb_hw.h` 이고, 그 출처는
`../pl_eb/SW/arty_cls_address_map.h` 다.

## DMA 버퍼 배치

```text
0x00000  roi      28,812 B   98×98×3, PS 가 zero border 까지 채운다
0x08000  act_a   147,456 B   ping-pong A
0x2c000  act_b   147,456 B   ping-pong B  ← 6-op 이 끝나면 여기 최종 출력
0x50000  w_conv0     432 B   OIHW, 전치 없음
0x51000  w_conv1   4,608 B   WPACK
0x53000  w_conv2  18,432 B   WPACK
0x58000  b_conv0      64 B   INT32
0x59000  b_conv1     128 B
0x5a000  b_conv2     256 B
         span    0x5b000
```

ping-pong 방향:

```text
conv0 : roi   → act_a      pool0 : act_a → act_b
conv1 : act_b → act_a      pool1 : act_a → act_b
conv2 : act_b → act_a      pool2 : act_a → act_b
```

op 이 6개(짝수)라 **최종 출력은 항상 act_b** 다. 이 규칙이 어긋나면 PS 가
중간 활성값을 결과로 읽는데, 크래시가 아니라 값만 틀린다 —
`test_classifier_accelerator` 가 고정해 둔다.

## Build and test

```bash
cmake -S arty/ps_eb -B arty/ps_eb/build
cmake --build arty/ps_eb/build -j2
ctest --test-dir arty/ps_eb/build --output-on-failure
```

Dummy server:

```bash
./arty/ps_eb/build/ps_dummy_server 0.0.0.0 5000
```

## 보드에서 — 순서대로

아래 2·3번에서 쓰는 `ps_eb_golden_test`·`ps_classifier_server`는 rootfs
레시피에 없어 보드에 미리 없다 — PC에서 크로스컴파일해서 scp로 올려 둬야
한다. 절차는
[`../../docs/ARTY_SD_BOOT_USAGE.md`](../../docs/ARTY_SD_BOOT_USAGE.md) §2.3.

### 1. 드라이버

SD 부팅 이미지는 `arty/classifier_linux_eb/`의 PetaLinux 레시피
(`meta-user/recipes-modules/adas-classifier/`)로 이 드라이버를 이미 빌드해
넣고 부팅 시 자동 로드한다 — 아래는 그 레시피 없이 커널 소스만 두고 수동으로
빌드·적재할 때(단독 bring-up)의 절차다.

```bash
cd arty/ps_eb/kernel && make KDIR=/path/to/target/kernel/build
insmod adas_classifier_drv.ko
dmesg | tail   # "EB classifier: DMA buffer at ..., 6 ops"
```

### 2. 단계별 golden 대조 (실가중치)

```bash
./ps_eb_golden_test /opt/adas/model /opt/adas/golden /dev/adas_classifier 2000
```

op 을 **하나씩** 돌리고 매 단계를 golden 과 바이트로 대조한다. 통째로 돌려
최종만 보면 "틀렸다"만 알고 엔진 3개·op 6개 중 어디서 갈렸는지는 모른다.

```text
  op0 conv0  PASS   147456 B bit-exact   1521 us
  op1 pool0  PASS    36864 B bit-exact    501 us
  ...
PASS: 6-op 전부 bit-exact, PL 합계 7780 us
```

- `model` = `arty/models/roi_classifier_int8_eb/export/` (**`_eb` 여야 한다**)
- `golden` = `arty/pl_eb/golden/` (실가중치 golden)

### 3. 서버

```bash
./ps_classifier_server "*" 5000 /opt/adas/model 6 1 \
    1545298110 37 1 \
    1525725976 36 1 \
    1924470265 39 0 \
    7.863078629513149e-06
```

인자는 conv 마다 `<multiplier> <shift> <leaky>` 3개씩이다. `leaky` 는 DB 에
없던 것으로, EB 엔진이 requant **이전**에 LeakyReLU(13/128)를 적용할지
정한다. manifest 의 activation 과 어긋나면 오류 없이 결과만 틀린다
(conv0·conv1 = 1, conv2 = 0).

GAP 나눗수 144 는 FC scale 에 이미 접혀 있으므로 `gap-div` 는 `1` 이다.
**양쪽에 적용하면 argmax 는 맞고 confidence 만 틀린다.**

### 측정

요청 구간별 소요 시간과 CSV 출력은 DB 판과 같다.
`ADAS_PS_CSV`, `ADAS_PS_REPORT_EVERY`, `ADAS_TCP_NODELAY` 사용법은
[`../../docs/FPS_MEASUREMENT_GUIDE.md`](../../docs/FPS_MEASUREMENT_GUIDE.md).
EB 의 `pl_run_us` 는 6-op 전체 시간이다.

## 남은 일

- 커널 드라이버는 `classifier_linux_eb`의 PetaLinux 레시피에 편입됐지만,
  `ps_classifier_server`·`ps_eb_golden_test` 자체는 아직 rootfs 레시피에
  없다 — 매번 수동 크로스컴파일 + `scp`로 올린다 (DB 판과 같은 과제).

개념과 코드의 대응은 `docs/PS_PIPELINE_STUDY.md` 에 정리되어 있다.
