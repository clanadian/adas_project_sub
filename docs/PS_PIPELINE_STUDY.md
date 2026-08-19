# Jetson–Arty 분류 파이프라인 학습 노트

## 1. 전체 데이터 흐름

```text
Jetson 카메라
  → 객체 후보 bbox
  → 정사각 crop, 96×96 resize
  → BGR에서 RGB로 변환
  → TCP 요청

Arty PS
  → 96×96×3 RGB UINT8 수신
  → INT8 양자화와 1픽셀 zero padding
  → 98×98×3 입력을 DDR에 기록
  → AXI-Lite로 PL 시작

Arty PL
  → DDR에서 입력과 Conv weight 읽기
  → Conv/Pool 3단 실행
  → 12×12×64 INT8 결과를 DDR에 기록

Arty PS
  → GAP
  → FC
  → argmax
  → TCP 응답
```

## 2. Jetson TCP client

관련 코드: `jetson/src/network/TcpRoiClient.cpp`

TCP는 한 번의 `send()`나 `recv()`가 요청한 전체 길이를 처리한다고 보장하지 않는다.
따라서 `sendAll()`과 `receiveAll()`은 누적 길이가 목표 길이에 도달할 때까지 반복한다.

`classify()`의 순서는 다음과 같다.

1. TCP 연결과 ROI 형식을 검사한다.
2. OpenCV 행렬이 연속 메모리인지 확인한다.
3. 요청 header를 network byte order로 encode한다.
4. header 20바이트와 RGB payload를 보낸다.
5. 응답 header와 result payload를 받는다.
6. frame ID와 ROI ID가 요청과 일치하는지 검사한다.
7. class ID와 confidence를 호출자에게 반환한다.

## 3. TCP header와 payload

관련 코드: `shared/include/roi_protocol.h`

- header: 뒤에 오는 데이터의 종류, 식별자, 길이를 설명한다.
- payload: 실제 ROI 픽셀 또는 분류 결과다.
- 다중 바이트 정수는 big-endian으로 전송한다.

서로 다른 CPU가 같은 바이트를 같은 숫자로 해석하도록 host 구조체를 직접 보내지 않고
`encode`/`decode` 함수를 거친다.

## 4. PS 전처리

관련 코드: `arty/ps_db/src/preprocess/roi_preprocessor.c`

Jetson에서 받은 값은 `96×96×3 RGB UINT8`이다. PS는 다음 변환을 수행한다.

```text
q = round(pixel × 127 / 255)
96×96×3 → 상하좌우 INT8 0 테두리 → 98×98×3
```

PL의 첫 Conv 엔진이 내부 padding을 하지 않으므로 PS가 1픽셀 테두리를 만든다.
입력과 출력 index가 다른 이유도 이 테두리 때문이다.

## 5. DDR 버퍼와 AXI-Lite의 차이

관련 코드:

- `classifier_buffers.c`: DDR 안에서 데이터가 놓일 주소 계산
- `classifier_accelerator.c`: 계산한 주소를 PL 레지스터에 전달

```text
DDR
├── 입력 feature map
├── Conv0/1/2 weight
└── PL 출력 feature map

AXI-Lite register
├── 위 DDR 주소 숫자
├── bias와 requant 값
└── ap_start / ap_done
```

DDR에는 큰 데이터가 들어가고, AXI-Lite에는 작은 설정값과 제어값이 들어간다.
PS가 AXI-Lite 레지스터에 DDR 주소를 쓰면 PL의 AXI master가 해당 DDR 주소를 읽고 쓴다.

## 6. MMIO와 mmap

관련 코드: `arty/ps_db/src/accelerator/pl_mmio.c`

MMIO는 장치 레지스터에 메모리 주소가 배정된 방식이다. Linux 사용자 프로그램은 물리주소를
직접 포인터처럼 사용할 수 없으므로 `/dev/mem`을 열고 `mmap()`으로 프로세스 가상주소에
연결한다.

```text
PL 레지스터 물리주소
  → /dev/mem
  → mmap
  → 사용자 프로세스의 volatile 포인터
```

`munmap()`은 이 가상주소 연결을 해제한다. `close()` 뒤 구조체를 다시 초기화하는 이유는
이미 닫힌 fd나 이전 포인터를 실수로 다시 쓰지 못하게 하기 위해서다.

## 7. 가속기 제어

관련 코드: `arty/ps_db/src/accelerator/classifier_accelerator.c`

1. 두 AXI-Lite 영역을 mmap한다.
2. 입력, weight, 출력 DDR 물리주소를 쓴다.
3. bias와 requant multiplier/shift를 쓴다.
4. `ap_start` 비트를 쓴다.
5. `ap_done`이 올라올 때까지 polling한다.
6. 제한 시간을 넘으면 timeout을 반환한다.

이 코드는 Conv 계산을 수행하지 않는다. 계산에 필요한 위치와 파라미터를 알려주고 PL의
실행을 제어한다.

## 8. GAP, FC, argmax

관련 코드: `arty/ps_db/src/postprocess/classifier_postprocess.c`

PL 출력은 `12×12×64`이다. GAP는 각 채널의 12×12 값 144개를 하나로 합쳐 특징 64개를
만든다.

```text
12×12×64 → GAP → 64
```

FC는 특징 64개에 클래스별 weight를 곱하고 bias를 더해 클래스별 logit을 만든다.

```text
logit[class] = bias[class]
             + Σ(feature[channel] × weight[class][channel])
```

`argmax`는 가장 큰 logit의 class ID를 선택한다. confidence는 class ID와 별개이며,
logit scale과 softmax 계약이 확정돼야 계산할 수 있다.

## 9. 모델 로더

관련 코드: `arty/ps_db/src/model/classifier_model.c`

모델 로더는 Conv/FC 바이너리를 읽고 파일 크기가 하드웨어 계약과 정확히 같은지 검사한다.
일부 파일만 읽힌 모델은 사용하지 않고 모두 정리한다.

현재 requant, 클래스 수, GAP divisor는 실행 인자로 전달한다. 최종 manifest 형식이 확정되면
이 값을 manifest에서 읽도록 교체한다.

## 10. main의 역할

Jetson main: `jetson/tools/jetson_roi_client.cpp`

- 카메라와 TCP 연결을 연다.
- 프레임마다 후보를 생성한다.
- crop과 RGB 전처리를 적용한다.
- ROI별 분류를 요청하고 결과를 출력한다.

PS main: `arty/ps_db/tools/ps_classifier_server.c`

- 모델과 예약 DDR을 준비한다.
- PL 레지스터와 파라미터를 설정한다.
- TCP 요청을 반복 수신한다.
- 전처리, PL 실행, GAP/FC, 응답을 순서대로 수행한다.

## 11. 아직 외부 산출물이 필요한 부분

- Jetson proposal ONNX는 `jetson/models/proposal/`에 반영됨
- Arty INT8 모델과 golden은 `arty/models/roi_classifier_int8_db/`에 반영됨
- `RoiProposer`의 TensorRT C++ 추론 코드
- 최종 XSA/bitstream 및 Linux device tree
- 예약 DDR 물리주소
- logit scale과 confidence 계산 방식
- golden input/output을 이용한 실제 PL 일치 검증
