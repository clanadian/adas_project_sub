# Common safety layer

`common/`은 APU/Linux와 RPU/Cortex-R5F가 함께 컴파일하는 안전 계층이다.
APU가 detection을 공유 메모리에 기록하고 RPU가 이를 읽어 안전 상태와 UART
frame을 만들기 때문에 양쪽이 동일한 메시지·정책·프로토콜 선언을 사용한다.

## 데이터 흐름

```text
APU detections
  → SafetyMessage publish (shared DDR, seqlock)
  → RPU SafetyMessage read
  → SafetyJudge
  → HazardLatch
  → UartFrame
  → Raspberry Pi
```

주기 실행, stale APU 감지와 실제 UART 송신은 `rpu/SafetyLoop`가 담당한다.
`common/`은 특정 운영체제나 하드웨어에 닿지 않는 순수 로직만 포함한다.

## 구성요소

| 구성요소 | 역할 |
| --- | --- |
| `SafetyMessage` | APU–RPU 고정 크기 공유 메시지와 seqlock snapshot |
| `SafetyJudge` | detection 위치·크기·점수로 `CLEAR/SLOW/STOP` 판단 |
| `HazardLatch` | 정지 이벤트 하나를 정지→유지→출발→재트리거 억제로 관리(아래 참고) |
| `SafetyDebounce` | 위험은 즉시, 안전 해제는 지연하는 범용 비대칭 debounce. 현재 `SafetyLoop`는 안 쓰고 `HazardLatch`가 그 역할을 대신한다 — 독립 유틸리티로 남겨둠 |
| `UartFrame` | safety state를 CRC-8/SMBUS 3-byte frame으로 encoding |

### 공유 메시지

`SafetyMessage`는 DDR에 그대로 놓이는 고정 크기 POD 구조체다. APU와 RPU가
mutex를 공유하지 않으므로 `sequence`가 홀수면 쓰는 중, 같은 짝수 값으로 읽기
전후가 감싸지면 온전한 snapshot으로 판단하는 seqlock 방식을 사용한다.

메시지 layout을 바꾸면 `kMessageVersion`과 양쪽 소비 코드를 함께 검토한다.
구조체에 포인터나 동적 크기 필드를 추가하면 안 된다.

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
        출력(Clear/Slow일 수 있다). 그 class가 release_frames(기본 10)
        프레임 연속으로 안 보이면 Idle로 돌아가 다음 감지를 새 이벤트로 받는다
        (다른 class가 새로 Stop급이면 Released 중이라도 즉시 새 이벤트)
```

object tracking이 없어 "같은 개체인지 다른 개체인지"는 구분하지 못하고
class 단위로만 근사한다. car/person도 표지판과 동일하게 다루기로
했으므로, 출발 직후 같은 대상이 실제로 그 자리에 남아 있어도
`release_frames` 동안은 무시된다 — 데모에서 대상 등장을 컨베이어·랙피니언
같은 기구로 직접 통제하는 것을 전제로 팀이 승인한 트레이드오프다.

`stale_timeout_ms` fail-safe(APU 정지 감지)는 `HazardLatch`를 거치지 않고
`SafetyLoop`가 곧바로 `Stop`을 낸다. "APU가 죽었으니 T초 기다렸다 출발"은
fail-safe로 말이 안 되기 때문이다.

판단 임계값(`JudgeConfig`)과 `hold_ms`/`release_frames`(`HazardLatch::Config`)
는 실보드에서 조정할 설정값이다. 상태 값과 순서는 UART 계약과 연결되어
있으므로 임의로 변경하지 않는다.

## R5 호환 규칙

이 디렉터리의 코드는 Linux와 R5 bare-metal 양쪽에서 컴파일돼야 한다.

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

하드웨어 접근이 필요하면 `common/`에 넣지 않고 APU 또는 RPU의 platform
경계 뒤에 둔다.

## 빌드와 테스트

`common/`은 독립 실행파일이 아니라 `ps_common` 정적 라이브러리다. APU 테스트
빌드에서 APU·RPU 양쪽 사용 방식을 함께 검증한다.

```bash
cmake -S apu -B apu/build -DBUILD_TESTS=ON
cmake --build apu/build -j
ctest --test-dir apu/build -R 'safety|rpu_loop' --output-on-failure
```

- `test_safety`: 공유 메시지, 판단, HazardLatch, debounce, CRC와 UART vector
- `test_rpu_loop`: cache invalidate 호출, 주기 송신, stale APU와 송신 실패

전체 테스트 설명은 [APU test guide](../apu/tests/README.md)에 있다.

R5 Application은 이 소스를 직접 참조하지 않고 복사본을 사용한다. 수정 후
실제 R5 ELF를 빌드할 때는 반드시 동기화한다.

```bash
./rpu/vitis/sync_sources.sh
```

## 관련 계약

- [UART protocol](../platform/handoff/turtlebot/UART_PROTOCOL_v0.md)
- [TurtleBot control plan](../platform/handoff/turtlebot/TURTLEBOT_CONTROL_PLAN_v0.md)
- [Model I/O](../platform/contracts/model_io_v0.json)
- [RPU guide](../rpu/README.md)

프로토콜 byte나 상태 의미를 바꿀 때는 코드만 수정하지 말고 계약 문서, 생성
스크립트, binary vector와 테스트를 함께 갱신한다.
