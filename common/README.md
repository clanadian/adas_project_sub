# Common safety layer

`common/`은 [KR260 프로젝트](../common/ORIGIN)의 안전 판단 계층을 그대로 복사해
온 것이다. 그쪽은 APU(Linux)가 detection을 공유 메모리에 쓰고 RPU(R5
bare-metal)가 읽어 UART로 내보내는 2-코어 구조였다. **이 프로젝트(Jetson +
Arty)에는 RPU가 없다** — Jetson 프로세스 안 스레드 두 개가 그 역할을
대신한다. 아래는 이 저장소 기준 설명이다.

## 이 프로젝트에서의 데이터 흐름

```text
Jetson 분류 스레드
  → DetectionAdapter (bbox + class → DetectionRecord)
  → SafetyDecider
      → SafetyJudge   (이번 프레임 판단)
      → HazardLatch   (정지 이벤트 래치)
  → SafetyDecision (뮤텍스로 공유, 코어 간 공유 메모리 아님)
  → Jetson 송신 스레드 (20 ms 고정 주기)
      → UartFrame (state → 3-byte CRC-8 frame)
      → UartPort → Raspberry Pi
```

같은 프로세스 안 두 스레드가 뮤텍스 하나로 값을 주고받으므로, KR260의
`SafetyMessage`(seqlock 공유 DDR)에 해당하는 코어 간 동기화가 필요 없다.
전체 구조와 스레드를 나누는 이유는
[`../docs/JETSON_CONTROL_DESIGN.md`](../docs/JETSON_CONTROL_DESIGN.md) 참고.

## 이 프로젝트에서 실제로 쓰는 것

| 구성요소 | 역할 | 이 프로젝트에서 |
| --- | --- | --- |
| `SafetyJudge` | detection 위치·크기·점수로 `CLEAR/SLOW/STOP` 판단 | 사용 (`jetson/include/control/SafetyDecider.hpp`가 링크) |
| `HazardLatch` | 정지 이벤트 하나를 정지→유지→출발→재트리거 억제로 관리(아래 참고) | 사용, `release_ms` 추가(아래) |
| `UartFrame` | safety state를 CRC-8/SMBUS 3-byte frame으로 encoding | 사용, 그대로 |
| `SafetyMessage` | APU–RPU 공유 메시지(seqlock) | **미사용.** RPU가 없어 코어 간 공유가 필요 없다. KR260과의 동기화 경로 유지를 위해 지우지 않고 둔다 |
| `SafetyDebounce` | 위험은 즉시, 안전 해제는 지연하는 비대칭 debounce | **미사용.** `HazardLatch`가 그 역할을 대신한다. 독립 유틸리티로 남겨둠 |

빌드는 `common/CMakeLists.txt`(APU/RPU 양쪽을 위한 `ps_common` 정적 라이브러리)를
쓰지 않는다. `jetson/CMakeLists.txt`가 실제로 쓰는 세 개(`SafetyJudge.cpp`,
`SafetyHazardLatch.cpp`, `UartFrame.cpp`)만 직접 골라 `adas_safety` 타깃으로
링크한다.

### 안전 판단과 HazardLatch

위험 대상은 자동차·사람·표지판 3종(경고/규제/지시) 다섯 class 전부다.
car/person은 박스 화면상 위치와 높이를 거리·진행 경로의 대용값으로 쓴다.
표지판은 바닥에 닿는 대상이 아니라 높이 임계값 없이 경로 안에 있으면
곧바로 `Stop`이다.

`SafetyJudge`는 "이번 프레임에 뭐가 보이는가"만 순수하게 판단한다.
반복 트리거 방지와 재출발 시점은 `HazardLatch`가 맡는다 — 다섯 class
전부 같은 방식으로 다룬다.

```text
Idle    위험 없음. Stop급이 잡히면 즉시 Holding
Holding hold_ms(기본 3000ms) 동안 detection과 무관하게 무조건 Stop
Released hold_ms를 넘기면 진입. 트리거한 class를 제외하고 재판단해서
        출력(Clear/Slow일 수 있다). 그 class가 release_ms(기본 200ms) 이상
        && release_frames(기본 3) 이상 연속으로 안 보이면 Idle로 돌아가
        다음 감지를 새 이벤트로 받는다
        (다른 class가 새로 Stop급이면 Released 중이라도 즉시 새 이벤트)
```

시간과 프레임 수를 함께 쓰는 이유는 프레임률 하나에만 의존하면 안 되기
때문이다 — 자세한 근거는
[`../docs/JETSON_CONTROL_DESIGN.md`](../docs/JETSON_CONTROL_DESIGN.md) §4.

object tracking이 없어 "같은 개체인지 다른 개체인지"는 구분하지 못하고
class 단위로만 근사한다. car/person도 표지판과 동일하게 다루기로
했으므로, 출발 직후 같은 대상이 실제로 그 자리에 남아 있어도
`release_frames` 동안은 무시된다 — 데모에서 대상 등장을 기구로 직접
통제하는 것을 전제로 한 트레이드오프다.

카메라·링크 장애 같은 fail-safe(`forceStop`)는 `HazardLatch`를 거치지 않고
곧바로 `Stop`을 낸다. "장애가 났으니 T초 기다렸다 출발"은 fail-safe로
말이 안 되기 때문이다.

판단 임계값(`JudgeConfig`)과 `hold_ms`/`release_ms`/`release_frames`
(`HazardLatch::Config`)는 실보드에서 조정할 설정값이다. `zone_x`·`stop_height`
등 기하 임계값은 카메라 장착 조건에 달려 있어 실물 캘리브레이션이 필요하다
([`../docs/JETSON_CONTROL_DESIGN.md`](../docs/JETSON_CONTROL_DESIGN.md) §11).
상태 값과 순서는 UART 계약과 연결되어 있으므로 임의로 변경하지 않는다.

## R5 호환 규칙 — 왜 아직 지키는가

이 프로젝트에는 R5가 없지만, 이 디렉터리는 여전히 **Linux와 R5 bare-metal
양쪽에서 컴파일 가능한 제약**을 지킨다. 그래야 여기서 고친 것을 KR260
저장소(원본, R5가 실제로 있는 쪽)로 되돌릴 수 있다(`ORIGIN` 규칙).

허용하는 것:

- 고정 크기 POD 구조체와 배열
- 호출자가 제공한 포인터와 buffer
- `<cstdint>`, `<cstddef>` 수준의 공통 C++ 기능
- 결정적인 순수 계산

사용하지 않는 것:

- 힙 할당과 동적 컨테이너
- 파일 I/O, thread, mutex와 Linux system call
- OpenCV 또는 Xilinx BSP 직접 의존성
- 예외, RTTI와 소유권이 불분명한 포인터

하드웨어 접근이나 스레드·뮤텍스가 필요한 부분(`SafetyTransmitter`,
`UartPort` 등)은 `common/`에 넣지 않고 `jetson/include/control/`에 둔다.

## 빌드와 테스트

이 프로젝트는 `common/`을 독립적으로 빌드하지 않는다. `jetson/`의 일부로
빌드·테스트된다.

```bash
cmake -S jetson -B jetson/build
cmake --build jetson/build -j2
ctest --test-dir jetson/build --output-on-failure
```

관련 테스트:

- `test_detection_adapter`: 좌표 변환, class 매핑, Unclassified 4경로
- `test_safety_decider`: zone·높이 경계, 표지판 규칙, 래치 전이 (`SafetyJudge`/`HazardLatch` 검증 포함)
- `test_safety_transmitter`: 주기, watchdog, STOP 즉시 송신, 프레임 바이트

## Jetson-Arty 판에서 바뀐 것

이 저장소에서 두 가지를 추가했다. **둘 다 기본값이 KR260 동작과 같아** 그쪽
저장소로 되돌려도 거동이 바뀌지 않는다 (`ORIGIN` 규칙).

| 추가 | 왜 |
| --- | --- |
| `JudgeConfig::classes` (`ClassMap`) | 모델의 클래스 순서가 프로젝트마다 다르다. Jetson-Arty 판은 `background`가 앞에 붙어 전체가 한 칸씩 밀린다. 상수를 소스에 박으면 반대쪽에서 **오류 없이 car를 person으로 판단한다** |
| `HazardLatch::Config::release_ms` | `release_frames`만 쓰면 판단 주기가 바뀔 때 해제 시간이 같이 흔들린다. KR260은 20ms 고정 tick이었지만 Jetson은 프레임률이 ROI 개수에 따라 프레임마다 달라진다. 0이면 시간 조건 없음(기존 동작) |

`common/`을 수정할 때는 다른 변경과 섞지 말고 별도 커밋으로 남긴다
(`ORIGIN` 규칙).
