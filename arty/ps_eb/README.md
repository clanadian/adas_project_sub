# Arty Z7-20 PS — EB backend

현재 DB PS 코드에서 분기한 작업 사본이다. TCP·전처리·후처리는 재사용하고,
가속기 제어와 커널 드라이버는 EB의 3-IP 주소맵 및 6-op 실행 순서로 교체한다.

## Flow

```text
TCP server
→ 96×96×3 RGB UINT8 수신
→ q = round(pixel × 127 / 255)
→ 98×98×3 INT8 zero-padding
→ DDR buffer 설정
→ conv0 → pool0 → conv1 → pool1 → conv2 → pool2
→ 12×12×64 INT8
→ GAP/FC/argmax
→ TCP response
```

## Components

| 컴포넌트 | 역할 | 상태 |
| --- | --- | --- |
| `tcp_roi_server` | persistent TCP server | 구현·테스트 |
| `roi_preprocessor` | UINT8→INT8, 1-pixel padding | 구현·테스트 |
| `adas_classifier_drv` | AXI-Lite와 coherent DMA 버퍼 소유 | EB 계약으로 변경 필요 |
| `classifier_device` | `/dev/adas_classifier` ioctl/mmap wrapper | 구현·빌드 확인 |
| `pl_mmio` | 초기 bring-up용 `/dev/mem` MMIO | 단위 테스트용으로 유지 |
| `classifier_registers` | AXI-Lite base와 register offset | EB 주소맵으로 변경 필요 |
| `classifier_accelerator` | 3개 IP의 6-op 실행 제어 | EB 시퀀서로 변경 필요 |
| `dummy_roi_service` | PL 없이 고정 결과 반환 | 임시·테스트 |
| `classifier_model` | Conv/FC 바이너리 로드·크기 검사 | 구현·테스트 |
| GAP/FC/argmax | PL 출력 후처리 | 구현·계약값 대기 |

## AXI-Lite map

| 영역 | 주소 | 내용 |
| --- | ---: | --- |
| conv engine | `0x40000000` | conv1·conv2 실행 |
| conv0 engine | `0x40010000` | conv0 실행 |
| maxpool engine | `0x40020000` | pool0·pool1·pool2 실행 |

실제 input, weight, output은 드라이버가 `dma_alloc_coherent()`로 할당한 DDR에
있고 PL의 AXI master가 접근한다. 사용자 프로그램은 DMA 영역을 `mmap()`한다.

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

실제 서버는 먼저 `adas_classifier_drv.ko`를 로드하여
`/dev/adas_classifier`가 생성된 상태에서 실행한다. 실행 인자는 다음 명령으로
확인한다.

```bash
./arty/ps_eb/build/ps_classifier_server
```

배포용 INT8 모델과 golden은 다음 위치에 있다. **`_eb` 여야 한다 — `_db`
모델은 활성화·requant 산술이 달라 교환 불가.**

```text
arty/models/roi_classifier_int8_eb/export/
```

실행 시 model directory에는 위 `export/` 경로를 전달한다. GAP은 144개 값의
합계를 그대로 FC에 넣으므로 현재 `gap-div` 실행 인자는 `1`이다.

개념과 코드의 대응은 `docs/PS_PIPELINE_STUDY.md`에 정리되어 있다.
