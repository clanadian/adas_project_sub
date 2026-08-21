# Jetson–Arty ROI Classifier

## Architecture

```text
USB camera
    │
    ▼
Jetson Nano
  V4L2 capture → bbox proposal → square crop → 96×96 RGB UINT8
    │
    │ TCP request: bbox metadata + ROI image
    ▼
Arty Z7-20 PS (Linux)
  receive → UINT8→INT8 → 1-pixel zero padding → 98×98×3
    │
    │ DDR + AXI
    ▼
Arty Z7-20 PL
  Conv/ReLU/Pool ×3 → 12×12×64 INT8
    │
    ▼
Arty Z7-20 PS
  GAP → FC → argmax
    ├── TCP response: class_id, confidence_ppm → Jetson overlay
    └── bbox + class → SafetyJudge/HazardLatch
                         │ UART1 115200 8N1
                         ▼
                 Raspberry Pi (TurtleBot)
```

## Interfaces

| 구간 | 형식 |
| --- | --- |
| Jetson → PS | 원본 bbox 28 B + `96×96×3` RGB UINT8 NHWC |
| PS → PL | `98×98×3`, signed INT8, NHWC, zero border |
| PL → PS | `12×12×64`, signed INT8, NHWC |
| PS → Jetson | `class_id`, `confidence_ppm` |

TCP는 persistent connection에서 ROI 한 건씩 요청·응답한다. Multi-byte 정수는
network byte order를 사용한다.

## TurtleBot UART

최종 DB XSA는 UART0을 Linux 콘솔로 유지하고 UART1을 EMIO로 추가했다.

| 신호 | Arty Z7-20 핀 | 연결 |
| --- | --- | --- |
| UART1 TXD | PMOD JA1 (`Y18`) | Raspberry Pi RXD |
| UART1 RXD | PMOD JA2 (`Y19`) | Raspberry Pi TXD |
| GND | PMOD GND | Raspberry Pi GND |

PS에서는 UART1이 `/dev/ttyPS1`로 나타난다. 최종 운용에서는
`ps_classifier_server`에 `ADAS_UART_PORT=/dev/ttyPS1`을 설정하고, Jetson에는
`ADAS_UART_PORT`를 설정하지 않는다. 두 프로세스가 동시에 UART를 송신하면 안 된다.

## 실기기 접속

| 대상 | 명령 | 비밀번호 |
| --- | --- | --- |
| RPi (TurtleBot) | `ssh ubuntu@10.10.16.200` | `ubuntu` |
| Jetson | `ssh -J ubuntu@10.10.16.200 jetson@192.168.100.2` | `123456` |
| Arty DB | `ssh petalinux@10.10.16.61` | `123456` |

Jetson은 RPi 뒤 사설망(`192.168.100.0/24`)에 있어서 RPi를 거쳐야 붙는다
(`-J`가 그 경유지). Arty DB는 실습망(`10.10.16.0/24`)에 바로 있어서 경유
없이 붙는다.

## Repository

| 경로 | 내용 |
| --- | --- |
| `jetson/` | 캡처, ROI 생성, crop, TCP client, 결과 시각화; 기존 Jetson 제어 경로는 시험·대체용으로 유지 |
| `turtlebot/` | RPi ROS 2 안전 상태 수신, `cmd_vel` 중재, 기동·종료 스크립트 |
| `common/` | 공통 안전 판단 로직(`SafetyJudge`/`HazardLatch`/`UartFrame`), 최종 구성에서는 Arty PS가 링크 |
| `arty/ps_db/` | DB PL용 TCP server, 전처리, 가속기 제어, 후처리, 안전 판단·UART 송신 |
| `arty/ps_eb/` | EB PL용 TCP server, 전처리, 3-IP 6-op 가속기 제어, 후처리 |
| `arty/pl_db/` | DB 96×96 ROI 분류 가속기 HLS 소스와 보고서 |
| `arty/pl_eb/` | EB 96×96 ROI 분류 가속기 HLS 소스, 골든, PS7 프리셋, PS 인계 산출물(bitstream·golden·weights, 소스와 한 트리로 병합됨) |
| `arty/models/` | INT8 양자화 산출물 (db·eb 각각, 교환 불가) |
| `arty/classifier_linux_db/`, `arty/classifier_linux_eb/` | 각 보드용 PetaLinux 프로젝트 (XSA, 커널 드라이버 레시피, rootfs) |
| `arty/deploy/` | SD 카드 굽기·검사 스크립트(`burn_sd.sh`, `inspect_sd.sh`) |
| `arty/tools/` | XSA 하드웨어 설정 검증(`check_xsa.sh`) |
| `shared/` | Jetson–PS 공통 TCP 프로토콜 |
| `docs/contracts/` | 전체 데이터·하드웨어 계약 |

정본 계약: [`docs/contracts/ROI_CLASSIFIER_CONTRACT.md`](docs/contracts/ROI_CLASSIFIER_CONTRACT.md)

주요 문서:

| 문서 | 내용 |
| --- | --- |
| [`docs/ARTY_SD_BOOT_USAGE.md`](docs/ARTY_SD_BOOT_USAGE.md) | DB/EB SD 부팅, Jetson 연결 사용법 |
| [`docs/ARTY_NETWORK_SETUP.md`](docs/ARTY_NETWORK_SETUP.md) | Arty 네트워크 인터페이스 설정 |
| [`docs/JETSON_CONTROL_DESIGN.md`](docs/JETSON_CONTROL_DESIGN.md) | 안전 판단·UART 제어 계층 설계, KR260 대비 변경분 |
| [`docs/DB_EB_VERIFICATION_SUMMARY.md`](docs/DB_EB_VERIFICATION_SUMMARY.md) | DB/EB 검증 결과 비교 |
| [`docs/contracts/PL_HANDOFF_CHECKLIST.md`](docs/contracts/PL_HANDOFF_CHECKLIST.md) | PL 인계 산출물 인수 기준 |

`108`개 파일 규모의 최신 팀 인계 내역(작업 시점 스냅샷, 이후 변경은 반영 안 됨)은
[`CHANGED_FILES.md`](CHANGED_FILES.md) 참고.

## Start and stop

기동은 **Arty → Jetson → TurtleBot**, 종료는 **Jetson → Arty →
TurtleBot** 순서다. Jetson을 먼저 종료해야 Arty가 클라이언트 연결 종료를
감지하고 세션 측정 요약을 `server.log`에 남긴다.

### Start

Arty에 접속해 golden 검증 후 분류 서버를 실행한다.

```sh
sudo ps_db_golden_test ~/arty_deploy_v2/model

sudo sh -c 'ADAS_UART_PORT=/dev/ttyPS1 ADAS_TCP_NODELAY=1 nohup ps_classifier_server "*" 5000 /home/petalinux/arty_deploy_v2/model 6 1 1467099144 38 1160501223 35 1422046702 38 8.540366656652573e-06 > /home/petalinux/server.log 2>&1 &'

grep -a ':1388' /proc/net/tcp && echo LISTEN
```

Jetson에서 ROI 클라이언트와 MJPEG 서버를 실행한다.

```bash
setsid nohup ~/start_adas.sh > /tmp/jetson.log 2>&1 < /dev/null &
```

TurtleBot RPi에서 로봇 bringup, UART 수신, 조종 중재를 실행한다.

```bash
~/start_robot.sh
ros2 topic echo /adas/safety_state --field data
```

상태 값은 `0=CLEAR`, `1=SLOW`, `2=STOP`이다.

### Stop

Jetson을 먼저 종료한다. `SIGINT`를 써야 측정 요약이 정상 출력된다.

```bash
pkill -INT -f '[j]etson_roi_client'
```

Arty에서 세션 요약을 확인하고 서버를 종료한다.

```sh
sudo sed -n '/=== session summary ===/,/=====/p' /home/petalinux/server.log | tail -14

PID=$(ps | grep -a ps_classifier_server | grep -av grep | awk '{print $1}' | head -1)
[ -n "$PID" ] && sudo kill -TERM "$PID"
```

마지막으로 TurtleBot을 정지한다. 스크립트가 0 속도 명령, 모터 토크
OFF, ROS 노드 종료를 순서대로 수행한다.

```bash
~/stop_robot.sh
```

상세 절차와 장애 대응은
[`turtlebot/docs/SERVER_START_STOP.md`](turtlebot/docs/SERVER_START_STOP.md)에 있다.

## Build and test

```bash
cmake -S jetson -B jetson/build
cmake --build jetson/build -j2
ctest --test-dir jetson/build --output-on-failure

cmake -S arty/ps_db -B arty/ps_db/build
cmake --build arty/ps_db/build -j2
ctest --test-dir arty/ps_db/build --output-on-failure

cmake -S arty/ps_eb -B arty/ps_eb/build
cmake --build arty/ps_eb/build -j2
ctest --test-dir arty/ps_eb/build --output-on-failure
```
