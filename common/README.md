# Common safety layer

`common/`은 [KR260 프로젝트](../common/ORIGIN)의 안전 판단 계층을 그대로 복사해
온 것이다. 그쪽은 APU(Linux)가 detection을 공유 메모리에 쓰고 RPU(R5
bare-metal)가 읽어 UART로 내보내는 2-코어 구조였다. **이 프로젝트에는
RPU가 없고, 최종 DB 구성에서는 Arty PS Linux 프로세스가 판단과 UART 송신을
맡는다.** Jetson에는 이 판단·UART 계층을 두지 않는다.

## 이 프로젝트에서의 데이터 흐름

```text
Jetson
  → bbox + ROI를 Arty PS에 전송

Arty PS 분류 서버
  → PL 분류 결과 + bbox → DetectionRecord
  → SafetyJudge   (이번 프레임 판단)
  → HazardLatch   (car/person Stop 이벤트 래치)
  → SafetyDecision
  → PS 송신 스레드 (20 ms 고정 주기)
      → UartFrame (state → 3-byte CRC-8 frame)
      → UartPort(`/dev/ttyPS1`) → Raspberry Pi
```

같은 PS 프로세스 안 두 스레드가 판단과 송신을 분리하므로, KR260의
`SafetyMessage`(seqlock 공유 DDR)에 해당하는 코어 간 동기화가 필요 없다.
전체 구조와 스레드를 나누는 이유는
최종 결선은 [`../arty/ps_db/README.md`](../arty/ps_db/README.md)를 참고한다.

## 이 프로젝트에서 실제로 쓰는 것

| 구성요소 | 역할 | 이 프로젝트에서 |
| --- | --- | --- |
| `SafetyJudge` | detection 위치·크기·점수로 `CLEAR/SLOW/STOP` 판단 | 사용 (`arty/ps_db/src/control/ps_safety_bridge.cpp`가 링크) |
| `HazardLatch` | 정지 이벤트 하나를 정지→유지→출발→재트리거 억제로 관리(아래 참고) | 사용, `release_ms` 추가(아래) |
| `UartFrame` | safety state를 CRC-8/SMBUS 3-byte frame으로 encoding | 사용, 그대로 |
| `SafetyMessage` | APU–RPU 공유 메시지(seqlock) | **미사용.** RPU가 없어 코어 간 공유가 필요 없다. KR260과의 동기화 경로 유지를 위해 지우지 않고 둔다 |
| `SafetyDebounce` | 위험은 즉시, 안전 해제는 지연하는 비대칭 debounce | **미사용.** `HazardLatch`가 그 역할을 대신한다. 독립 유틸리티로 남겨둠 |

빌드는 `common/CMakeLists.txt`를 직접 쓰지 않는다. 최종 DB에서는
`arty/ps_db/CMakeLists.txt`의 `adas_ps_safety`가 `SafetyJudge.cpp`,
`SafetyHazardLatch.cpp`, `UartFrame.cpp`를 링크한다. Jetson은 이 계층을
링크하지 않는다.

### 안전 판단과 HazardLatch

위험 대상은 자동차·사람·표지판 3종(경고/규제/지시) 다섯 class다.
car/person은 박스 화면상 위치와 높이를 거리·진행 경로의 대용값으로 써
`Slow` 또는 `Stop`을 낸다. 표지판은 진행 경로 안에서 폭이
`sign_slow_width` 이상일 때만 `Slow`이며 **절대 `Stop`을 만들지 않는다.**
현재 분류기가 개별 정지 표지판을 구분하지 못하므로 모든 표지판에 정지를
거는 오동작을 피하기 위한 정책이다. (2026-08-21부터 높이 대신 폭을 쓴다 —
TurtleBot 카메라가 표지판을 올려다보는 각도라 세로 방향이 원근으로
찌그러져 높이만으로는 실제 거리보다 작게 잡혔다.)

`SafetyJudge`는 "이번 프레임에 뭐가 보이는가"만 순수하게 판단한다.
반복 트리거 방지와 재출발 시점은 `HazardLatch`가 맡는다. 래치는 `Stop`에만
열리므로 car/person에만 적용되고, 표지판 `Slow`는 보이는 동안만 유지된다.

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
세부 임계값은 `SafetyJudge.hpp`와 Arty PS README에 기록한다.

object tracking이 없어 "같은 개체인지 다른 개체인지"는 구분하지 못하고
class 단위로만 근사한다. 이 제한은 Stop 이벤트를 만드는 car/person 래치에
적용된다. 표지판은 래치되지 않는다.

카메라·링크 장애 같은 fail-safe(`forceStop`)는 `HazardLatch`를 거치지 않고
곧바로 `Stop`을 낸다. "장애가 났으니 T초 기다렸다 출발"은 fail-safe로
말이 안 되기 때문이다.

판단 임계값(`JudgeConfig`)과 `hold_ms`/`release_ms`/`release_frames`
(`HazardLatch::Config`)는 실보드에서 조정할 설정값이다. `zone_x`·`stop_height`
등 기하 임계값은 카메라 장착 조건에 달려 있어 실물 캘리브레이션이 필요하다
테스트는 Arty PS 빌드에서 함께 수행한다.
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
`UartPort` 등)은 `common/`에 넣지 않고 `arty/ps_db`에 둔다.

## 빌드와 테스트

이 프로젝트는 `common/`을 독립적으로 빌드하지 않는다. PS와 Jetson 빌드가
필요한 소스를 직접 링크한다.

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

이 저장소에서 세 가지를 추가·변경했다. 앞의 두 항목은 기본값이 KR260 동작과
호환되지만, 표지판 정책은 이 프로젝트 요구에 맞춘 의도적인 동작 변경이다.

| 추가 | 왜 |
| --- | --- |
| `JudgeConfig::classes` (`ClassMap`) | 모델의 클래스 순서가 프로젝트마다 다르다. Jetson-Arty 판은 `background`가 앞에 붙어 전체가 한 칸씩 밀린다. 상수를 소스에 박으면 반대쪽에서 **오류 없이 car를 person으로 판단한다** |
| `HazardLatch::Config::release_ms` | `release_frames`만 쓰면 판단 주기가 바뀔 때 해제 시간이 같이 흔들린다. KR260은 20ms 고정 tick이었지만 현재 PS 판단 갱신 주기는 Jetson의 ROI 개수에 따라 달라진다. 0이면 시간 조건 없음(기존 동작) |
| `JudgeConfig::sign_slow_width` | 개별 정지표지판을 구분하지 못하므로 표지판은 가까울 때 `Slow`까지만 내고 `Stop`·래치를 만들지 않는다. 높이가 아니라 폭으로 판단한다(위 카메라 각도 사유) |

`common/`을 수정할 때는 다른 변경과 섞지 말고 별도 커밋으로 남긴다
(`ORIGIN` 규칙). 특히 표지판 정책은 KR260에 그대로 back-port하지 말고 해당
프로젝트의 클래스 의미를 먼저 확인한다.
