# 세 보드 빠른 기동·종료

대상: Arty Z7-20 + Jetson Nano + TurtleBot Raspberry Pi

## 순서

```text
물리 전원 ON : RPi → Arty·Jetson
서비스 기동  : Arty → Jetson → RPi
서비스 종료  : Jetson → Arty → RPi
```

RPi는 Jetson과 Arty로 들어가는 SSH 경유지이므로 전원은 먼저 켠다. 하지만 ROS
서비스는 Arty 분류 서버와 Jetson 클라이언트가 준비된 뒤 마지막에 시작한다.

## 접속 주소 한눈에 보기

| 대상 | 주소 | 어디에서 접속하나 | 용도 |
|---|---|---|---|
| RPi | `10.10.16.200` | 작업 PC | SSH 경유지·TurtleBot·통합 UI |
| Arty | `10.10.16.61` | 작업 PC에서 RPi를 SSH 점프 호스트로 사용 | 분류 서버·PS 안전 판단 |
| Jetson | `192.168.100.2` | 작업 PC에서 RPi를 SSH 점프 호스트로 사용 | 카메라·후보 탐지·MJPEG |
| Jetson MJPEG | `http://10.10.16.200:8080` | 작업 PC 브라우저 | RPi가 Jetson `:8080`으로 포트 전달 |
| 통합 UI | `http://10.10.16.200:8090` | 작업 PC 브라우저 | RPi가 HTML·안전 상태·속도 제공 |

`192.168.100.2`는 RPi와 Jetson 사이의 유선망 주소이므로 작업 PC에서 직접
접속하지 않는다. 예전에 사용한 Jetson 주소 `10.10.16.108`도 현재 구성에서는
사용하지 않는다. 작업 PC에서 Arty나 Jetson으로 들어갈 때는 항상
`10.10.16.200`을 경유한다.

## 1. 네트워크 확인

작업 PC에서 RPi에 접속한다.

```bash
ssh ubuntu@10.10.16.200
```

RPi에서 두 보드가 보이는지 확인한다.

```bash
ping -c 2 10.10.16.61       # Arty
ping -c 2 192.168.100.2     # Jetson
```

## 2. Arty 서버 기동

작업 PC에서 RPi를 경유해 접속한다.

```bash
ssh -J ubuntu@10.10.16.200 petalinux@10.10.16.61
```

필요할 때만 golden 검증을 먼저 실행한다.

```sh
sudo ps_db_golden_test ~/arty_deploy_v2/model
```

분류 서버와 UART 안전 상태 송신을 시작한다.

```sh
sudo sh -c 'ADAS_UART_PORT=/dev/ttyPS1 ADAS_TCP_NODELAY=1 nohup ps_classifier_server "*" 5000 /home/petalinux/arty_deploy_v2/model 6 1 1467099144 38 1160501223 35 1422046702 38 8.540366656652573e-06 > /home/petalinux/server.log 2>&1 &'
```

포트 5000을 확인한다.

```sh
grep -a ':1388' /proc/net/tcp && echo 'Arty LISTEN OK'
```

## 3. Jetson 기동

작업 PC에서 다음 명령을 실행한다. `192.168.100.2`에 직접 SSH하는 명령이
아니며, 앞의 `-J ubuntu@10.10.16.200`이 RPi를 경유하도록 만든다.

```bash
ssh -J ubuntu@10.10.16.200 jetson@192.168.100.2
```

```bash
setsid nohup ~/start_adas.sh > /tmp/jetson.log 2>&1 < /dev/null &
```

```bash
pgrep -af jetson_roi_client
grep tcp_nodelay /tmp/jetson.log
grep -o 'rtt_us=[0-9]*' /tmp/jetson.log | tail -3
```

`tcp_nodelay=on`이고 ROI 왕복시간이 대략 8~11 ms면 정상이다. Jetson MJPEG는
RPi의 포트 전달을 통해 `http://10.10.16.200:8080`에서 확인한다.

## 4. TurtleBot 기동

RPi에서 실행한다.

```bash
~/start_robot.sh
```

```bash
ros2 topic echo /adas/safety_state --field data
```

상태가 장면에 따라 `0=CLEAR`, `1=SLOW`, `2=STOP`으로 바뀌는지 확인한다.
통합 화면은 다음 주소에서 확인한다.

```text
http://10.10.16.200:8090
```

## 5. 전체 종료

### 5.1 Jetson 먼저 종료

작업 PC에서 실행한다. `SIGINT`를 사용해야 측정 요약이 로그에 남는다.

```bash
ssh -J ubuntu@10.10.16.200 jetson@192.168.100.2 \
  "pkill -INT -f '[j]etson_roi_client'"
```

### 5.2 Arty 서버 종료

```bash
ssh -J ubuntu@10.10.16.200 petalinux@10.10.16.61
```

먼저 Jetson 연결 종료로 생성된 세션 요약을 확인한다.

```sh
sudo sed -n '/=== session summary ===/,/=====/p' \
  /home/petalinux/server.log | tail -14
```

그다음 서버를 종료하고 프로세스와 포트를 모두 확인한다.

```sh
PID=$(ps | grep -a ps_classifier_server | grep -av grep | awk '{print $1}' | head -1)
[ -n "$PID" ] && sudo kill -TERM "$PID"

ps | grep -a ps_classifier_server | grep -av grep || echo 'Arty process OFF'
grep -a ':1388' /proc/net/tcp || echo 'Arty port 5000 OFF'
```

### 5.3 TurtleBot 마지막 종료

RPi에서 실행한다.

```bash
~/stop_robot.sh
```

이 스크립트는 정지 명령, 모터 토크 OFF, ROS 노드와 통합 UI 종료를 수행한다.

## 문제가 생기면

상세 진단과 재배포 절차는
[`../turtlebot/docs/SERVER_START_STOP.md`](../turtlebot/docs/SERVER_START_STOP.md)를
참고한다.
