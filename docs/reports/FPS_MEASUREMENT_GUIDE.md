# FPS·지연 측정 안내

보고서에 쓸 수치를 뽑기 위한 계측과 절차를 정리한다. Jetson 클라이언트와
Arty PS 서버 양쪽에 구간 측정을 넣었고, 두 쪽 숫자를 빼면 네트워크 몫이 나온다.

---

## 1. 먼저 — "FPS"를 무엇으로 부를지 정한다

ROI 단위 분류 구조에서는 서로 다른 세 숫자가 전부 FPS라고 불릴 수 있다.
하나만 적으면 읽는 쪽이 다른 것으로 이해하므로, **세 개를 같이 보고한다.**

| 이름 | 정의 | 무엇을 말하는가 |
| --- | --- | --- |
| **frame FPS** | 완료된 프레임 / 초 | **화면 결과가 갱신되는 속도.** 사람이 "자연스럽다"고 느끼는 값이 이것이다 |
| **ROI/s** | 분류한 ROI / 초 | 보드 처리량. 프레임당 ROI 개수와 무관한 순수 성능 |
| **처리분 FPS** | 1 / (프레임 시간 − 카메라 대기) | 카메라가 더 빨랐다면 낼 수 있었을 상한. 카메라 병목과 처리 병목을 가른다 |

루프가 완전 순차이므로 이 관계가 성립한다.

```text
프레임 시간 = 캡처(대기 포함) + proposal 추론 + Σ(crop + TCP 왕복) + publish
```

프레임당 ROI 개수 `N`이 프레임마다 달라지므로 **`N`이 섞인 평균 FPS 하나는
재현도 비교도 되지 않는다.** 그래서 요약 출력은 `N`별 프레임 시간 표를
따로 낸다. 보고서에는 이렇게 적는 것이 정확하다.

> 검출 ROI 2건 기준 frame FPS 22.8, ROI 처리량 47.5 ROI/s,
> 카메라 대기를 뺀 처리분 상한 28.1 FPS

위 예시는 `SHUTDOWN_LOG_2026-08-24.md`의 값이다. 같은 자리에 있던 옛
예시(frame FPS 14.9 / 34.6 ROI/s / 20.7 FPS)는 TCP 응답 40 ms 지연이 있던
시점의 값이라 교체했다.

---

## 2. 계측 위치

### Jetson (`jetson/tools/jetson_roi_client.cpp`)

| 구간 | 재는 것 |
| --- | --- |
| `capture` | `captureFrame()` — **다음 프레임을 기다린 시간 포함** |
| `proposal 추론` | `RoiProposer::propose()` |
| `crop+prepare` | ROI당 crop·resize·BGR→RGB |
| `TCP 왕복 RTT` | ROI당 `classify()` 호출~반환 |
| `MJPEG publish` | 오버레이 공유 슬롯 갱신 |
| `프레임 전체` / `처리분` | 루프 한 바퀴 / 거기서 캡처 대기를 뺀 값 |

계측은 항상 켜져 있다 — `steady_clock` 호출 몇 번은 ms 단위 구간에 비해
무시할 수 있고, 스위치를 두면 "측정할 때만 다른 경로"라는 의심이 생긴다.
**출력만** 환경변수로 조절한다.

| 환경변수 | 기본값 | 뜻 |
| --- | --- | --- |
| `ADAS_MEASURE_WARMUP` | 10 | 통계에서 제외할 앞쪽 프레임 수 (TensorRT 첫 추론·카메라 안정화) |
| `ADAS_MEASURE_QUIET` | 0 | 1이면 ROI별 결과 줄을 끈다 — **측정 실행에서는 켤 것** |
| `ADAS_MEASURE_PROGRESS` | 100 | 몇 프레임마다 진행 줄을 찍을지 (0=끔) |
| `ADAS_MEASURE_CSV` | (없음) | ROI 한 건당 한 행을 남길 CSV 경로 |
| `ADAS_TCP_NODELAY` | 0 | 1이면 분류 socket의 Nagle을 끈다 (§5) |

### Arty PS (`arty/ps_db/tools/ps_classifier_server.c`)

요청 payload를 다 받은 시점부터 응답 직전까지를 재고, 그 안을 셋으로 쪼갠다.

| 구간 | 재는 것 |
| --- | --- |
| `preprocess` | UINT8→INT8 양자화 + 1픽셀 zero padding |
| `pl_run` | PL 가속기 start~done |
| `postprocess` | GAP → FC → argmax → confidence |
| `server total` | 위 셋을 포함한 요청 처리 전체 |

| 환경변수 | 기본값 | 뜻 |
| --- | --- | --- |
| `ADAS_PS_REPORT_EVERY` | 100 | 몇 건마다 진행 줄을 찍을지 (0=끔) |
| `ADAS_PS_CSV` | (없음) | 요청 한 건당 한 행을 남길 CSV 경로 |
| `ADAS_TCP_NODELAY` | 0 | 1이면 accept 후 client socket의 Nagle을 끈다 |

PS 요약은 **연결이 끊길 때** 출력된다. Jetson 쪽을 Ctrl-C로 멈추면 보드
콘솔에 그 세션의 요약이 남는다.

### 두 숫자를 빼면 네트워크가 나온다

```text
네트워크 왕복 = Jetson의 RTT − PS의 server total
```

---

## 3. 측정 절차

### 준비

```bash
# Jetson: 클럭 고정. 안 하면 실행마다 값이 흔들려 재현이 안 된다.
sudo nvpmodel -m 0
sudo jetson_clocks
```

### 실행 — Arty 보드

```bash
insmod adas_classifier_drv.ko

ADAS_PS_CSV=/tmp/ps_nodelay_off.csv \
./ps_classifier_server "*" 5000 /home/petalinux/arty_deploy_v2/model \
    6 1 1342756158 38 1322019071 35 1920779908 38 2.9190799511495295e-05
```

### 실행 — Jetson

```bash
ADAS_MEASURE_QUIET=1 \
ADAS_MEASURE_CSV=/tmp/jetson_nodelay_off.csv \
./jetson/build/jetson_roi_client /dev/video0 <ARTY_IP> 5000 \
    models/proposal/export/proposal_yolov8n_fp16.engine
```

60초 정도 돌린 뒤 **Ctrl-C**로 멈추면 양쪽에 요약이 출력된다.

### 조건을 바꿔 반복

| 실행 | 조건 | 목적 |
| --- | --- | --- |
| 1 | 기본 | 기준선 |
| 2 | 양쪽 `ADAS_TCP_NODELAY=1` | §5 판정 |
| 3 | MJPEG 포트 인자 추가 | 스트리밍 부하 확인 |
| 4 | `--full-frame` | proposal 추론을 뺀 상한 확인 |

각 조건을 **최소 2회** 돌려 같은 값이 나오는지 본다. 1회 실행의 숫자는
보고서에 넣지 않는다.

### 백분위수·비교표

```bash
python3 tools/summarize_measurement.py /tmp/jetson_nodelay_off.csv /tmp/ps_nodelay_off.csv
python3 tools/summarize_measurement.py --compare \
    /tmp/jetson_nodelay_off.csv /tmp/jetson_nodelay_on.csv
```

PS 서버는 콘솔에 평균·최소·최대만 찍는다. **백분위수는 CSV에서만 나온다.**

---

## 4. 결과 읽는 법

| 관찰 | 해석 | 다음 조치 |
| --- | --- | --- |
| `capture`가 프레임 시간의 대부분 | **카메라 병목** — 처리가 이미 카메라보다 빠르다 | 최적화는 FPS를 못 올린다. 카메라 fps/해상도를 올리는 논의로 |
| `proposal 추론`이 지배 | Jetson 병목 | imgsz·FP16·배치 검토 |
| `RTT`가 지배하고 N이 크다 | 보드 병목 | ROI 개수 상한, PL 최적화 |
| `RTT` ≫ PS `server total` | **네트워크 병목** | §5, 링크 속도, 경유 장비 |
| `RTT`의 median은 작은데 p95가 크다 | **간헐적 정체** — 평균만 보면 안 보인다 | §5 |

`pl_run`의 기준은 실보드 골든 테스트의 **6.597 ms / 6.613 ms**
(`accelerator time=6597 us`, `6613 us`)이고, 라이브 세션 평균은
**6.611~6.617 ms**다. 이것보다 크게 나오면 그 자체가 조사 대상이다.

---

## 5. TCP_NODELAY — 켜고 끄고 두 번 재서 판정한다

**현상의 근거:** 응답이 header 20 B와 result 12 B로 **두 번 송신**된다
(`tcp_roi_server.c`). Nagle이 켜져 있으면 두 번째 조각이 첫 조각의 ACK를
기다리고, 상대의 delayed ACK는 Linux 기본 최대 40 ms다. ROI 한 건의 PL
실행이 6.6 ms인 것을 생각하면 이 지연은 처리량을 통째로 지배한다.

**기본값은 끈 상태로 두었다.** 측정으로 확인하기 전에 기본값을 바꾸면
"고쳤다"가 아니라 "바꿨다"에 그친다.

### 판정 기준

`--compare`로 두 실행의 `rtt_us`를 나란히 놓고 본다.

| 결과 | 판정 |
| --- | --- |
| p95가 40 ms대에서 median 근처로 내려온다 | **Nagle 확정.** 양쪽 기본값을 `on`으로 옮긴다 |
| median·p95 모두 변화 없음 | 병목이 아니다. 옵션은 그대로 두고 다른 구간을 본다 |
| median은 같은데 p95만 조금 준다 | 부분 기여. 수치를 그대로 적고 기본값은 유지 |

**기본값을 옮기는 방법** — Nagle로 확정되면 소스 두 곳에서 기본값을 1로
바꾸면 된다 (`env_ulong("ADAS_TCP_NODELAY", 0ul)` → `1ul`, Jetson·PS 각 1곳).
그 경우 응답 32바이트를 한 번의 `send()`로 합치는 것도 같이 검토할 만하다 —
옵션에 기대지 않고 원인 자체를 없애는 쪽이다.

---

## 6. 보고서에 조건을 함께 적는다

숫자만 있는 측정은 재현되지 않는다. 최소한 이것들을 같이 적는다.

- 클럭 설정 (`nvpmodel -m 0`, `jetson_clocks` 적용 여부)
- 카메라 포맷과 fps (`camera:` 줄에 출력된다)
- warmup 프레임 수, 측정 구간 길이, 반복 횟수
- MJPEG 스트리밍 on/off, `TCP_NODELAY` on/off
- 네트워크 경로 (직결/스위치 경유, 링크 속도)
- 평균 ROI 개수 — **이걸 빼면 frame FPS는 비교할 수 없다**
- 중앙값과 p95를 함께 (평균만 적지 않는다)

---

## 7. 관련 파일

| 경로 | 내용 |
| --- | --- |
| `jetson/include/metrics/LatencyStats.hpp` | 표본 수집·백분위수 (헤더 전용) |
| `jetson/tests/test_latency_stats.cpp` | 백분위수 정의와 꼬리 검출 검사 |
| `jetson/tools/jetson_roi_client.cpp` | 파이프라인 구간 측정과 요약 출력 |
| `arty/ps_db/tools/ps_classifier_server.c` | 요청 구간 측정과 세션 요약 |
| `arty/ps_db/tests/test_tcp_roi_server.c` | `TCP_NODELAY` 토글이 socket에 도달하는지 검사 |
| `tools/summarize_measurement.py` | CSV → 백분위수 표·조건 비교표 |
