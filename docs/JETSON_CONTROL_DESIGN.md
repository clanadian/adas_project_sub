# Jetson 제어 로직 설계

Arty에서 받은 분류 결과로 TurtleBot을 제어하는 계층의 설계다.

- 상태: **구현 완료, 실보드 데모 1회 진행(2026-08-20).** 코드는
  `jetson/src/control/`, 단위 테스트 3종 통과. 그 데모에서 검출 0건
  프레임 때문에 Stop이 계속 걸려 바퀴가 안 도는 문제를 발견·수정했다(§6
  heartbeat ROI). §11 캘리브레이션은 여전히 임시값(KR260 값 그대로)이다.
- 설정값은 전부 기본값을 정해 두었다(§10). 구현을 시작하기 위해 기다려야 하는
  측정은 없다. **카메라를 로봇에 장착한 뒤 자로 재는 캘리브레이션 한 가지**만
  실물이 필요하다(§11).
- KR260 프로젝트에서 가져온 것과 바꾼 것은 부록에 따로 정리했다.

---

## 1. 구조

```text
┌─ Jetson Nano ───────────────────────────────────────────────────────┐
│                                                                     │
│  [분류 스레드 — 기존 메인 루프]         [안전 송신 스레드 — 신설]   │
│   주기: 가변 (프레임률)                  주기: 20 ms 고정 (50 Hz)   │
│                                                                     │
│  카메라 캡처                                  │                     │
│      ↓                                        │ ① 판단 신선도 검사  │
│  YOLOv8n proposal (bbox만, class 없음)        │    (오래되면 STOP)  │
│      ↓                                        │                     │
│  경로 zone 사전 필터                          │ ② 3-byte frame      │
│      ↓                                        │    A5 / state / CRC8│
│  ROI crop → TCP → Arty → class_id             │                     │
│      ↓                                        │ ③ UART 송신         │
│  판단 (zone·거리·클래스) → 이벤트 래치        │                     │
│      ↓                                        ↓                     │
│  ┌── 최신 판단 { state, 판단 시각 } ───────────┐                    │
│  └─────────────────────────────────────────────┘  STOP 진입 시      │
│                                                    즉시 1회 추가    │
└─────────────────────────────────────────────────────────┬───────────┘
                                                          │ UART 115200 8N1
                                                          ▼
                                            Raspberry Pi (ROS2 arbiter)
                                                          ↓ /cmd_vel
                                                      TurtleBot3
```

### 왜 스레드를 나누는가

분류 루프는 **주기가 일정하지 않다.** 프레임당 ROI 개수 N이 달라지고, ROI마다
TCP 왕복이 있어 프레임 시간이 그때그때 변한다. 이 루프에서 직접 UART를 보내면
송신 간격이 그대로 흔들리고, 느린 프레임 한 번에 RPi가 통신 두절로 판단할 수
있다.

**송신은 고정 20 ms로 독립시키고, 분류 루프는 "최신 판단"만 갱신한다.**
그러면 판단이 늦어져도 링크는 살아 있고, 판단이 아예 멈춘 것은 송신 스레드가
감지한다.

**감시(watchdog)를 송신 스레드에 두는 것이 핵심이다.** 분류 스레드가 멈추면
스스로 그 사실을 보고할 수 없다. 밖에서 보는 쪽이 있어야 한다.

두 스레드가 공유하는 것은 이것 하나뿐이다.

```cpp
struct SafetyDecision {
    safety::State state{safety::State::Stop};  // 초기값은 Stop
    std::uint64_t decided_at_ms{0};
    std::uint32_t frame_id{0};
};
```

20 ms에 한 번 읽고 프레임마다 한 번 쓴다. 경합이 없으므로 `std::mutex` 하나면
충분하다.

### 왜 판단이 Jetson인가

Arty는 96×96 픽셀만 받는다. 그 crop이 **화면 어디에 있었는지, 얼마나 컸는지를
구조적으로 알 수 없다.** 경로 판정과 거리 추정이 전부 bbox 기하에 의존하므로
판단은 bbox를 가진 쪽에 있어야 한다.

ROI 프로토콜에 bbox를 실어 Arty가 판단하게 만들 수도 있지만, 그러면 임계값
하나 바꾸는 데 보드 바이너리를 다시 올려야 한다. **Arty는 "이 96×96이
무엇인가"만 답하는 순수 함수로 남긴다.**

---

## 2. 데이터 흐름

### 2.1 두 조각을 하나로 합친다

Jetson은 bbox를, Arty는 class를 준다. 판단에 쓰려면 합쳐야 한다.

| 출처 | 자료형 | 내용 |
| --- | --- | --- |
| `RoiProposer` | `RoiCandidate` | 픽셀 좌표 bbox(좌상단+크기), `objectness` |
| `TcpRoiClient` | `ClassificationResult` | `status`, `class_id`, `confidence_ppm` |
| **합친 결과** | `safety::DetectionRecord` | 정규화 `x1y1x2y2`, `score`, `class_id` |

```cpp
// jetson/include/control/DetectionAdapter.hpp
namespace adas::control {

struct AdapterConfig {
    int frame_width{640};
    int frame_height{360};

    // 안전 판단에 쓸 최소 objectness. RoiProposer의 conf(0.10)와 별개다 -
    // "후보로 볼 것"과 "제동 근거로 쓸 것"의 기준은 다르다.
    float min_objectness{0.25F};

    // 분류 신뢰도 게이트. 0이면 끈 것이다(§10에서 초기값 0을 쓰는 이유).
    std::uint32_t min_confidence_ppm{0u};
};

enum class AdaptResult {
    Hazard,        // 판단에 넣는다
    Background,    // 분류기가 "물체 아님"이라고 했다 - 버린다
    Unclassified,  // class를 못 얻었다 - §3.3
    Rejected,      // objectness 미달 - 버린다
};

AdaptResult adapt(const adas::roi::RoiCandidate& candidate,
                  const adas::network::ClassificationResult& result,
                  const AdapterConfig& config,
                  safety::DetectionRecord& out);

}  // namespace adas::control
```

### 2.2 crop 영역이 아니라 원본 bbox를 쓴다

`RoiCropper`가 만드는 crop은 **15% 여백 + 정사각 확장**이 들어가 있어 실제
물체보다 크다. 거리 대용값(박스 높이)에 그걸 쓰면 실제보다 가깝다고 판단한다.
`RoiCandidate::object_bbox`(proposal 원본)를 쓴다.

```text
x1 = bbox.x / frame_width          y1 = bbox.y / frame_height
x2 = (bbox.x + bbox.width)  / frame_width
y2 = (bbox.y + bbox.height) / frame_height
```

---

## 3. 판단 규칙

### 3.1 클래스

`docs/contracts/ROI_CLASSIFIER_CONTRACT.md` §7의 순서를 그대로 쓴다.

| id | class | 취급 |
| ---: | --- | --- |
| 0 | background | **버린다.** 분류기가 명시적으로 "물체 아님"이라고 한 것이다 |
| 1 | car | 경로 안 + 가까움 → Stop / 중간 → Slow |
| 2 | person | car와 동일 |
| 3 | sign_warning | 경로 안이면 거리와 무관하게 **Stop** |
| 4 | sign_prohibition | 동일 |
| 5 | sign_mandatory | 동일 |

**표지판에 거리 기준을 두지 않는 이유:** 모델이 개별 표지가 아니라 종류만
구분한다. "무슨 표지인지" 모르는 상태에서 거리로 무시할 근거가 없으므로,
경로 안에 보이면 일단 멈춘다. 반복 정지는 §4의 래치가 막는다.

**car/person과 표지판을 가르는 게이트가 다른 이유:** car/person은 바닥에
닿는 대상이라 박스 아랫변 위치가 실제 거리와 상관이 있다. 표지판은 세워둔
물건이라 화면 아래쪽에 찍힐 이유가 없어 같은 게이트를 쓸 수 없다.

### 3.2 경로와 거리

카메라에 depth가 없으므로 **박스 크기를 거리 대용값**으로 쓴다. 정확한 거리가
아니라 "가까운가"만 판단한다.

```text
경로 판정 : 박스 중심 x가 [zone_x_min, zone_x_max] 안
            (가장자리에 걸친 박스는 좌표가 0~1을 벗어날 수 있는데,
             중심을 쓰면 그 영향이 절반으로 준다)

car/person: 박스 아랫변 y2 >= zone_y_min 이어야 경로 안으로 본다
            (발·바퀴가 닿는 지점이 실제 위치에 가깝다. 중심을 쓰면
             키 큰 대상이 실제보다 멀리 있는 것처럼 잡힌다)
            높이 >= stop_height  → Stop
            높이 >= slow_height  → Slow

표지판    : 경로 안이면 Stop (높이·아랫변 게이트 없음)
```

한 프레임에서 **가장 위험한 것 하나**가 그 프레임의 상태다. 하나라도 Stop이면
Stop이다.

### 3.3 분류하지 못한 ROI — Clear가 아니다

새 구조에서만 생기는 상황이다. proposal은 "여기 물체가 있다"고 말했는데
class를 얻지 못한 경우다.

| 상황 | 처리 |
| --- | --- |
| `status != OK` (가속기·후처리 오류) | Unclassified |
| TCP 왕복 실패 / 연결 끊김 | Unclassified |
| RTT가 `rtt_budget_ms` 초과 | Unclassified |
| `confidence_ppm < min_confidence_ppm` | Unclassified |

**Unclassified는 car/person과 같은 기하 규칙으로 판단한다.**

```text
경로 안 + 높이 >= stop_height  →  Stop
경로 안 + 높이 >= slow_height  →  Slow
그 밖                          →  Clear
```

근거: 실패한 것은 분류기이지 proposal이 아니다. "물체가 있다"는 정보는 여전히
유효하고, 그 물체가 가깝고 경로 안에 있으면 정체를 몰라도 멈추는 것이 맞다.
반대로 멀거나 경로 밖이면 class를 알았어도 어차피 Clear였다.

> **전부 즉시 Stop으로 두지 않은 이유:** 화면 구석의 작은 오탐 하나로 로봇이
> 멈추면 데모가 성립하지 않는다. 기하 게이트는 car/person에 이미 쓰는 기준이고,
> 안전을 버리지 않으면서 오탐을 거른다.

개별 ROI 실패가 아니라 **연속 실패**는 링크 장애다. §6이 처리한다.

### 3.4 분류 전에 거르기 — 지연을 줄이는 지점

프레임 시간이 `Σ(ROI마다 왕복)`에 비례하므로, **판단을 바꿀 수 없는 ROI를
분류하지 않는 것**이 그대로 지연 감소가 된다.

```text
proposal 최대 10개
  → 경로 zone_x 밖 제외      ← 어떤 class여도 결과는 Clear다. 동작 보존
  → 위험도 내림차순 정렬      ← 아래쪽·큰 박스 먼저
  → 상한 개수까지만 분류 요청
```

정렬을 두는 이유는 상한에 걸려 잘릴 때 **가장 위험한 것이 남게** 하기
위해서다. `RoiProposer`는 objectness 내림차순으로 자르는데, objectness가 높은
것과 위험한 것은 다르다.

> 시각화(MJPEG)에는 걸러진 후보도 박스만 그리고 `class=?`로 표시한다.
> 화면에서 사라지면 "검출이 안 된다"로 오해된다.

---

## 4. 정지 이벤트 — 멈춤·유지·출발·재트리거 억제

판단만으로는 표지판 앞에서 계속 멈춰 있게 된다. 이벤트 층을 따로 둔다.

```text
Idle      위험 없음. Stop급이 잡히면 즉시 Holding
Holding   hold_ms 동안 detection과 무관하게 무조건 Stop
Released  hold_ms 경과 후 진입. 트리거한 class를 제외하고 재판단해
          Clear/Slow로 내려갈 수 있다. 그 class가 release_ms 이상 &&
          release_frames 이상 연속으로 안 보이면 Idle로 복귀
          (다른 class가 새로 Stop급이면 Released 중에도 즉시 새 이벤트)
```

**시간과 프레임 수를 함께 쓰는 이유:** 프레임 조건만 쓰면 프레임률에
끌려다닌다(느린 프레임에서 10프레임은 1초가 넘는다). 시간 조건만 쓰면 판단이
멈춘 사이에 시간이 흘러 저절로 풀린다. 둘 다 만족해야 한다.

**object tracking이 없으므로 class 단위로만 근사한다.** 같은 class의 다른
개체가 래치 기간에 나타나도 무시된다. 데모에서 대상 등장을 기구로 통제하는
것을 전제로 한 트레이드오프다.

---

## 5. 상태 값

UART로 나가는 것은 명령이 아니라 **현재 안전 상태**다.

| 값 | 이름 | RPi arbiter 동작 |
| --- | --- | --- |
| `0x00` | CLEAR | ADAS 제한 해제 가능. **자동 출발 명령이 아니다** |
| `0x01` | SLOW | 수동 입력에 속도 상한 적용 |
| `0x02` | STOP | 수동 입력과 무관하게 즉시 정지 |

`SLOW`의 실제 속도 상한은 **frame에 실리지 않는다.** RPi arbiter의 설정값이다.
Jetson은 "제한하라"는 상태만 전달한다.

---

## 6. Fail-safe

| 상황 | 감지 | 동작 |
| --- | --- | --- |
| 분류 스레드 정지·지연 | 송신 스레드가 `now - decided_at_ms` 검사 | **Stop** |
| 카메라 정지·오류 | `captureFrame()` 실패 | **Stop** |
| Arty 연결 끊김 | `classify()` 연속 실패 ≥ K | **Stop** |
| Arty 응답 지연 | RTT > `rtt_budget_ms` | 해당 ROI만 Unclassified |
| 개별 ROI 오류 | `status != OK` | 해당 ROI만 Unclassified |
| UART 쓰기 실패 | `write()` 실패 | 카운트 + 로그, 다음 주기 재시도 |
| 기동 직후 | 초기 상태 | **Stop** — 첫 정상 판단 전까지 |

**Stop으로 가는 fail-safe는 §4의 래치를 거치지 않는다.** "링크가 끊겼으니
3초 기다렸다 출발"은 말이 안 된다. 래치의 내부 시계는 건드리지 않는다 —
복구되면 경과 시간이 저절로 맞는다.

### 검출 0건 프레임 — heartbeat ROI

**2026-08-20 데모에서 실제로 걸린 문제.** proposal이 하나도 안 나온 프레임은
Arty PS로 요청 자체를 안 보내게 되어 있었는데, 그러면 판단이 그 프레임에서
갱신되지 않는다. 빈 화면(=안전한 상태)이 계속되면 위 표의 "판단 stale
timeout"에 걸려 **Stop이 나가고 바퀴가 멈춘다** — 안전한데 멈추는, 데모가
안 되는 증상이다.

고친 방법: 검출 0건이고 마지막 요청 이후 `heartbeat_interval_ms`(기본
150ms)가 지났으면, **objectness 0·bbox를 경로 밖 구석에 둔 가짜 ROI**를
하나 만들어 그대로 보낸다. `DetectionAdapter`가 `min_objectness`(0.25)
미만을 무조건 `Rejected`로 버리므로 이 ROI는 위험 판단에 절대 끼지 않고,
"이 프레임은 살아 있고 볼 것이 없다"만 전달되어 watchdog을 먹인다. 실제
ROI 요청도 같은 타이머를 갱신하므로 물체가 보이는 동안에는 heartbeat가
나가지 않는다 — 매 프레임 보내면 빈 화면에서 FPS가 반토막난다(29.3 →
12.3 FPS 실측).

heartbeat ROI는 metrics·overlay·CSV에서 전부 제외한다 — 그래야 "검출 0건
프레임"이라는 사실이 수치에 그대로 남는다. `ADAS_EMPTY_FRAME_HEARTBEAT=0`으로
끌 수 있다(순수 성능 측정 때만).

### 시간 상수 세 개가 서로 다른 일을 한다

| 상수 | 값 | 무엇을 막나 | 소유 |
| --- | --- | --- | --- |
| 송신 주기 | 20 ms | RPi의 수신 timeout. 판단이 안 바뀌어도 계속 보낸다 | Jetson |
| 판단 stale timeout | 500 ms | **Jetson 자신이 멈춘 것** | Jetson |
| 수신 timeout | 100 ms | UART 두절 | RPi |

송신 스레드가 링크를 계속 살려 두므로, 판단 stale timeout은 RPi의 100 ms와
경쟁하지 않는다. 이 둘을 같은 값으로 맞추려 하면 안 된다.

---

## 7. 송신

### 7.1 Frame

고정 3 byte. 유효한 frame은 셋뿐이다.

```text
offset 0  magic  0xA5
offset 1  state  0x00 CLEAR / 0x01 SLOW / 0x02 STOP
offset 2  crc8   CRC-8/SMBUS (poly 0x07, init 0x00, xorout 0x00), 대상 0..1

CLEAR  A5 00 59      SLOW  A5 01 5E      STOP  A5 02 57
```

CRC 구현 검증값: ASCII `"123456789"` → `0xF4`. 양측이 이 값을 내는지 먼저
확인하면 파라미터 불일치를 미리 거른다.

`common/UartFrame.cpp`를 그대로 링크한다.

### 7.2 송신 규칙

| 항목 | 값 |
| --- | --- |
| 방향 | Jetson TX → RPi RX 단방향 |
| ACK / 재전송 | 없음 |
| 주기 | 20 ms (50 Hz), 상태 변화와 무관하게 매번 |
| **STOP 진입 시** | **즉시 1회 추가 송신 후 주기 재개** |
| CLEAR/SLOW 진입 | 다음 주기에 반영 |

STOP만 즉시 보내는 이유는 지연이 곧 제동 거리이기 때문이다. CLEAR는 늦게
반영돼도 위험하지 않으므로 주기에 맡긴다. 구현은 condition variable로 송신
스레드를 깨우면 된다.

### 7.3 왜 UART인가 — Ethernet이 아니라

Arty와의 통신에 이미 Ethernet을 쓰고 있으니 RPi도 TCP로 붙이는 게 자연스러워
보인다. 그럼에도 UART를 쓰는 이유는 **분류 경로와 제어 경로를 다른 고장
영역에 두기 위해서다.** 같은 링크에 얹으면 스위치 한 대가 죽었을 때 판단
근거와 전달 수단을 동시에 잃는다. UART는 점대점이라 그 사고를 공유하지 않는다.

3 byte / 20 ms = 1,500 bps. 115200 8N1에서 점유율 1.3%다. 비용이 없다.

### 7.4 배선

Jetson Nano 40핀 헤더의 UART(`/dev/ttyTHS1`).

```text
Jetson pin 8  (UART TXD)  →  RPi pin 10 (GPIO15 / RXD)
Jetson pin 6  (GND)       ─  RPi pin 6   (GND)
RPi pin 8 (TXD)              연결하지 않음 (단방향)
```

- **8·10번 교차.** 양쪽 헤더 핀아웃이 같아 그대로 꽂으면 TX–TX가 되어 통신이
  되지 않는다. 커넥터 모양이 같아 그냥 꽂고 싶어지는 형태라 특히 주의한다.
- **공통 GND 필수.** 신호선만 연결하면 통신이 안 되거나 간헐적으로 깨진다.
- 양쪽 3.3 V — 레벨 시프터 불필요. 배선 전에 실측으로 한 번 더 확인한다.
- **`nvgetty` 비활성화.** Jetson Nano는 기본적으로 `ttyTHS1`에 serial console이
  붙어 있어, 안 끄면 콘솔 출력이 frame에 섞인다.
  `sudo systemctl disable --now nvgetty`
- 초기 브링업에는 **USB–TTL 어댑터**가 편하다. 배선 실수 위험이 낮고
  `/dev/ttyUSB0`으로 잡힌다. 포트 이름만 설정값으로 두면 나중에 교체된다.

---

## 8. 지연 예산

| 구간 | 값 | 출처 |
| --- | ---: | --- |
| 카메라 캡처 | 33 ms | 640×360 @30 fps |
| proposal 추론 | 13.6 ms | Jetson 실측 (중앙값) |
| ROI 분류 (N=2) | ~16 ms | PL 6.57 ms 실측 + 왕복 |
| 판단 | < 0.1 ms | 순수 계산 |
| 송신 대기 | STOP ~0 / 그 외 0~20 ms | §7.2 |
| **합 (STOP 기준)** | **약 63 ms** | RPi 처리 시간 별도 |

TurtleBot3 Burger 0.22 m/s 기준 63 ms에 **약 1.4 cm** 이동한다.
N이 커지면 그대로 늘어난다 — §3.4의 사전 필터가 지연 제어 수단인 이유다.

---

## 9. 모듈 구성

```text
jetson/
  include/control/ · src/control/
    DetectionAdapter        bbox + 분류결과 → DetectionRecord
    SafetyDecider           판단 → 이벤트 래치 → 상태
    SafetyTransmitter       20 ms 스레드, watchdog, STOP 즉시 송신
    UartPort                termios 래퍼 + 테스트용 인터페이스
  tests/
    test_detection_adapter.cpp    좌표 변환, class 매핑, Unclassified 4경로
    test_safety_decider.cpp       zone·높이 경계, 표지판 규칙, 래치 전이
    test_safety_transmitter.cpp   주기, watchdog, STOP 즉시 송신, 프레임 바이트
  tools/jetson_roi_client.cpp     제어 계층 결선 (ADAS_UART_PORT 로 활성화)
```

제어 계층은 **환경변수로 켠다.** `ADAS_UART_PORT` 가 없으면 결선하지 않고
기존과 동일하게 동작한다.

| 환경변수 | 기본 | 뜻 |
| --- | --- | --- |
| `ADAS_UART_PORT` | (없음) | 안전 상태를 내보낼 포트. 지정하면 제어 계층이 켜진다 |
| `ADAS_UART_BAUD` | 115200 | |
| `ADAS_CONTROL_ZONE_FILTER` | 0 | 1이면 §3.4 사전 필터를 켠다 |
| `ADAS_EMPTY_FRAME_HEARTBEAT` | 1(켜짐) | 0이면 위 heartbeat ROI를 끈다 |
| `ADAS_HEARTBEAT_INTERVAL_MS` | 150 | heartbeat 최소 간격. 판단 stale timeout(500ms)보다 넉넉히 짧아야 한다 |

**포트를 지정했는데 열지 못하면 프로세스가 종료된다.** 조용히 분류만 돌면
로봇이 제어 없이 움직이는 상태가 되기 때문이다.

`UartPort`를 인터페이스로 분리하는 이유는 **하드웨어 없이 시간 동작을
테스트하기 위해서다.** 가짜 시계와 가짜 포트를 주면 "판단이 500 ms 멈추면
STOP이 나가는가", "STOP 진입 시 주기를 기다리지 않는가"를 단위 테스트로
검증할 수 있다. 실물 UART로는 확인하기 어려운 것들이다.

```cpp
class SafetyDecider final {
public:
    struct Config {
        AdapterConfig               adapter;
        safety::JudgeConfig         judge;
        safety::HazardLatch::Config latch;
    };

    // 한 프레임 분의 (후보, 분류결과) 쌍. 분류 실패한 것도 버리지 말고
    // status를 채워 그대로 넘긴다(§3.3).
    safety::State decide(const std::vector<RoiObservation>& observations,
                         std::uint64_t now_ms);

    // 카메라·링크 장애처럼 판단 자체가 불가능한 경우. 래치를 거치지 않는다.
    void forceStop(std::uint64_t now_ms);
};
```

---

## 10. 설정값 — 전부 기본값이 있다

**구현을 시작하기 위해 기다려야 하는 측정은 없다.** 아래 값으로 시작하고,
운용하면서 조정한다.

### 10.1 지금 확정 — 정책 결정이라 측정과 무관하다

| 값 | 기본 | 근거 |
| --- | ---: | --- |
| 송신 주기 | 20 ms | 프로토콜. RPi 수신 timeout의 1/5 |
| `hold_ms` | 3,000 ms | 팀 합의값 |
| `release_ms` | 200 ms | 사람이 "지나갔다"고 보는 최소 시간 |
| `release_frames` | 3 | 12 FPS 가정 시 250 ms — `release_ms`와 비슷한 시점에 만족 |
| 연속 실패 K | 3 | 일시적 오류와 링크 단선을 가르는 값. 3회면 ~200 ms |
| `min_objectness` | 0.25 | proposal conf(0.10)보다 높게. 제동 근거는 더 확실한 것만 |

### 10.2 지금 확정 — 이미 아는 수치로 계산된다

| 값 | 기본 | 계산 |
| --- | ---: | --- |
| 판단 stale timeout | 500 ms | 최악 프레임 시간(캡처 33 + proposal 13.6 + ROI 10개×8 ≈ 127 ms)의 약 4배. 크게 잡아도 손해는 "완전 정지를 늦게 발견"뿐이고, 작게 잡으면 정상 동작 중 오작동한다 |
| `rtt_budget_ms` | 50 ms | PL 실측 6.57 ms + PS 전처리·후처리 + 네트워크. 정상값의 6배 이상이면 이상이다 |

### 10.3 운용 로그로 조정 — 기본값으로 시작해도 된다

| 값 | 초기 | 조정 방법 |
| --- | ---: | --- |
| `min_confidence_ppm` | **0 (끔)** | 게이트를 켜 두고 시작하면 잘못된 값 하나로 전부 Unclassified가 되어 과잉 정지처럼 보인다. **먼저 끄고 confidence 분포를 로그로 모은 뒤** 오분류가 몰리는 구간을 보고 정한다 |

### 10.4 실물 캘리브레이션 필요 — §11

| 값 | 임시값 | 왜 |
| --- | ---: | --- |
| `zone_x_min` / `zone_x_max` | 0.25 / 0.75 | 카메라 화각과 로봇 폭에 달렸다 |
| `zone_y_min` | 0.55 | 카메라 장착 높이·각도에 달렸다 |
| `stop_height` | 0.45 | **거리 대용값이라 장착 조건이 바뀌면 의미가 통째로 바뀐다** |
| `slow_height` | 0.25 | 위와 동일 |

---

## 11. 캘리브레이션 — 자와 카메라만 있으면 된다

§10.4의 네 값은 **Jetson 성능이나 FPS와 아무 상관이 없다.** 카메라를 로봇에
장착한 상태에서 화면상 크기를 재는 물리 측정이다. Arty 연결도, 분류도 필요
없다 — `--full-frame` 모드나 MJPEG 화면만으로 할 수 있다.

### 절차

```bash
# 카메라만 띄우고 화면을 본다 (Arty 없이)
./jetson/build/jetson_roi_client /dev/video0 <ARTY_IP> 5000 --full-frame 8080
# 브라우저에서 http://<jetson-ip>:8080/
```

1. **로봇 폭 → `zone_x`**
   로봇 정면에 폭 표시를 두고, 화면에서 그 폭이 차지하는 x 구간을 읽는다.
   여유를 조금 주면 그것이 `zone_x_min`/`zone_x_max`다. 좁히면 옆에 서 있는
   사람 때문에 안 멈추고, 넓히면 지나가는 사람에도 멈춘다.

2. **거리별 박스 높이 → `stop_height` / `slow_height`**
   사람(또는 대상 물체)을 30 cm / 50 cm / 100 cm / 150 cm에 세우고 각각
   화면상 박스 높이를 정규화 값으로 기록한다.

   | 거리 | 박스 높이 (측정) |
   | ---: | ---: |
   | 30 cm | |
   | 50 cm | |
   | 100 cm | |
   | 150 cm | |

   **제동 거리를 정한 뒤 그 거리의 높이를 `stop_height`로 쓴다.**
   0.22 m/s에 반응 지연 ~100 ms면 관성을 빼도 2 cm대이므로, 정지 거리는
   안전 여유로 정하는 값이다(예: 50 cm). `slow_height`는 그 두 배 거리쯤.

3. **`zone_y_min`**
   대상을 `stop_height` 거리에 두었을 때 박스 **아랫변**의 y 값보다 조금
   작게 잡는다. 그보다 위에 찍히는 것은 더 멀리 있는 것이다.

4. **검증** — 대상을 경로 안팎으로 옮기며 상태가 의도대로 바뀌는지 본다.
   화면에 zone 경계선과 `stop_height` 기준선을 오버레이로 그려 두면 훨씬
   빠르다. `MjpegStreamServer`에 선 몇 개 그리는 것으로 충분하다.

> 이 표를 채우는 것이 제어 로직 작업의 **첫 번째** 할 일이다. 나머지 값은
> 전부 기본값으로 돌려놓고 시작해도 된다.

---

## 12. 다음 단계

1. ~~`common/`의 클래스 상수를 설정으로 뺀다~~ → 완료 (부록 A).
2. ~~`DetectionAdapter` + `SafetyDecider` 구현·단위 테스트~~ → 완료.
3. ~~`SafetyTransmitter` 구현~~ → 완료. 가짜 시계·가짜 포트로 주기·watchdog·
   즉시 송신·프레임 바이트를 검증했다.
4. **배선 후 RPi와 확인** — `A5 00 59` / `A5 01 5E` / `A5 02 57` 세 값이 그대로
   보이는지부터. `nvgetty` 를 껐는지 먼저 확인한다.
5. **§11 캘리브레이션.** zone·height 임계값은 아직 KR260 값 그대로다.
6. 실주행 시험 — **STOP이 안 뜨는 것**보다 **CLEAR가 잘못 뜨는 것**을 먼저 본다.

---

## 부록 A. `common/`에서 가져온 것과 바꿔야 하는 것

`common/`은 이전 KR260 프로젝트에서 복사한 안전 계층이다. 힙·STL·예외를 쓰지
않는 제한된 C++17이라 그대로 쓸 수 있다.

| 파일 | 이 설계에서 |
| --- | --- |
| `SafetyJudge` | §3의 판단. **클래스 상수 수정 필요 (아래)** |
| `SafetyHazardLatch` | §4의 이벤트 래치. `release_ms` 추가 필요 |
| `UartFrame` | §7의 3-byte frame. 그대로 |
| `SafetyDebounce` | 쓰지 않는다. 래치가 그 역할을 한다 |
| `SafetyMessage` | 쓰지 않는다. 코어 간 공유 메모리용이라 필요 없다 |

### ⚠️ 클래스 ID가 한 칸 밀렸다

| class | `common/SafetyJudge.hpp` | 이 프로젝트 계약 |
| --- | ---: | ---: |
| background | (없음) | **0** |
| car | 0 | **1** |
| person | 1 | **2** |
| sign_warning | 2 | **3** |
| sign_prohibition | 3 | **4** |
| sign_mandatory | 4 | **5** |

그대로 링크하면 **car를 person으로, background를 car로 판단한다.** 오류 없이
동작하고 결과만 조용히 틀리는 종류다 — `arty/models/README.md`가 경고하는
"가중치 교환 불가"와 같은 성질의 함정이다.

**상수를 고치지 말고 설정으로 뺀다 (적용 완료).** `common/ORIGIN`이 "여기서 고친 것을
KR260 저장소로 되돌릴 수 있게" 제약 유지를 요구하므로, 프로젝트마다 다른
값을 소스에 박으면 그 경로가 막힌다.

```cpp
struct ClassMap {
    int32_t car              = 0;
    int32_t person           = 1;
    int32_t sign_warning     = 2;
    int32_t sign_prohibition = 3;
    int32_t sign_mandatory   = 4;
    int32_t background       = -1;   // 없으면 -1
};
// JudgeConfig에 ClassMap classes; 를 추가하고 isHazardClass/isSignClass가
// 이것을 참조한다. 기본값이 기존 값이라 KR260 동작은 바뀌지 않는다.
```

이 저장소에서는 `{1, 2, 3, 4, 5, 0}`으로 채운다. `common/` 변경은 다른 변경과
섞지 말고 별도 커밋으로 남긴다(ORIGIN 규칙).

## 부록 B. RPi 쪽은 바뀌는 것이 없다

전송하는 값과 의미가 같으므로 기존 arbiter(`STOP > SLOW > CLEAR + manual`)를
그대로 쓴다. **송신 주체가 무엇인지 RPi는 알 필요가 없다.**

- `STOP` — 수동 입력과 무관하게 zero `Twist`를 주기적으로 계속 출력
- `SLOW` — 수동 명령에 속도 상한 적용
- `CLEAR` — 재개 조건(deadman 해제, stick neutral) 확인 후 수동 입력 통과
- 수신 timeout·CRC 오류·알 수 없는 state 처리 규칙도 그대로
