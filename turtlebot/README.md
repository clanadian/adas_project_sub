# turtlebot/ — TurtleBot(RPi) 백업 사본 + 전체 실행 안내

최종 RPi 반영 2026-08-25 · 대상 저장소 `adas_project_sub`

---

## 0. 이 디렉터리의 성격 — 먼저 읽을 것

**이것은 백업 사본이다. 실제 데모는 이 디렉터리로 돌지 않는다.**

지금 동작하는 데모는 **TurtleBot(RPi)의 홈 디렉터리(`/home/ubuntu`)**에서 돈다.
그쪽 구조는 그대로 두었고 이 디렉터리는 손대지 않는다. 여기 있는 것은 그
파일들을 **날짜 기준으로 떠 놓은 사본**이고, 목적은 두 가지다.

1. **파일이 날아갔을 때 복구.** RPi SD 카드가 죽거나 실수로 지웠을 때 여기서 되돌린다.
2. **Arty·젯슨·RPi 세 대를 한 문서로 설명.** 저장소만 받은 사람이 전체를 파악할 수 있게.

**`adas_project_sub` 의 기존 디렉터리(`arty/`, `jetson/`, `common/`, `shared/`,
`docs/`, `tools/`)는 하나도 건드리지 않았다.** 이 `turtlebot/` 하나만 추가된다.

### 터틀봇을 켜는 방법은 두 가지다

| | 방법 A — 기존 방식 (권장) | 방법 B — 이 저장소에서 |
|---|---|---|
| 어디서 | RPi 홈에 이미 있는 파일 | 이 디렉터리를 RPi 로 복사해서 |
| 언제 쓰나 | **평소. 데모는 항상 이쪽.** | 파일이 날아갔을 때, 새 보드에 처음 올릴 때 |
| 절차 | §2 | §4 (복원) 후 §2 |

---

## 1. 전체 구조

```
Jetson Nano             Arty Z7-20 (FPGA)                TurtleBot (RPi)
카메라 + YOLO      ──▶  ROI 를 INT8 CNN 으로 분류    ──▶  안전상태 받아 모터 제어
ROI 후보 추출      TCP   + 위험도 판단(zone/거리)    UART   + Xbox 조종
                   5000                             115200
```

- **판단은 Arty 가 한다.** RPi 는 결과를 UART 로 받기만 하는 소비자다.
- **RPi 가 젯슨↔Arty 사이를 라우팅한다.** RPi 를 끄면 둘이 통신하지 못한다.
- 젯슨 영상: `http://10.10.16.200:8080` (RPi 가 8080 을 젯슨으로 DNAT)

| 장비 | 주소 | 계정 | 비고 |
|---|---|---|---|
| TurtleBot(RPi) | `10.10.16.200` | `ubuntu` | 라우터 역할 겸함 |
| Jetson Nano | `192.168.100.2` | **`jetson`** | `nvidia` 아님. RPi 경유로만 접속 |
| Arty Z7-20 | `10.10.16.61` | **`petalinux`** | `root` 아님. RPi 경유로만 접속 |

```bash
ssh -J ubuntu@10.10.16.200 jetson@192.168.100.2
```
```bash
ssh -J ubuntu@10.10.16.200 petalinux@10.10.16.61
```

### 안전 프레임 (Arty → RPi)

`[0xA5][state][CRC-8/ATM]`, 115200 8N1, 단방향. 약 50 Hz.

| 상태 | 바이트 |
|---|---|
| CLEAR | `a5 00 59` |
| SLOW | `a5 01 5e` |
| STOP | `a5 02 57` |

---

## 2. 실행 — 순서를 지킬 것

**Arty → 젯슨 → RPi.** 젯슨 클라이언트는 접속에 실패하면 **재시도 없이 즉시
종료**한다. Arty 가 먼저 떠 있어야 한다.

### 2.1 Arty — 분류 서버

```bash
ssh -J ubuntu@10.10.16.200 petalinux@10.10.16.61
```

가속기부터 확인한다.

```sh
sudo ps_db_golden_test ~/arty_deploy_v2/model
```
```
PASS: 9216 bytes bit-exact, accelerator time=6597 us
```

서버를 올린다. **환경변수 두 개는 선택이 아니다.**

```sh
sudo sh -c 'ADAS_UART_PORT=/dev/ttyPS1 ADAS_TCP_NODELAY=1 nohup ps_classifier_server "*" 5000 /home/petalinux/arty_deploy_v2/model 6 1 1467099144 38 1160501223 35 1422046702 38 8.540366656652573e-06 > /home/petalinux/server.log 2>&1 &'
```

| 환경변수 | 빼면 |
|---|---|
| `ADAS_UART_PORT=/dev/ttyPS1` | **안전 프레임을 한 개도 안 보낸다.** 분류만 조용히 한다 → 로봇은 계속 STOP |
| `ADAS_TCP_NODELAY=1` | ROI 왕복이 9.7 ms → **51.6 ms** (5배 느려짐). 이유는 [`../docs/reports/PS_TCP_RESPONSE_FIX.md`](../docs/reports/PS_TCP_RESPONSE_FIX.md) |

확인한다. **`server.log` 가 비어 있어도 정상이다** — 블록 버퍼링 때문이다.
로그 말고 포트를 본다.

```sh
grep -a ':1388' /proc/net/tcp && echo LISTEN
```

`scripts/arty_start.sh` 가 위 과정을 담고 있다.

### 2.2 젯슨 — ROI 클라이언트

```bash
setsid nohup ~/start_adas.sh > /tmp/jetson.log 2>&1 < /dev/null &
```

```bash
grep -o 'rtt_us=[0-9]*' /tmp/jetson.log | tail -3
```

**9000~11000** 이면 정상. **50000 대면** Arty 를 `ADAS_TCP_NODELAY=1` 없이 띄운
것이다.

### 2.3 RPi — ROS 일체

```bash
~/start_robot.sh
```

bringup → **모터 토크 ON** → UART 수신 → joy → arbiter → teleop 순으로 올린다.

최종 SLOW 상한은 직선 `0.05 m/s`, 회전 `0.30 rad/s`다. 평상시 조종값보다
낮게 설정해 감속이 실제 주행과 화면에서 구분되도록 했다.

> bringup 은 매번 토크가 **꺼진 채로** 올라온다. 안 켜면 `/cmd_vel` 에 값이
> 실려도 바퀴가 안 돈다. 스크립트가 대신 켜 준다.

### 2.4 통합 UI

`start_robot.sh`가 UI 서버도 함께 실행한다. 브라우저에서 다음 주소로 접속한다.

```text
http://10.10.16.200:8090
```

- 영상은 Jetson MJPEG를 브라우저가 직접 받으며 RPi는 재인코딩하지 않는다.
- 안전 상태는 `/adas/safety_state`를 표시한다. 1초 이상 수신이 없으면
  마지막 상태 대신 `NO SIGNAL`을 표시한다.
- `/odom` 실측 속력, 조종 입력, 안전 개입에 따른 입력 차단 여부를 표시한다.
- Arty UART·Jetson 영상·모터 odometry 연결 상태를 각각 표시한다.
- JSON 상태는 `http://10.10.16.200:8090/api/state`에서 확인한다.

HTML·CSS·JavaScript는 `scripts/ui_server.py`의 `PAGE` 문자열에 있다.

### 2.5 확인

```bash
ros2 topic echo /adas/safety_state --field data
```

장면에 따라 0/1/2 가 바뀌면 끝까지 살아 있는 것이다. 계속 `2` 만 나오면
젯슨이나 Arty 가 안 떠 있다.

---

## 3. 종료 — 순서가 반대다

**젯슨 → Arty → RPi.** Arty 서버에는 **시그널 핸들러가 없다.** 세션 요약은
`SIGINT` 때가 아니라 **클라이언트 연결이 끊길 때** 출력되고 거기서 `fflush`
된다. 서버를 먼저 죽이면 그 실행의 측정치가 통째로 사라진다.

```bash
ssh -J ubuntu@10.10.16.200 jetson@192.168.100.2 "pkill -INT -f '[j]etson_roi_client'"
```
```sh
PID=$(ps | grep -a ps_classifier_server | grep -av grep | awk '{print $1}'); sudo kill -TERM $PID
```
```bash
~/stop_robot.sh
```

**완전히 꺼졌는지 판정** — UART 가 0 바이트여야 한다(켜져 있으면 약 50 Hz):

```bash
python3 ~/rawuart.py
```

자세한 것은 [`docs/SERVER_START_STOP.md`](docs/SERVER_START_STOP.md).

---

## 4. 복구 — 파일이 날아갔을 때

이 디렉터리를 RPi·젯슨에 되돌리는 절차다. **평소에는 할 필요가 없다.**

### 4.1 RPi

```bash
scp -r turtlebot/ros2_ws/src/rpi_adas_demo ubuntu@10.10.16.200:~/ros2_ws/src/
```
```bash
scp turtlebot/scripts/{button_teleop.py,ui_server.py,start_robot.sh,stop_robot.sh} ubuntu@10.10.16.200:~/
```
```bash
scp turtlebot/tools/{rawuart.py,joy_check.py} ubuntu@10.10.16.200:~/
```

실행 권한과 줄바꿈을 정리한 뒤 빌드한다.

```bash
ssh ubuntu@10.10.16.200 "sed -i 's/\r$//' ~/*.sh && chmod +x ~/*.sh"
```
```bash
ssh ubuntu@10.10.16.200 "cd ~/ros2_ws && source /opt/ros/humble/setup.bash && colcon build --packages-select rpi_adas_demo"
```

### 4.2 젯슨

```bash
scp -J ubuntu@10.10.16.200 turtlebot/scripts/start_adas.sh jetson@192.168.100.2:~/
```

젯슨의 C++ 소스와 빌드는 이 저장소의 **`jetson/`** 에 있다. 그쪽을 참고한다.

### 4.3 Arty

Arty 의 서버·모델은 이 저장소의 **`arty/`** 소관이다. 여기에는 기동·정지
스크립트만 있다.

```bash
scp -J ubuntu@10.10.16.200 turtlebot/scripts/{arty_start.sh,arty_stop.sh} petalinux@10.10.16.61:/tmp/
```

### 4.4 시스템 설정 — 파일 복사만으로는 안 되는 것

RPi 를 새로 깔았다면 이것들도 다시 해야 한다. **하나라도 빠지면 안 돈다.**

| 설정 | 내용 | 없으면 |
|---|---|---|
| `/boot/firmware/config.txt` | `enable_uart=1` | UART 자체가 없음 |
| `/boot/firmware/cmdline.txt` | `console=serial0,115200` **제거** | 커널 콘솔이 UART 를 씀 |
| systemd | `sudo systemctl mask serial-getty@ttyS0` | getty 가 안전 프레임을 먹음 |
| `/etc/sysctl.d/99-adas-forward.conf` | `net.ipv4.ip_forward=1` | 젯슨↔Arty 통신 불가 |
| iptables | `-t nat -A POSTROUTING -s 192.168.100.0/24 -d 10.10.16.0/24 -j MASQUERADE`<br>`-t nat -A PREROUTING -p tcp --dport 8080 -j DNAT --to 192.168.100.2:8080` | 위와 같음 + 영상 안 나옴 |
| | `sudo apt install iptables-persistent` | 재부팅하면 위 규칙 사라짐 |
| `/etc/modprobe.d/xbox_bt.conf` | `options bluetooth disable_ertm=Y` | Xbox 패드 BT 연결 안 됨 |
| netplan | eth0 `192.168.100.1/24`, USB NIC `10.10.16.253/32` + `10.10.16.61/32` 경로 | 라우팅 안 됨 |

---

## 5. 파일 대응표

| 이 디렉터리 | 장비에서의 위치 | 비고 |
|---|---|---|
| `ros2_ws/src/rpi_adas_demo/` | RPi `~/ros2_ws/src/rpi_adas_demo/` | 팀 정식 ROS 2 패키지 |
| `scripts/button_teleop.py` | RPi `~/button_teleop.py` | Xbox 조종. **패키지 밖에 있다** |
| `scripts/ui_server.py` | RPi `~/ui_server.py` | 최종 통합 HTML·상태 API 서버 |
| `scripts/start_robot.sh` | RPi `~/start_robot.sh` | |
| `scripts/stop_robot.sh` | RPi `~/stop_robot.sh` | |
| `tools/rawuart.py` | RPi `~/rawuart.py` | UART 진단 |
| `tools/joy_check.py` | RPi `~/joy_check.py` | 조이스틱 축·버튼 번호 |
| `scripts/start_adas.sh` | 젯슨 `~/start_adas.sh` | |
| `scripts/arty_start.sh` | Arty `/tmp/` | 원래 휘발됨. 여기 보존 |
| `scripts/arty_stop.sh` | Arty `/tmp/` | 원래 휘발됨. 여기 보존 |

### 일부러 안 가져온 것

| | 이유 |
|---|---|
| RPi `~/cmd_vel_arbiter.py` (63줄) | 패키지 정식본(350줄)의 **초기 시제품**. 안전 중재·resume gate·e-stop 이 전부 없다. 섞이면 낡은 쪽을 실행할 위험 |
| RPi `~/uart_receiver.py` (74줄) | 위와 같음. 정식본은 `rpi_adas_demo/uart_safety_receiver.py` |
| RPi `~/monitor.py` | 초기 디버그용 |
| RPi `~/robot_nolidar.launch.py` | `turtlebot3_bringup` 설치본과 바이트 동일 |
| RPi `~/xone/`, `~/xpadneo/` | 별개 git 저장소 |
| ZIP의 `detector.py`, `yolo_safety_node.py`, ONNX 모델, WebRTC 설정 | 폐기한 RPi 자체 YOLO 구조. 현재는 Jetson 탐지 + Arty 분류·판단 구조 |

---

## 6. ⚠️ 함정 — 이것만 알면 대부분 막힌다

### 6.1 모터 토크는 매번 꺼진 채로 올라온다

```bash
ros2 topic echo /sensor_state --once | grep torque
```

`false` 면 `/cmd_vel` 에 값이 실려도 바퀴가 안 돈다.

### 6.2 `pkill -f` 가 자기 SSH 를 죽인다

패턴이 호출한 셸의 명령줄에도 들어가기 때문이다. `[j]etson_roi_client` 처럼
대괄호를 넣거나 스크립트 파일로 만들어 실행한다.

### 6.3 Arty 는 busybox 다

`ps -o pid,args`, `head -c`, `timeout`, `base64` 가 **없다.** `ps -o` 를 쓰면
아무것도 안 나와서 "서버가 없다"고 오판하고, 다시 띄우면
`Address already in use` 가 난다.

```sh
ps | grep -a ps_classifier_server | grep -av grep
grep -a ':1388' /proc/net/tcp
```

### 6.4 `sudo ... &` 로 백그라운드 기동 금지

`Stopped (SIGTTIN)` 으로 그 자리에서 멈춘다. 스크립트 파일로 실행한다.

### 6.5 계속 STOP 만 나온다면

① 젯슨이 안 떠 있다 ② Arty 에 `ADAS_UART_PORT` 를 안 줬다 ③ 둘 다.

---

## 7. 문서

| 문서 | 내용 |
|---|---|
| [`docs/SERVER_START_STOP.md`](docs/SERVER_START_STOP.md) | 기동·종료 상세, 함정 10가지 표 |
| [`../docs/reports/UART_STATUS_REPORT.md`](../docs/reports/UART_STATUS_REPORT.md) | UART 점검 근거. "설정이 덜 됐다"는 오진의 실제 원인 |
| [`../docs/reports/PS_TCP_RESPONSE_FIX.md`](../docs/reports/PS_TCP_RESPONSE_FIX.md) | Arty 응답 40 ms 지연 원인·수정안 |
| [`../docs/reports/E2E_MEASUREMENT_REPORT.md`](../docs/reports/E2E_MEASUREMENT_REPORT.md) | Jetson→Arty→RPi 엔드투엔드 실측 (FPS·지연·UART·재현 절차) |

---

## 8. 성능 (2026-08-21 실측)

| 항목 | 값 |
|---|---|
| 가속기(PL) | **6.6 ms** / ROI, golden bit-exact |
| Arty 서버 전체 | 7.7 ms / ROI (전처리 0.51 + PL 6.61 + 후처리 0.58) |
| 젯슨↔Arty 왕복 | **9.7 ms** (직전 51.6 ms 에서 5.3배 개선) |
| 젯슨 파이프라인 | 25.7 FPS, 27 ROI/s, 오류 0 (47,102 ROI) |
| UART | 49.7 Hz, CRC 오류 0 |

병목은 이제 **젯슨의 YOLO proposal 28.2 ms** 다. 가속기가 아니다.

---

## 9. 이 사본의 기준 시점

2026-08-25 최종 RPi 압축본과 대조해 현재 데모에 필요한 파일만 반영했다.
통합 UI, 조종 입력, 기동 스크립트와 속도 중재 설정은 최종본 기준이며,
폐기한 RPi 자체 YOLO·WebRTC 파일과 모델은 의도적으로 포함하지 않았다.
