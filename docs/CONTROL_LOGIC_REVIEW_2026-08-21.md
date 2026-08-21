# 제어 로직 검토와 수정 — 저신뢰 분류 처리

검토·수정 2026-08-21 15:0x KST · 검토자 Claude
· 대상 `arty/ps_db/src/control/ps_safety_bridge.cpp`, `common/`
· 비교 기준 `9d2e13c` → `ab22bf2`

---

## 0. 요약

`ab22bf2` 까지의 변경 중 **저신뢰 분류를 다루는 방식**에 문제가 있어 고쳤다.
표지판 Stop→Slow 강등, 젯슨 판단 계층 삭제 등 나머지 변경은 타당해 그대로 뒀다.

| | 변경 전 (`ab22bf2`) | 변경 후 |
|---|---|---|
| 통신·가속기 오류 | `person` 으로 판단 → **Stop 가능** | 동일 |
| 분류됨 + confidence < 60% | 레코드 폐기, **최대 Slow** | **`person` 으로 판단 → Stop 가능** |
| `pending_uncertain_slow` | 존재 | **제거** |

---

## 1. 무엇이 문제였나

### 1.1 반응이 역전돼 있었다

```cpp
// 변경 전
if (classification_ok && !usable_class) {   // 분류됨 + 저신뢰
    ... pending_uncertain_slow = true;      // 최대 Slow
    return;                                 // ← 레코드를 버린다
}
if (!usable_class) {                        // 통신·가속기 오류
    record.class_id = ...person;            // Stop 가능
}
```

| 상황 | 가진 정보 | 최대 반응 |
|---|---|---|
| 통신 실패 | class 정보 **없음** | **Stop** |
| 저신뢰 분류 | class 정보 **있음(약함)** | Slow |

**정보가 더 많은 쪽이 더 약하게 반응한다.** 순서가 뒤집혀 있었다.

게다가 저신뢰 경로는 class 를 아예 보지 않는다 — 55 % 확신의 `person` 이든
55 % 확신의 `car` 든 똑같이 Slow 였다.

### 1.2 래치의 "사라짐" 판정이 오염됐다

`return` 때문에 저신뢰 레코드는 `pending[]` 에 들어가지 않는다. 그런데
`HazardLatch` 의 해제 조건은 그 배열을 보는 `classPresent()` 다.

```
차는 그대로 있는데 confidence 만 60 % 아래로 흔들림
  → pending[] 에 없음 → classPresent() = false
  → 래치가 "사라졌다"로 카운트 (release_frames = 3)
  → 20 FPS 기준 150 ms 만에 래치 해제
  → 다시 60 % 위로 올라오면 새 이벤트 → Stop + hold 3000 ms
```

같은 대상에 대해 **정지–출발이 반복**된다. 래치가 막으려던 바로 그 현상이다.

### 1.3 근본 원인 — confidence 를 "중요도"로 쓴 것

**분류 신뢰도는 "무엇인가"에 대한 확신이지 "있는가"에 대한 확신이 아니다.**

의도는 "중요하지 않은 것이 자꾸 잡히는 것"을 줄이는 것이었는데, 그 목적에는
이미 세 관문이 있다.

| 걸러내려는 것 | 담당 |
|---|---|
| 물체가 아닌 것(노이즈·질감) | `min_objectness`(0.25), `background` 클래스 |
| 멀거나 작은 것 | `slow_height` / `stop_height` |
| 경로 밖 | `zone_x_min` / `zone_x_max` |

`confidence` 는 이 중 어디에도 해당하지 않는다. 55 % 확신의 사람은
"중요하지 않은 것"이 아니라 **"차인지 사람인지 모르겠는 것"** 이다.

---

## 2. 어떻게 고쳤나

`arty/ps_db/src/control/ps_safety_bridge.cpp`

**조기 `return` 을 없애고, 저신뢰를 통신 실패와 같은 경로로 보낸다.**

```cpp
const bool usable_class = result->status == ADAS_ROI_STATUS_OK
    && result->confidence_ppm >= handle->min_confidence_ppm;

if (!usable_class) {
    // (a) 통신·가속기 오류  (b) 분류됨 + 저신뢰  ->  둘 다 person 규칙
    record.class_id = handle->judge_config.classes.person;
} else {
    ... background 제외 후 실제 class ...
}

handle->pending[handle->pending_count] = record;   // 두 경우 모두 들어간다
```

`pending_uncertain_slow` 필드와 `flush` 의 후처리는 필요 없어져 **제거**했다.

### 2.1 이렇게 해도 노이즈 억제는 유지된다

저신뢰 레코드가 `pending[]` 에 들어가도 `judgeOne()` 이 뒤에서 거른다.

```
zone_x 밖            -> Clear
y2 < zone_y_min      -> Clear   (바닥에 안 닿음)
height < slow_height -> Clear   (작거나 멀다)
```

**작고 먼 저신뢰 검출은 그대로 Clear 다.** 60 % 게이트를 켜기 전에도 그랬다.
달라지는 것은 **경로 안의 크고 가까운 물체**뿐이고, 그건 원래 반응해야 할
대상이다.

### 2.2 부수 효과

- **1.2 의 래치 오판이 사라진다.** 레코드가 `pending[]` 에 있으므로
  `classPresent()` 가 정상 동작한다.
- **`min_confidence_ppm`(60 %) 자체는 그대로 둔다.** class 를 신뢰할지 말지의
  기준으로는 여전히 쓰인다. 다만 그것이 "무시" 가 아니라 "person 으로 보수적
  판단" 을 뜻하게 됐다.

---

## 3. 같이 고친 주석 (사실과 달랐던 것)

`common/include/common/SafetyJudge.hpp`

| 틀린 서술 | 실제 |
|---|---|
| "jetson_roi_client overrides it via `ADAS_SIGN_SLOW_HEIGHT`" | **Arty PS** 가 읽는다 (`ps_safety_bridge.cpp:113`). 젯슨 판단 계층은 `ab22bf2` 에서 삭제됐다 |
| "Lower than the old Stop gate (0.62)" | 이전 코드에 **표지판 높이 게이트가 없었다** (`return State::Stop` 무조건). 0.62 라는 값은 이 파일에 존재한 적이 없다 |

근거로 제시된 문장이라 그대로 두면 다음 사람이 잘못된 전제로 튜닝하게 된다.
`0.62` 언급은 삭제하고, 덮어쓰는 주체를 PS 로 정정했다.

---

## 4. 타당하다고 판단해 그대로 둔 변경

### 4.1 표지판 Stop → Slow 강등 ✅

분류기가 개별 표지를 구분하지 못하는데 전부 Stop 으로 대응하면 틀릴 때가 더
많고, 틀렸을 때 비용도 정지 쪽이 크다. 근거가 타당하다.

**면적 대신 높이를 쓴 이유도 정확하다.** 16:9 에서 면적 50 % 는 한 변
`sqrt(0.5 × 640 × 360) = 339 px`, 높이의 94 % 라 사실상 도달 불가다.
높이 비율은 종횡비에 영향받지 않는다.

### 4.2 젯슨 판단 계층 삭제 ✅

`DetectionAdapter`·`SafetyDecider`·`SafetyTransmitter`·`UartPort` 가 젯슨에서
빠지고 PS 로 옮겨갔다. 판단이 한 곳에만 있어야 두 구현이 갈라지지 않는다.

### 4.3 표지판이 래치를 열지 않는다는 서술 ✅

`judgeOne` 이 표지판을 Slow 로 막고 래치는 Stop 에서만 열리므로 맞다.
수정 후에도 유효하다 — 저신뢰 레코드는 `person` 이 되므로 래치를 여는 것은
여전히 car/person 뿐이다.

---

## 5. 검증

| | 결과 |
|---|---|
| `g++ -fsyntax-only` `ps_safety_bridge.cpp` | OK |
| `g++ -fsyntax-only` `SafetyJudge.cpp` | OK |
| `g++ -fsyntax-only` `SafetyHazardLatch.cpp` | OK |
| `pending_uncertain_slow` 잔여 참조 | 0건 |
| 틀린 주석 잔여 | 0건 |

**실보드 검증은 하지 않았다.** 크로스 컴파일·재배포 후 확인이 필요하다.

---

## 6. 남은 판단 — `min_confidence_ppm = 60 %` 가 맞는 값인가

이 값은 그대로 뒀지만, 근거는 확인해 두는 게 좋다.

**측정된 confidence 분포** (2026-08-21, 87,944건):

| | 값 |
|---|---|
| 60 % 미만 | **18,566건 (21.1 %)** |
| p5 | 48.7 % |
| p10 | 51.7 % |
| p25 | **64.0 %** |
| p50 | 92.8 % |
| p75 | 98.9 % |

60 % 는 하위 4분의 1 언저리를 자른다. 검출 5건 중 1건이 이 게이트에 걸린다.

**아직 모르는 것:** 그 21 % 가 실제로 "경로 안 + 큰 박스" 인지는 재지 않았다.
대부분이 화면 구석의 작은 오탐이라면 이번 수정의 영향은 미미하고, 가까운
물체가 자주 60 % 아래로 떨어진다면 크다. `ADAS_PS_CSV` 에 confidence 와
bbox 가 함께 남으므로 결합 분포를 보면 알 수 있다.

> 삭제된 `DetectionAdapter` 에 이런 경고가 있었다 — 지금도 유효하다.
>
> "처음부터 켜지 않는 것을 권한다: 값을 잘못 잡으면 전부 Unclassified 가
> 되어 **과잉 정지처럼 보이는데, 원인이 게이트라는 것이 밖에서 안 보인다.**
> 먼저 끄고 confidence 분포를 로그로 모은 뒤 정한다."

노이즈 억제가 목적이라면 `min_objectness`(현재 0.25)를 올리는 쪽이 더
직접적이다. 그건 "물체인가" 를 직접 거른다.

---

## 7. 변경 파일

| 파일 | 내용 |
|---|---|
| `arty/ps_db/src/control/ps_safety_bridge.cpp` | 저신뢰 조기 return 제거, `pending_uncertain_slow` 제거, 근거 주석 |
| `common/include/common/SafetyJudge.hpp` | 틀린 주석 2건 정정 |

---

## 8. 추가 검토 의견 — 저신뢰를 모두 person으로 바꾸는 정책의 한계

위 검토에서 지적한 **저신뢰 레코드 조기 폐기 문제**와
`pending_uncertain_slow` 제거 방향에는 동의한다. 다만 수정안인
“confidence 60% 미만을 모두 person으로 취급”도 완전한 해결은 아니다.

### 8.1 car의 래치 연속성이 여전히 깨질 수 있다

같은 자동차의 confidence만 임계값 위아래로 흔들리는 경우를 생각할 수 있다.

```text
car 70% → class_id=car
car 55% → class_id=person으로 강제 변경
```

첫 프레임에서 `car`로 열린 래치를 두 번째 프레임의 `person` 레코드가 유지하지
못한다. `classPresent(car)`는 false가 되므로, 문서 §1.2에서 지적한 “대상이
사라졌다고 세는 문제”가 car에 대해서는 남는다. 저신뢰 레코드를 배열에 넣는
것만으로는 충분하지 않고, **성공한 분류의 class_id를 유지해야** 클래스
연속성도 유지된다.

### 8.2 표지판은 confidence가 낮을수록 반응이 강해진다

현재 표지판 정책은 확정된 sign에 대해 최대 `SLOW`까지만 허용한다. 그런데
저신뢰 sign을 person으로 바꾸면 다음과 같은 역전이 생긴다.

```text
표지판 70% → sign   → 최대 SLOW
표지판 55% → person → STOP 가능
```

정보가 불확실할 때 더 보수적으로 반응한다는 해석은 가능하지만, 같은 표지판의
confidence가 흔들릴 때 `SLOW/STOP`이 바뀌어 데모 주행이 불안정해질 수 있다.
또한 “표지판은 개별 의미를 구분하지 못하므로 Stop을 만들지 않는다”는 §4.1의
정책과도 결과적으로 충돌한다.

### 8.3 권장 정책

분류 성공과 분류 실패를 분리해서 처리하는 편이 가장 일관적이다.

```text
분류 성공(status == OK):
    confidence와 무관하게 FPGA argmax class_id 사용

분류 실패(status != OK):
    class를 얻지 못했으므로 person 규칙으로 보수 처리

Jetson 화면 표시:
    confidence 60% 미만 bbox/label은 그리지 않음
```

현재 구현에서는 다음 설정으로 이 정책에 가까워진다.

```bash
# Arty 제어: 성공한 분류 class를 confidence로 버리지 않는다.
ADAS_MIN_CLASS_CONFIDENCE_PPM=0

# Jetson 시각화: 낮은 confidence는 화면만 숨긴다.
ADAS_OVERLAY_MIN_CONFIDENCE_PPM=600000
```

이렇게 하면 저신뢰 car는 계속 car로, 저신뢰 sign은 계속 sign으로 남는다.
따라서 클래스 기반 래치 연속성이 유지되고, 표시 정책이 제어 정책을 약화하거나
변형하지 않는다. 실제 통신·가속기 실패만 class 미확정으로 처리해 person
규칙을 적용한다.

### 8.4 objectness 조정 위치

노이즈 억제에 class confidence보다 objectness가 직접적이라는 §6의 결론에는
동의한다. 다만 현재 Arty의 `min_objectness=0.25`는 ROI가 TCP로 전달되고 FPGA
분류까지 수행된 뒤 안전 판단 단계에서 검사된다. 따라서 이 값만 올려서는
Jetson→Arty 전송량이나 FPGA 연산량이 줄지 않는다.

불필요한 ROI 생성과 연산 자체를 줄이려면 Jetson의 proposal 단계에 있는
`RoiProposer::confidence_threshold`를 조정해야 한다. 현재 기본값은 `0.10`이며,
다음 순서로 실물 장면을 확인하는 것이 적절하다.

```text
0.10 → 0.20 → 0.25 → 필요 시 0.30
```

먼 거리 사람이나 작은 표지판을 놓치지 않는 범위에서 결정해야 하므로 처음부터
`0.5`처럼 크게 올리지는 않는다. Arty의 `min_objectness`는 최종 방어용 검사로
같거나 더 낮게 유지할 수 있다.

### 8.5 최종 권장 조합

| 목적 | 적용 위치 | 권장 시작값 |
|---|---|---:|
| 불필요한 ROI 억제 | Jetson proposal objectness | `0.20~0.25` |
| 화면 정리 | Jetson overlay confidence | `60%` |
| 성공한 분류의 제어 반영 | Arty class confidence | `0%` |
| 실제 분류 실패의 fail-safe | Arty PS | person 규칙 유지 |

이 절의 내용은 **추가 검토 의견이며 아직 코드와 시작 스크립트에 반영하지
않았다.** 변경 전 실제 데모 객체에 대해 proposal 누락률과 `SLOW/STOP` 전이를
확인해야 한다.

---

## 9. §8 에 대한 회신 — 지적이 맞다, 2절의 수정은 절반짜리였다

작성 2026-08-21 15:2x KST · 작성자 Claude (§1~§7 작성자)

§8 의 사실관계를 코드에서 다시 확인했다. **두 지적 모두 맞고, §2 의 수정은
문제의 절반만 고친 것이었다.**

| §8 확인 항목 | 코드 |
|---|---|
| `RoiProposer::confidence_threshold` 기본값 | `jetson/include/roi/RoiProposer.hpp:19` = **0.10F** ✅ |
| overlay 게이트가 표시 전용인가 | `jetson_roi_client.cpp:289` → `mjpeg_config.overlay_min_confidence_ppm` ✅ |
| PS `min_objectness` 가 판단 단계에 있는가 | `ps_safety_bridge.cpp` 안, TCP 수신·PL 실행 이후 ✅ |

### 9.1 §8.1 — 맞다. 클래스 정체성을 되살리지 못했다

§2 의 수정은 **"레코드를 버리는 것"** 만 고쳤고 **"클래스를 잃는 것"** 은
그대로 뒀다. `class_id = person` 을 강제하므로

```text
car 70% → class_id = car
car 55% → class_id = person   ← classPresent(car) = false
```

§1.2 에서 지적한 래치 오판이 car 에 대해 **그대로 남는다.** 레코드를 배열에
넣는 것만으로는 부족하고 class_id 를 보존해야 한다는 §8.1 의 결론이 옳다.

### 9.2 §8.2 — 맞다. 표지판에 새 역전을 만들었다

이건 §2 를 쓸 때 놓쳤다.

```text
표지판 70% → sign   → 최대 SLOW
표지판 55% → person → STOP 가능
```

§1.1 에서 "정보가 많은 쪽이 약하게 반응하는 역전" 을 문제로 지적해 놓고,
§2 의 수정으로 **같은 종류의 역전을 표지판에 새로 만들었다.** §4.1 에서
"표지판은 Stop 을 만들지 않는다" 는 정책이 타당하다고 판단해 놓고 그 정책에
우회로를 뚫은 셈이다. 자체 모순이다.

### 9.3 §8.3 정책에 동의한다

§1.3 의 논증("confidence 는 무엇인가에 대한 확신이지 있는가에 대한 확신이
아니다")을 끝까지 밀면 **분류 성공/실패로만 갈라야 한다**는 §8.3 이 나온다.
§2 는 그 중간에서 멈춘 것이다.

- 성공한 분류의 argmax class 를 confidence 로 버리지 않는다 → **클래스 연속성
  유지**, 래치 정상 동작
- 표지판은 계속 sign 이므로 **§4.1 정책이 우회되지 않는다**
- 표시(overlay)와 제어(judge)를 분리 → 표시 정책이 제어를 변형하지 않는다

### 9.4 §8.4 — §6 의 서술이 부정확했다

§6 에서 "노이즈 억제가 목적이라면 `min_objectness` 를 올리는 쪽이 더
직접적" 이라고 썼는데, **"노이즈"** 로 두 가지를 뭉뚱그렸다.

| 줄이려는 것 | 올바른 위치 |
|---|---|
| 잘못된 안전 판단 | PS `min_objectness` (판단 단계) |
| **불필요한 전송·PL 연산** | **Jetson `RoiProposer::confidence_threshold`** (제안 단계) |

PS 의 `min_objectness` 는 ROI 가 이미 TCP 로 오고 PL 이 6.6 ms 를 쓴 **뒤에**
검사되므로, 그 값만 올려서는 부하가 전혀 줄지 않는다. §8.4 의 지적이 맞다.

### 9.5 남는 위험 하나 — 저신뢰 argmax 가 background 인 경우

§8.3 으로 가면 confidence 를 무시하고 argmax 를 쓰는데, 현재 코드는 argmax 가
`background` 면 레코드를 `return` 으로 버린다.

```text
바로 앞 사람, confidence 35%, argmax = background  →  폐기  →  Clear
```

§2 의 수정에서는 person 으로 강제돼 Stop 이 가능했던 경우다. 빈도는 낮겠지만
**가장 비싼 실패 방향**이므로, 감수하는 결정인지 명시해 두는 편이 좋다.

이 구멍을 막을 곳은 confidence 가 아니라 objectness 다 — proposal 이
"물체가 있다" 고 했는데 분류기가 "background" 라고 하는 불일치 상황이므로,
§8.4 의 proposal 임계값 조정과 같은 축에서 다뤄야 한다.

### 9.6 정리 — §2 와 §8.3 의 차이

| | §2 (현재 코드) | §8.3 (권장) |
|---|---|---|
| 저신뢰 car | person 으로 변경 → 래치 끊김 | **car 유지** → 래치 연속 |
| 저신뢰 sign | person → **Stop 가능(정책 위반)** | **sign 유지** → 최대 Slow |
| 저신뢰 background | person 강제 → Stop 가능 | **폐기** (§9.5 위험) |
| 통신·가속기 실패 | person 규칙 | 동일 |
| 부하 감소 | 없음 | proposal 임계값으로 별도 처리 |

`background` 항목만 §2 가 보수적이고, 나머지 전부 §8.3 이 낫다.

### 9.7 상태

**아직 코드에 반영하지 않았다.** 현재 저장소 작업본은 §2 상태
(`class_id = person` 강제)다. §8.3 으로 옮기려면 다음이 필요하다.

1. `ps_safety_bridge.cpp` — `usable_class` 에서 confidence 조건 제거,
   `status == OK` 면 argmax class 사용
2. `min_confidence_ppm` 기본값 `600000` → **0** (환경변수로 켤 수 있게 유지)
3. 기동 스크립트에 `ADAS_OVERLAY_MIN_CONFIDENCE_PPM=600000` 명시
4. §2 를 갱신하고 이 절(§9)을 근거로 남김

§8.4 의 proposal 임계값(0.10 → 0.20~0.25)은 **실물 장면에서 누락률을 봐야**
하므로 코드에는 넣지 않고 판단 대기로 둔다.

---

## 10. 최종 의견 — background 위험 후보는 SLOW로 처리

§9.5의 선택지는 다음과 같이 확정하는 것이 타당하다.

> proposal이 물체 후보를 만들었지만 FPGA 분류 결과가 background인 경우,
> 무조건 폐기하지 않는다. 주행 경로 안의 크고 가까운 후보라면 class 미확정
> 장애물로 보고 `SLOW`까지만 출력한다.

이 정책은 실제 객체가 background로 오분류돼 즉시 `CLEAR`가 되는 위험을 줄이되,
background 하나 때문에 `STOP` 래치를 여는 과잉 반응은 피한다.

### 10.1 최종 분기표

| 분류 결과 | 처리 |
|---|---|
| `status != OK` | class 자체를 얻지 못한 오류이므로 person 규칙, `STOP` 가능 |
| `status == OK`, foreground argmax | confidence와 무관하게 argmax class 규칙 사용 |
| `status == OK`, background argmax, 위험 geometry 충족 | class 미확정 장애물로 `SLOW` |
| `status == OK`, background argmax, 위험 geometry 미충족 | `CLEAR` |

background의 위험 geometry는 다음 조건을 모두 만족하는 경우로 시작한다.

```text
bbox center_x ∈ [zone_x_min, zone_x_max]
bbox y2       >= zone_y_min
bbox height   >= slow_height
objectness    >= min_objectness
```

`zone_y_min`을 포함하는 이유는 화면 위쪽의 손·배경 조각처럼 바닥 주행 경로와
무관한 큰 오탐이 `SLOW`를 만드는 것을 줄이기 위해서다. 이 조건에서도 실제
표지판 누락이 관찰되면 background fallback만 별도 조정할 수 있지만, 우선은
자동차·사람과 같은 ground-plane 조건을 사용하는 편이 보수성과 데모 안정성의
균형이 낫다.

### 10.2 구현 요청

1. `usable_class`에서 confidence 조건을 제거한다.

   ```cpp
   const bool classification_ok = result->status == ADAS_ROI_STATUS_OK;
   ```

2. 분류 성공이면 confidence와 무관하게 `result->class_id`를 사용한다.
3. 분류 실패만 person 규칙으로 보낸다.
4. background 성공 결과는 위 geometry를 검사해 `SLOW` 또는 `CLEAR`로 나눈다.
5. background fallback은 `STOP`을 만들거나 `HazardLatch`를 열지 않아야 한다.
6. 제어용 `ADAS_MIN_CLASS_CONFIDENCE_PPM`은 제거한다. 화면 표시용 60%와
   혼동할 수 있는 죽은 설정을 남기지 않는다.
7. 시작 스크립트에서 `ADAS_MIN_CLASS_CONFIDENCE_PPM`을 제거한다.
8. Jetson의 `ADAS_OVERLAY_MIN_CONFIDENCE_PPM=600000`은 이미 적용돼 있으므로
   유지한다.

background fallback 구현은 `pending_background_slow` 같은 프레임 단위 flag로
둘 수 있다. `flush_frame()`에서 일반 판단 결과가 `CLEAR`일 때만 `SLOW`로
올리면 기존 `STOP`과 알려진 class의 판단을 덮어쓰지 않는다.

### 10.3 proposal objectness

Jetson proposal threshold는 즉시 `0.25`로 고정하지 않는다. 런타임 환경변수로
조절할 수 있게 만든 뒤 다음 값을 같은 데모 장면에서 비교한다.

```text
0.10 / 0.20 / 0.25
```

각 값에서 최소한 다음을 기록한다.

- 실제 사람·자동차·표지판 proposal 누락 수
- 프레임당 ROI 개수
- Jetson→Arty ROI/s
- 불필요한 background 분류 수
- `CLEAR/SLOW/STOP` 전이

누락이 증가하지 않는 가장 높은 값을 선택한다. Arty의 `min_objectness`는 최종
방어선으로 유지하되, 불필요한 TCP 전송과 PL 연산을 줄이는 주체는 Jetson
proposal 단계임을 문서와 코드에서 구분한다.

### 10.4 검증해야 할 시나리오

| 입력 | 기대 상태 |
|---|---|
| car confidence가 60% 위아래로 변동 | class는 계속 car, 래치 연속성 유지 |
| sign confidence가 60% 아래 | class는 계속 sign, 최대 `SLOW` |
| 작은 background 또는 경로 밖 background | `CLEAR` |
| 경로 안의 크고 가까운 background | `SLOW`, `STOP` 래치 없음 |
| 분류 통신·가속기 실패 | person 규칙으로 fail-safe |
| 화면의 60% 미만 결과 | 제어에는 반영되지만 MJPEG overlay에는 표시 안 됨 |

이 절은 §8과 §9의 논의를 반영한 **최종 수정 요청**이다. 코드 반영 후 로컬
단위 테스트와 실보드 상태 전이 확인 결과를 이 문서에 이어서 기록한다.

---

## 11. §10 구현 결과

구현 2026-08-21 15:4x KST · 구현자 Claude

§10.2 의 요청 1~8 을 모두 반영했다. **실보드 검증은 아직이다.**

### 11.1 `arty/ps_db/src/control/ps_safety_bridge.cpp`

| §10.2 항목 | 반영 |
|---|---|
| 1. `usable_class` 에서 confidence 제거 | `const bool classification_ok = result->status == ADAS_ROI_STATUS_OK;` |
| 2. 성공이면 confidence 무관하게 `result->class_id` | ✅ |
| 3. 실패만 person 규칙 | ✅ (`if (!classification_ok)`) |
| 4. background 는 geometry 검사로 SLOW/CLEAR | ✅ (아래) |
| 5. background fallback 이 STOP·래치를 만들지 않음 | ✅ `flush` 에서 `state == Clear` 일 때만 승격 |
| 6. 제어용 `ADAS_MIN_CLASS_CONFIDENCE_PPM` 제거 | ✅ 상수·파서·필드·기동 로그까지 전부 |

background 위험 기하 판정은 §10.1 그대로다.

```cpp
if (record.score >= handle->min_objectness
    && center_x >= handle->judge_config.zone_x_min
    && center_x <= handle->judge_config.zone_x_max
    && record.y2 >= handle->judge_config.zone_y_min
    && height >= handle->judge_config.slow_height) {
    handle->pending_background_slow = true;
}
return;
```

`pending_uncertain_slow` → `pending_background_slow` 로 교체했고,
`flush_frame()` 과 `force_stop()` 양쪽에서 초기화한다.

기동 로그 `safety judge:` 한 줄에서 `min_class_confidence_ppm` 항목이 빠졌다.
형식 문자열과 인자를 같이 고쳤다 — 한쪽만 고치면 인자 없는 `%u` 가 남는다.

### 11.2 `jetson/tools/jetson_roi_client.cpp` (§10.3)

proposal 임계값을 런타임 환경변수로 뺐다. **기본값은 `0.10` 그대로다.**

```cpp
proposer_config.confidence_threshold = env_ratio(
    "ADAS_PROPOSAL_CONFIDENCE", proposer_config.confidence_threshold);
```

`env_ratio()` 헬퍼를 새로 넣었다 — 0.0~1.0 범위를 벗어나거나 형식이 틀리면
경고를 찍고 기본값을 쓴다. 조용히 이상한 값이 들어가는 것을 막는다.

파일 상단 환경변수 목록에도 추가했다.

### 11.3 스크립트·문서에서 제거 (§10.2 의 7)

| 파일 | 처리 |
|---|---|
| `turtlebot/scripts/arty_start.sh` | 환경변수 제거 (`sh -n` 통과) |
| `README.md` | 실행 예시에서 제거 + 산문을 "표시 전용" 으로 정정 |
| `arty/ps_db/README.md` | 실행 예시 줄 삭제 + 임계값 설명을 새 정책으로 교체 |
| `docs/contracts/ROI_CLASSIFIER_CONTRACT.md` | 계약 항목 3줄 교체 |
| `turtlebot/docs/SERVER_START_STOP.md` | 실행 예시·환경변수 표 행 제거 |

저장소 전체에서 `ADAS_MIN_CLASS_CONFIDENCE_PPM` 잔여 **0건**
(이 문서의 §8~§10 논의 인용 제외).

`ADAS_OVERLAY_MIN_CONFIDENCE_PPM=600000` 은 §10.2 의 8 대로 유지했다.

### 11.4 검증

| | 결과 |
|---|---|
| `g++ -fsyntax-only -Wall -Wextra -Wformat=2` `ps_safety_bridge.cpp` | **OK, 경고 없음** |
| `env_ratio()` 독립 컴파일 + 동작 | OK. `1.5` 입력 시 경고 후 기본값 사용 확인 |
| `sh -n turtlebot/scripts/arty_start.sh` | OK |
| `min_confidence_ppm` / `kDefaultMinConfidencePpm` 잔여 | 0건 |
| `jetson_roi_client.cpp` 전체 문법 | **미확인** — adas-pc 에 OpenCV/TensorRT 가 없다. 젯슨에서 빌드해야 한다 |

`-Wformat=2` 를 넣은 이유: 편집 중에 형식 문자열의 `%u` 만 남고 인자가 빠진
상태가 실제로 한 번 나왔다. 그 종류는 문법 검사만으로는 안 잡힌다.

### 11.5 아직 안 한 것

- **크로스 컴파일·실보드 재배포.** 지금 보드 바이너리는 이 변경 이전 것이다.
- **§10.4 시나리오 검증.** 재배포 후 6가지를 확인해 이 문서에 이어 기록한다.
- **proposal 임계값 확정.** 환경변수만 만들었고 값은 `0.10` 그대로다.
  같은 장면에서 `0.10 / 0.20 / 0.25` 를 비교해 §10.3 의 5개 항목을 기록한
  뒤 정한다.

### 11.6 재배포 후 바로 볼 것

기동 로그 한 줄로 새 정책이 실렸는지 확인할 수 있다.

```text
safety judge: sign_slow_height=... stop_height=... slow_height=...
              zone_x=[...] zone_y_min=... min_score=...
```

**`min_class_confidence_ppm` 항목이 없으면** 새 바이너리다. 남아 있으면 옛
바이너리가 돌고 있는 것이다.
