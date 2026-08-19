# 변경 파일 인계 목록

이 문서만 보고 변경분을 받은 쪽에서 제자리에 놓을 수 있게 정리했다.

- **신규 19 · 수정 45 · 이동 44 = 108 파일**
- 모든 경로는 **저장소 루트(`adas_project_sub/`) 기준**이다.
- 작업 묶음 4개: ① FPS·지연 계측 ② ps_eb 완성 ③ 제어 로직 ④ PL 디렉터리 통합

> **먼저 읽을 것**
> - §6 **동작이 바뀐 것 2건** — 받는 쪽이 모르면 곤란한 변경이다.
> - §5 **한 세트로 보내야 하는 파일** — 따로 보내면 컴파일은 되고 동작만 틀린다.

---

## 1. 계측 — FPS·지연 (작업 ①)

프레임률과 구간별 지연을 수치로 남기기 위한 계측. Jetson·PS 양쪽.

| 경로 | 구분 | 내용 |
| --- | --- | --- |
| `jetson/include/metrics/LatencyStats.hpp` | **신규** | 지연 표본 수집·백분위수 (헤더 전용) |
| `jetson/tests/test_latency_stats.cpp` | **신규** | 백분위수 정의, 꼬리 검출 |
| `jetson/tools/jetson_roi_client.cpp` | 수정 | 구간별 타이머, 종료 시 요약, CSV. **제어 계층 결선도 이 파일**(작업 ③) |
| `jetson/include/network/TcpRoiClient.hpp` | 수정 | `setNoDelay()` 선언 |
| `jetson/src/network/TcpRoiClient.cpp` | 수정 | `setNoDelay()` 구현 |
| `jetson/tests/test_tcp_roi_client.cpp` | 수정 | `setNoDelay` 계약 검사 추가 |
| `arty/ps_db/tools/ps_classifier_server.c` | 수정 | 요청 구간 측정, 세션 요약, CSV, `ADAS_TCP_NODELAY` |
| `arty/ps_db/include/network/tcp_roi_server.h` | 수정 | `adas_tcp_roi_server_set_no_delay()` 선언 |
| `arty/ps_db/src/network/tcp_roi_server.c` | 수정 | 〃 구현 |
| `arty/ps_db/tests/test_tcp_roi_server.c` | 수정 | `getsockopt` 으로 옵션 도달 확인 |
| `arty/ps_eb/include/network/tcp_roi_server.h` | 수정 | ps_db 와 동일 파일 (동기화) |
| `arty/ps_eb/src/network/tcp_roi_server.c` | 수정 | 〃 |
| `arty/ps_eb/tests/test_tcp_roi_server.c` | 수정 | 〃 |
| `tools/summarize_measurement.py` | **신규** | CSV → 백분위수 표·조건 비교표 |
| `arty/ps_db/README.md` | 수정 | 측정 환경변수 안내 |
| `jetson/CMakeLists.txt` | 수정 | `test_latency_stats` 등록 (작업 ③ 변경과 같은 파일) |

**`tools/` 는 저장소 루트의 새 디렉터리다.** 없으면 만든다.

---

## 2. ps_eb 완성 (작업 ②)

DB 에서 분기한 상태였던 EB PS 를 3-IP · 6-op 계약으로 다시 구현했다.

### 하드웨어 계약 (커널과 사용자 공간이 공유)

| 경로 | 구분 | 내용 |
| --- | --- | --- |
| `arty/ps_eb/include/driver/adas_classifier_eb_hw.h` | **신규** | **3-IP 주소맵·엔진별 레지스터 오프셋·6-op 표.** 이 트리의 정본 |
| `arty/ps_eb/include/driver/adas_classifier_uapi.h` | 수정 | DMA 배치(`0x5b000`), ABI **2**, requant+leaky, `RUN_OP`/`GET_STATUS` |

### 커널

| 경로 | 구분 | 내용 |
| --- | --- | --- |
| `arty/ps_eb/kernel/adas_classifier_drv.c` | 수정 | 창 3개 ioremap, **6-op 시퀀서**, 단일 op 실행, 상태 조회 |
| `arty/ps_eb/kernel/adas-classifier.dtsi` | 수정 | `reg` 3개, `compatible = "adas,classifier-eb-1.0"` |
| `arty/ps_eb/kernel/README.md` | 수정 | ioctl 계약과 DB 와의 차이 |

### 사용자 공간

| 경로 | 구분 | 내용 |
| --- | --- | --- |
| `arty/ps_eb/include/accelerator/classifier_registers.h` | 수정 | 하드웨어 헤더로 포워딩만 (값 중복 제거) |
| `arty/ps_eb/include/accelerator/classifier_accelerator.h` | 수정 | 주소 9개, requant+leaky, `run_op`/`run`/`op_buffers` |
| `arty/ps_eb/src/accelerator/classifier_accelerator.c` | 수정 | **6-op 시퀀서** (엔진별 프로그래밍, idle→start→done) |
| `arty/ps_eb/include/accelerator/classifier_buffers.h` | 수정 | uapi 로 포워딩, 출력 주소 헬퍼 |
| `arty/ps_eb/src/accelerator/classifier_buffers.c` | 수정 | ping-pong 포함 9개 버퍼 배치 |
| `arty/ps_eb/include/driver/classifier_device.h` | 수정 | bias·act_a/act_b 접근자, `run_op`, `status` |
| `arty/ps_eb/src/driver/classifier_device.c` | 수정 | 〃 구현, ABI 검사 강화 |
| `arty/ps_eb/include/model/classifier_model.h` | 수정 | bias 를 `pl_parameters` 밖으로 (EB 는 DDR 로 넘김) |
| `arty/ps_eb/src/model/classifier_model.c` | 수정 | 〃 |
| `arty/ps_eb/include/postprocess/classifier_postprocess.h` | 수정 | ps_db 와 동기화 (`confidence_ppm`) |
| `arty/ps_eb/src/postprocess/classifier_postprocess.c` | 수정 | 〃 |

### 도구·테스트·빌드

| 경로 | 구분 | 내용 |
| --- | --- | --- |
| `arty/ps_eb/tools/ps_classifier_server.c` | 수정 | leaky 인자 3개 추가, bias DDR 복사, 측정 계측 |
| `arty/ps_eb/tools/ps_eb_golden_test.c` | **신규** | **단계별** 온보드 golden 대조 (op 하나씩) |
| `arty/ps_eb/tests/test_classifier_accelerator.c` | 수정 | ping-pong 규칙, op 표, requant 검증 |
| `arty/ps_eb/tests/test_classifier_buffers.c` | 수정 | 9개 버퍼 겹침·정렬·출력 위치 |
| `arty/ps_eb/tests/test_classifier_postprocess.c` | 수정 | ps_db 와 동기화 |
| `arty/ps_eb/CMakeLists.txt` | 수정 | `ps_eb_golden_test` 등록, **`libm` 링크** |
| `arty/ps_eb/README.md` | 수정 | DB 와의 차이, DMA 배치, 보드 절차 |

---

## 3. 제어 로직 (작업 ③)

분류 결과 → 안전 상태 판단 → UART 로 Raspberry Pi 전송.

### `common/` — **별도 커밋으로 보낼 것** (§6 참고)

| 경로 | 구분 | 내용 |
| --- | --- | --- |
| `common/include/common/SafetyJudge.hpp` | 수정 | `ClassMap` 추가, `JudgeConfig::classes` |
| `common/src/SafetyJudge.cpp` | 수정 | 클래스 판정을 설정 기반으로, background 제외 |
| `common/include/common/SafetyHazardLatch.hpp` | 수정 | `release_ms` 추가 |
| `common/src/SafetyHazardLatch.cpp` | 수정 | 프레임 수 **와** 시간을 둘 다 만족해야 해제 |
| `common/README.md` | 수정 | 위 두 변경의 근거 |

### Jetson 제어 계층 (전부 신규)

| 경로 | 내용 |
| --- | --- |
| `jetson/include/control/DetectionAdapter.hpp` | bbox(Jetson) + class(Arty) → 판단용 레코드 |
| `jetson/src/control/DetectionAdapter.cpp` | 좌표 정규화, Unclassified 판정 |
| `jetson/include/control/SafetyDecider.hpp` | 판단 → 정지 이벤트 래치 |
| `jetson/src/control/SafetyDecider.cpp` | 〃 |
| `jetson/include/control/SafetyTransmitter.hpp` | 20 ms 송신, watchdog, STOP 즉시 송신 |
| `jetson/src/control/SafetyTransmitter.cpp` | 〃 |
| `jetson/include/control/UartPort.hpp` | 포트 인터페이스 + termios 구현 선언 |
| `jetson/src/control/UartPort.cpp` | termios 구현 |
| `jetson/tests/test_detection_adapter.cpp` | 좌표 변환, class 매핑, Unclassified 4경로 |
| `jetson/tests/test_safety_decider.cpp` | zone·거리 경계, 표지판 규칙, 래치 전이 |
| `jetson/tests/test_safety_transmitter.cpp` | 주기, watchdog, 즉시 송신, 프레임 바이트 |

**`jetson/include/control/` 과 `jetson/src/control/` 은 새 디렉터리다.**

| 경로 | 구분 | 내용 |
| --- | --- | --- |
| `jetson/CMakeLists.txt` | 수정 | `adas_safety`·`adas_jetson_control` 타깃, 테스트 3종 |
| `jetson/tools/jetson_roi_client.cpp` | 수정 | 제어 계층 결선 (`ADAS_UART_PORT` 로 활성화) |
| `jetson/README.md` | 수정 | 제어 절 추가 |

---

## 4. PL 디렉터리 통합 (작업 ④) — **파일이 아니라 이동**

`arty/handoffs/` 를 없애고 `arty/pl_eb/handoff/` 로 넣었다. PL 관련 최상위
디렉터리가 3개(`handoffs`/`pl_db`/`pl_eb`)에서 2개로 줄었다.

```text
arty/handoffs/eb/arty96_pl_handoff/   →   arty/pl_eb/handoff/     (44 파일)
```

**44개 파일의 내용은 한 바이트도 안 바뀌었다.** `CHECKSUMS.sha256` 으로
무결성을 검증하는 봉인된 인계물이라 손대지 않았다.

받는 쪽은 **파일을 받지 말고 디렉터리를 옮기면 된다.**

```bash
mkdir -p arty/pl_eb
git mv arty/handoffs/eb/arty96_pl_handoff arty/pl_eb/handoff
rmdir arty/handoffs/eb arty/handoffs
```

경로를 가리키던 문서 4곳을 함께 고쳤다.

| 경로 | 내용 |
| --- | --- |
| `arty/pl_eb/README.md` | `handoff/` 설명, **golden 두 벌의 차이** |
| `arty/README.md` | 트리 설명 |
| `README.md` | 저장소 표 |
| `docs/DB_ARTY_BRINGUP_REPORT.md` | 참조 경로 + EB 진행 상황 |
| `docs/contracts/PL_HANDOFF_CHECKLIST.md` | 참조 경로 |

---

## 5. 한 세트로 보내야 하는 파일

따로 보내면 **컴파일은 되고 동작만 틀리는** 조합들이다.

| 묶음 | 파일 | 왜 |
| --- | --- | --- |
| EB 하드웨어 계약 | `arty/ps_eb/include/driver/adas_classifier_eb_hw.h` + `.../adas_classifier_uapi.h` + `arty/ps_eb/kernel/adas_classifier_drv.c` + `arty/ps_eb/kernel/adas-classifier.dtsi` | 주소·offset·op 표를 공유한다. 하나만 옛것이면 엉뚱한 레지스터를 건드린다 |
| EB DMA 배치 | `adas_classifier_uapi.h` + `classifier_buffers.*` + `classifier_device.*` | 커널과 사용자 공간이 같은 offset 을 계산해야 한다 |
| EB 모델 | `classifier_model.*` + `ps_classifier_server.c` + `ps_eb_golden_test.c` | bias 가 `pl_parameters` 밖으로 나갔다 |
| 제어 판단 | `common/SafetyJudge.*` + `common/SafetyHazardLatch.*` + `jetson/*/control/*` | `ClassMap`·`release_ms` 가 없으면 제어 계층이 빌드되지 않는다 |
| NODELAY | `tcp_roi_server.{h,c}` + `TcpRoiClient.{hpp,cpp}` | 한쪽만 켜면 절반만 효과가 있다 |
| 측정 | `LatencyStats.hpp` + `jetson_roi_client.cpp` + `ps_classifier_server.c` + `summarize_measurement.py` | 두 CSV 를 빼야 네트워크 몫이 나온다 |

`arty/ps_db/` 와 `arty/ps_eb/` 의 `tcp_roi_server.{h,c}`, `test_tcp_roi_server.c`
는 **바이트 단위로 같은 파일**이다. 한쪽만 갱신하면 두 트리가 갈라진다.

---

## 6. 동작이 바뀐 것 — 받는 쪽이 알아야 한다

### ① 분류 실패로 프로세스가 죽지 않는다 (`jetson_roi_client`)

기존에는 `classify()` 실패 한 번에 `EXIT_FAILURE` 였다. 제어 계층이 붙은
이상 ROI 한 건 실패로 로봇 제어가 통째로 사라지면 안 되므로 바꿨다.

```text
실패 1건      → 해당 ROI 만 Unclassified (기하 규칙으로 판단)
연속 3건      → 링크 장애로 보고 STOP (래치를 거치지 않는다)
연결 끊김     → 프레임마다 재연결 시도
비정상 종료   → 카메라 정지일 때만
```

### ② EB 드라이버 ABI·compatible 분리

| | DB | EB |
| --- | --- | --- |
| ABI 버전 | 1 | **2** |
| dtsi `compatible` | `adas,classifier-1.0` | **`adas,classifier-eb-1.0`** |

같은 이름을 쓰면 반대쪽 드라이버가 붙어 존재하지 않는 레지스터를 건드린다.
**DB 보드의 dtsi 는 바꾸지 말 것.**

### ③ `common/` 은 별도 커밋으로

`common/ORIGIN` 규칙이다 — 이 디렉터리는 KR260 저장소에서 복사한 것이고,
여기서 고친 것을 그쪽으로 되돌릴 수 있어야 한다. 두 변경 모두 **기본값이
KR260 동작과 같아** 그대로 back-port 할 수 있다.

---

## 7. 받은 뒤 확인

```bash
# PS (DB / EB 각각)
cmake -S arty/ps_db -B arty/ps_db/build && cmake --build arty/ps_db/build -j2
ctest --test-dir arty/ps_db/build --output-on-failure      # 9/9

cmake -S arty/ps_eb -B arty/ps_eb/build && cmake --build arty/ps_eb/build -j2
ctest --test-dir arty/ps_eb/build --output-on-failure      # 9/9

# Jetson (OpenCV·TensorRT 있는 보드에서)
cmake -S jetson -B jetson/build && cmake --build jetson/build -j2
ctest --test-dir jetson/build --output-on-failure           # 제어 3종 포함

# EB 커널 모듈
cd arty/ps_eb/kernel && make KDIR=/path/to/target/kernel/build
```

### 이번 변경에서 검증한 범위

| 대상 | 어디까지 |
| --- | --- |
| `arty/ps_db`, `arty/ps_eb` | **빌드 + ctest 9/9 통과** |
| `common/`, Jetson 제어 3종 | **빌드 + 테스트 실행 통과** |
| Jetson 나머지 C++ | 문법 검사만 (개발 PC 에 OpenCV·TensorRT 없음) |
| EB 커널 드라이버 | 문법 검사만 (커널 헤더 없음) |
| 실보드 | **전부 미검증** |

---

## 8. 문서 (참고용, 코드와 무관)

| 경로 | 구분 | 내용 |
| --- | --- | --- |
| `docs/PROJECT_OVERVIEW.md` | **신규** | 외부 공개용 프로젝트 정리본 |
| `docs/FPS_MEASUREMENT_GUIDE.md` | **신규** | FPS 정의, 계측 위치, 측정 절차, 판정 기준 |
| `docs/JETSON_CONTROL_DESIGN.md` | **신규** | 제어 로직 설계·설정값·캘리브레이션 절차 |
