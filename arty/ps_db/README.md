# Arty Z7-20 PS — DB backend

## Flow

```text
TCP server
→ 96×96×3 RGB UINT8 수신
→ q = round(pixel × 127 / 255)
→ 98×98×3 INT8 zero-padding
→ DDR buffer 설정
→ classifier_top start/done
→ 12×12×64 INT8
→ GAP/FC/argmax
→ TCP response
```

## Components

| 컴포넌트 | 역할 | 상태 |
| --- | --- | --- |
| `tcp_roi_server` | persistent TCP server | 구현·테스트 |
| `roi_preprocessor` | UINT8→INT8, 1-pixel padding | 구현·테스트 |
| `adas_classifier_drv` | AXI-Lite와 coherent DMA 버퍼 소유 | 구현·타깃 검증 대기 |
| `classifier_device` | `/dev/adas_classifier` ioctl/mmap wrapper | 구현·빌드 확인 |
| `pl_mmio` | 초기 bring-up용 `/dev/mem` MMIO | 단위 테스트용으로 유지 |
| `classifier_registers` | AXI-Lite base와 register offset | 확정 |
| `classifier_accelerator` | buffer/parameter/start/done 제어 | 구현·가짜 MMIO 테스트 |
| `dummy_roi_service` | PL 없이 고정 결과 반환 | 임시·테스트 |
| `classifier_model` | Conv/FC 바이너리 로드·크기 검사 | 구현·테스트 |
| GAP/FC/argmax | PL 출력 후처리 | 구현·계약값 대기 |

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

Dummy server:

```bash
./arty/ps_db/build/ps_dummy_server 0.0.0.0 5000
```

실제 서버는 먼저 `adas_classifier_drv.ko`를 로드하여
`/dev/adas_classifier`가 생성된 상태에서 실행한다. 실행 인자는 다음 명령으로
확인한다.

```bash
./arty/ps_db/build/ps_classifier_server
```

배포용 INT8 v2 모델과 golden은 다음 위치에 있다.

```text
arty/models/roi_classifier_int8_v2/export/
```

실행 시 model directory에는 위 `export/` 경로를 전달한다. GAP은 144개 값의
합계를 그대로 FC에 넣으므로 현재 `gap-div` 실행 인자는 `1`이다.

개념과 코드의 대응은 `docs/PS_PIPELINE_STUDY.md`에 정리되어 있다.

## Board golden test

DB bitstream과 DMA 경로는 실제 모델 export의 입력·최종 출력으로 검증한다.

```bash
mkdir -p /opt/adas/golden_report
./ps_db_golden_test \
  /opt/adas/model \
  /dev/adas_classifier \
  /opt/adas/golden_report \
  2000
```

성공 시 `9216 bytes bit-exact`를 출력한다. 실패 시 report 디렉터리에
`actual_output.bin`과 `comparison.txt`를 남긴다.
