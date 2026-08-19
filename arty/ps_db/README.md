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
| `adas_classifier_drv` | AXI-Lite와 coherent DMA 버퍼 소유 | 구현·실보드 검증 완료 |
| `classifier_device` | `/dev/adas_classifier` ioctl/mmap wrapper | 구현·빌드 확인 |
| `pl_mmio` | 초기 bring-up용 `/dev/mem` MMIO | 단위 테스트용으로 유지 |
| `classifier_registers` | AXI-Lite base와 register offset | 확정 |
| `classifier_accelerator` | buffer/parameter/start/done 제어 | 구현·실보드 검증 완료 |
| `dummy_roi_service` | PL 없이 고정 결과 반환 | 임시·테스트 |
| `classifier_model` | Conv/FC 바이너리 로드·크기 검사 | 구현·테스트 |
| GAP/FC/argmax | PL 출력 후처리 | 구현·실보드 bit-exact 검증 완료 |
| `adas_classifier_confidence_ppm` | logits×logits_scale → softmax → confidence_ppm | 구현·golden 벡터 일치 확인 |

`실보드 검증`은 `ps_db_golden_test`(PL 출력 bit-exact 대조)와 실제 Jetson 카메라
연동 테스트로 확인한 것이다. 자세한 내용과 수치는
[`../../docs/DB_ARTY_BRINGUP_REPORT.md`](../../docs/DB_ARTY_BRINGUP_REPORT.md)에
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
[`../../docs/FPS_MEASUREMENT_GUIDE.md`](../../docs/FPS_MEASUREMENT_GUIDE.md)에
있다.

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
