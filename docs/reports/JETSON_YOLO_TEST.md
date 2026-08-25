# Arty FPGA vs Jetson GPU — ROI 분류기 A/B/C 실험

## 1. 목적과 범위

96×96 ROI용 3층 CNN을 Arty PL에서 실행할 때와 Jetson Nano GPU에서
실행할 때의 지연을 비교한다. 안전 판단, HazardLatch, UART, RPi 제어는
실험 범위에서 제외하며 기존 동작도 변경하지 않는다.

```text
A: Arty PL 기준선
B: Jetson TensorRT 분류기 단독
C: Jetson 한 GPU에서 YOLO -> 분류기 순차 실행
```

비교 종점을 두 개로 분리한다.

| 종점 | Arty 기준 | Jetson 엔진 |
|---|---:|---|
| Conv/ReLU/Pool ×3 → 12×12×64 | PL `6.62 ms` | `classifier_features.engine` |
| GAP+FC까지 포함한 분류 | PS+PL `6.88 ms` | `classifier_full.engine` |
| 네트워크 포함 Jetson 체감 | RTT `8.24 ms` | 로컬 호출이므로 직접 대응 없음 |

Arty 값의 출처는 `docs/reports/SHUTDOWN_LOG_2026-08-21_1630.md`다.

## 2. 모델 재현 원칙

재학습하지 않고 다음 배포 산출물을 사용한다.

```text
arty/models/roi_classifier_int8_db/export/
```

구조:

```text
1×3×96×96
 -> Conv 3→16, 3×3, pad 1 -> ReLU -> MaxPool
 -> Conv 16→32, 3×3, pad 1 -> ReLU -> MaxPool
 -> Conv 32→64, 3×3, pad 1 -> ReLU -> MaxPool
 -> 1×64×12×12
 -> Global Average Pool -> FC 64→6
```

FPGA의 98×98 입력은 pad 포트가 없는 conv0 엔진을 위한 pre-padding이다.
Jetson 그래프는 96×96 입력과 `Conv2d(padding=1)`을 사용한다.

### INT8 파일을 단순 `.float()` 하면 안 되는 이유

`.bin`은 양자화된 정수이므로 `manifest.json`의 scale로 역양자화한다.

```text
conv weight = int8 × weight_scale
conv bias   = int32 × input_scale × weight_scale
FC weight   = int8 × fc.weight_scale
FC bias     = int32 × logits_scale
```

`w_conv0`은 OIHW이고 `w_conv1/2`는 WPACK `[O][H][W][I]`이므로 후자는
OIHW `[O][I][H][W]`로 전치한다.

이 FP 그래프에는 FPGA의 레이어별 INT8 requant가 없다. 따라서 속도 비교에는
적합하지만 bit-exact 모델은 아니다. golden argmax 비교는 sanity check이며
불일치가 곧 export 실패를 뜻하지 않는다.

### Bias rounding compensation 처리

배포된 `b_conv*.bin`에는 고정소수점 requant의 truncation 편향을 보정하는 값이
이미 포함돼 있다(`bias_includes_rounding_compensation`: conv0=94, conv1=15,
conv2=97). `export_classifier_onnx.py`는 이 값을 **빼지 않고**, 전달된 INT32
bias 전체에 `input_scale × weight_scale`을 곱한다.

이 선택은 원래 FP32 학습 모델을 복원하기 위한 것이 아니라 **실제로 배포된
INT8 parameter set에 가장 가까운 연속 FP graph**를 만들기 위한 것이다.
원래 FP32 모델을 재현하려면 보정항을 빼는 것보다 학습 checkpoint를 직접
사용하는 편이 맞다. 어느 방식도 중간 INT8 requant를 제거한 FP16 graph와
bit-exact하지 않으며, 이번 latency 측정에서는 가중치 숫자가 실행시간에
유의미한 영향을 주지 않는다.

## 3. 준비된 코드

| 파일 | 역할 |
|---|---|
| `jetson/tools/export_classifier_onnx.py` | INT8 bin+manifest 역양자화, feature/full ONNX 생성, golden sanity check |
| `jetson/tools/classifier_gpu_benchmark.cpp` | CUDA event로 B/C GPU compute median/mean/p95/max 측정 |
| `jetson/CMakeLists.txt` | TensorRT가 있을 때 `classifier_gpu_benchmark` 빌드 |

## 4. ONNX 생성

PyTorch, NumPy, ONNX가 설치된 PC 또는 학습용 컴퓨터에서 실행한다. ONNX는
FP32 graph로 만들고 Jetson의 TensorRT build 단계에서 FP16을 적용한다.

```bash
cd /home/adas/adas_project_sub

python3 jetson/tools/export_classifier_onnx.py \
  arty/models/roi_classifier_int8_db/export \
  jetson/models/classifier_benchmark
```

산출물:

```text
jetson/models/classifier_benchmark/classifier_features.onnx
jetson/models/classifier_benchmark/classifier_full.onnx
jetson/models/classifier_benchmark/export_metadata.json
```

현재 adas-pc 기본 Python에는 `torch`, `numpy`, `onnx`가 없다. 의존성을 설치할
환경을 정한 뒤 실행해야 하며, 시스템 Python에 임의 설치하지 않는다.

## 5. Jetson에서 TensorRT 엔진 생성

Jetson 연결 후 ONNX와 최신 `jetson/` 소스를 복사한다. 먼저 전력·클럭 조건을
고정하고 기록한다.

```bash
sudo nvpmodel -m 0
sudo jetson_clocks
sudo nvpmodel -q
sudo jetson_clocks --show
```

JetPack 4.x의 `trtexec` 경로:

```bash
TRTEXEC=/usr/src/tensorrt/bin/trtexec
MODEL=~/adas_project_sub/jetson/models/classifier_benchmark

$TRTEXEC --onnx=$MODEL/classifier_features.onnx \
  --saveEngine=$MODEL/classifier_features.engine \
  --fp16 --workspace=1024 --buildOnly

$TRTEXEC --onnx=$MODEL/classifier_full.onnx \
  --saveEngine=$MODEL/classifier_full.engine \
  --fp16 --workspace=1024 --buildOnly
```

엔진은 생성한 Jetson에서만 사용한다. TensorRT engine은 저장소에 커밋하지 않는다.

## 6. 벤치마크 빌드

Jetson의 CMake 3.10은 `-S/-B`와 `cmake --build ... -j2` 신문법을 지원하지
않으므로 다음처럼 실행한다.

```bash
cd ~/adas_project_sub/jetson
mkdir -p build_benchmark
cd build_benchmark
cmake .. -DCMAKE_BUILD_TYPE=Release
cmake --build . --target classifier_gpu_benchmark -- -j2
```

## 7. B — 분류기 단독

PL 종점과 비교:

```bash
./classifier_gpu_benchmark \
  ../models/classifier_benchmark/classifier_features.engine \
  - 1000 100
```

GAP+FC 포함 분류 전체 비교:

```bash
./classifier_gpu_benchmark \
  ../models/classifier_benchmark/classifier_full.engine \
  - 1000 100
```

출력은 device buffer를 미리 할당하고 zero input으로 `enqueueV2()`만 반복한
**GPU compute 시간**이다. H2D/D2H, crop, RGB 변환, 제어 로직은 포함하지 않는다.

CUDA event는 event 사이에서 GPU가 소비한 시간을 잰다. CPU의 API 호출·kernel
launch 비용과 H2D/D2H 전송은 포함하지 않는다. 따라서 이 값을 네트워크까지
포함한 Arty RTT `8.24 ms`와 같은 종류의 수치처럼 직접 비교하지 않는다.

```text
PL 6.62 ms       <-> feature engine GPU compute
PS+PL 6.88 ms    <-> full engine GPU compute (범위가 가장 가까운 참고 비교)
Arty RTT 8.24 ms <-> Jetson 실제 로컬 호출 전체 시간 (현재 도구는 미측정)
```

발표 표에서도 위 세 줄을 합쳐 쓰지 않는다. 필요하면 Jetson 실제 입력 복사,
enqueue, 출력 복사와 CPU 동기화까지 포함한 host-side latency를 별도로 추가한다.

## 8. C — YOLO와 GPU 공유

현재 proposal engine과 동일한 GPU, 동일한 CUDA stream에서 YOLO 뒤에
분류기를 순차 실행한다. 실제 파이프라인도 YOLO 결과가 나와야 ROI를 만들 수
있으므로 이 순서가 기본 비교 조건이다.

```bash
YOLO=../models/proposal/export/proposal_yolov8n_fp16.engine
CLS=../models/classifier_benchmark/classifier_features.engine

./classifier_gpu_benchmark "$CLS" "$YOLO" 1000 100
```

다음을 한 번에 출력한다.

- classifier alone
- YOLO alone
- paired YOLO
- paired classifier
- paired total

이 코드는 순차 실행 비용을 측정한다. 서로 다른 CUDA stream에서 실제 동시
실행했을 때의 contention은 별도 실험이며 이 결과에 포함되지 않는다.

따라서 C의 올바른 해석은 "YOLO가 이미 사용하는 GPU에 분류 작업을 추가했을
때 프레임당 GPU 점유 시간이 얼마나 늘어나는가"다. 동일 stream에서는 YOLO와
분류기가 겹쳐 실행되지 않으므로, 이 결과만으로 동시 실행 contention을
증명했다고 표현하지 않는다. 실제 파이프라인은 YOLO 결과로 ROI를 만든 뒤
분류하므로 이 순차 조건이 우선 비교 대상이다.

## 9. 결과 기록표

| 항목 | Arty | Jetson MAXN | Jetson 5W |
|---|---:|---:|---:|
| Conv/Pool 코어 median | PL `6.62 ms` | feature `0.5094 ms` | feature `0.7237~0.7864 ms` |
| 분류 전체 median | PS+PL `6.88 ms` | full `0.5359 ms` | full `0.9541~0.9666 ms` |
| 시스템 체감 | RTT `8.24 ms` | full `trtexec` E2E `0.5529 ms` | 미측정 |
| YOLO 단독 median | 독립 하드웨어 | `13.2378 ms` | `18.8027 ms` |
| YOLO→분류 합계 median | PL과 별도 자원 | `13.7231 ms` | `19.4961 ms` |
| YOLO 뒤 분류 추가 시간 | 네트워크로 별도 PL 사용 | `0.4930 ms` | `0.7022 ms` |
| golden argmax | class 2 (`person`) | class 2, 일치 | 동일 engine 사용 |

### 2026-08-24 Jetson Nano 실측

측정 조건은 MAXN, CPU `1.479 GHz`, GPU `921.6 MHz`, EMC `1.6 GHz`, 측정
직후 온도 약 `41 °C`다. TensorRT는 `8.2.1`, L4T는 `32.7.1`이며 각
classifier 측정은 warmup 100회 후 1,000회 실행했다.

```text
B, feature-only
median 0.5094 ms / mean 0.5107 / p95 0.5192 / max 0.6173

B, full
median 0.5359 ms / mean 0.5550 / p95 0.6064 / max 0.6542

C, 배포 YOLO engine -> full classifier, 동일 CUDA stream
YOLO alone          median 13.2378 ms
paired YOLO         median 13.2303 ms
paired classifier   median  0.4930 ms
paired total        median 13.7231 ms
```

FPGA golden ROI를 실제 TensorRT full engine에 입력한 결과 logits는
`[-2.74609, -4.67188, 1.02246, -7.91406, -7.35547, -6.25391]`였고,
argmax는 FPGA와 같은 class 2(`person`)였다. 따라서 ONNX에서 확인한 sanity
결과뿐 아니라 Jetson에서 생성한 FP16 engine의 최종 출력도 확인했다.

`trtexec`이 별도로 측정한 full engine의 median은 GPU compute `0.5350 ms`,
H2D/D2H와 host 동기화를 포함한 E2E host latency `0.5529 ms`였다. 이는
분류기 엔진 한 번의 로컬 호출 참고값이며, 실제 ROI crop이나 YOLO 후처리까지
포함한 애플리케이션 전체 지연은 아니다.

실측상 이 작은 CNN은 Jetson GPU에서 Arty보다 훨씬 빨랐다. 또한 동일 stream
C에서도 분류기 추가 GPU 점유 시간은 약 `0.49 ms`라서, 현재 워크로드에서는
FPGA 오프로딩의 지연시간상 이득이 확인되지 않았다. FPGA의 의미는 GPU보다
빠르다는 데 두지 않고, 탐지 GPU와 독립된 고정 데이터패스에서 분류를 수행한
이기종 구현 및 검증에 둔다.

### YOLO engine 주의사항

실제 데모에 사용한 `proposal_yolov8n_fp16.engine`을 주 결과로 사용했다. 이
engine을 deserialize할 때 TensorRT가 장치 모델 호환성 경고를 출력했지만,
현재 실시간 파이프라인에서 장시간 검증한 바로 그 배포 engine이다.

동일 Jetson에서 같은 ONNX를 새로 빌드한 engine도 비교했다. 새 engine은 YOLO
단독 median `20.8160 ms`, full classifier까지 합친 median `21.1424 ms`로 더
느렸다. TensorRT tactic 선택과 engine 생성 조건에 따라 YOLO 시간이 크게
달라질 수 있다는 뜻이므로, 발표에는 **실제 배포 engine 결과**임을 명시하고
서로 다른 engine의 수치를 섞지 않는다.

GPU가 더 빠르더라도 실험 실패가 아니다. 작은 CNN의 단독 latency에서는 GPU가
유리할 수 있고, FPGA의 장점은 YOLO와 연산 자원을 공유하지 않으며 PL 시간이
입력과 무관하게 약 6.62 ms로 일정하다는 점이다. 반대로 GPU 공유 비용까지
작다면 이 워크로드에서는 오프로딩의 네트워크 비용이 더 컸다는 결론을 그대로
기록한다.

### 2026-08-24 Jetson Nano 5W 실측

실제 보조배터리 운용 조건과 같은 5W 모드에서 MAXN 실험과 동일하게 warmup
100회 후 1,000회 측정했다. `jetson_clocks`는 5W 모드가 허용하는 범위 안에서
고정했으며 CPU 2코어 `921.6 MHz`, GPU `614.4 MHz`, EMC `1.6 GHz`였다.
측정 직후 CPU thermal zone은 약 `34 °C`였다.

```text
B, feature-only
median 0.7864 ms / mean 0.7892 / p95 0.8039 / max 1.1153

B, full
median 0.9541 ms / mean 0.9609 / p95 1.0071 / max 1.1398

B 반복 확인
feature-only median 0.7237 ms / full median 0.9666 ms

C, 배포 YOLO engine -> full classifier, 동일 CUDA stream
classifier alone    median  0.7687 ms
YOLO alone          median 18.8027 ms
paired YOLO         median 18.7933 ms
paired classifier   median  0.7022 ms
paired total        median 19.4961 ms
```

반복 측정까지 포함하면 5W의 feature-only 중앙값은 `0.7237~0.7864 ms`, full은
`0.9541~0.9666 ms`였다. MAXN 대비 각각 약 42~54%, 78~80% 느렸다. 동일 stream
C의 YOLO와 총시간은 각각 약 42% 늘었다. C 실행 안에서 먼저 측정한 full 단독
값은 `0.7687 ms`로 별도 B 실행보다 낮았으므로, 단독 비교에는 재현된 별도 B
범위를 사용하고 C의 `0.7022 ms`는 YOLO 직후 추가 점유 시간으로만 사용한다.

5W에서도 feature-only와 full은 Arty의 PL `6.62 ms`, PS+PL `6.88 ms`보다
짧았다. 따라서 실제 전력 조건에서도 작은 분류 CNN의 지연시간만 보면 Jetson
GPU가 빠르며, FPGA 채택 근거는 속도 우위가 아니라 GPU와 분리된 고정
데이터패스와 이기종 구현에 둔다.

5W 측정에 사용한 engine SHA-256:

```text
classifier_features.engine
5f7bec50d65daab4bfc8c0877f0b74c28a72daed8351e1ba6420b5d1cd1fde90

classifier_full.engine
180172d3c8ac5b1eb4b5e46547674aef8b6c0757871c0c64fcf3f5bf3abbcfc7

proposal_yolov8n_fp16.engine
f0d7a4061ed97a43ad99d2edb18801d3fb45c3dd3044ee0fbf4895d1f7a4a04e
```

## 10. 실험 재현 정보

결과와 함께 다음을 반드시 남긴다.

```text
Jetson model / JetPack / TensorRT version
nvpmodel -q
jetson_clocks --show
classifier ONNX 및 engine sha256
YOLO engine sha256
iterations / warmup
classifier_features와 classifier_full 중 어느 엔진인지
```

## 11. 설계 검토 합의사항

실험 전에 다시 확인할 합의사항이다.

- [x] INT8 bin을 단순 float cast하지 않고 manifest scale로 역양자화한다.
- [x] `w_conv1/2`의 WPACK을 OIHW로 전치한다.
- [x] PL 종점은 feature-only, PS 후처리 포함 종점은 full engine과 비교한다.
- [x] 배포 bias의 rounding-compensation은 빼지 않고 유지한다.
- [x] golden argmax는 sanity check이며 FP16과 INT8의 bit-exact 일치를 요구하지 않는다.
- [x] CUDA event 결과는 순수 GPU 시간으로 표시하고 Arty RTT와 구분한다.
- [x] C는 실제 의존 순서인 `YOLO -> classifier`의 추가 GPU 점유 시간을 잰다.
- [x] 동일 stream C를 동시 실행 contention 측정이라고 표현하지 않는다.
- [ ] Jetson 연결 후 host-side local latency(H2D+enqueue+D2H+sync)를 추가 측정할지 결정한다.
- [ ] 별도 CUDA stream 동시 실행 contention 실험은 순차 C 결과를 본 뒤 결정한다.
