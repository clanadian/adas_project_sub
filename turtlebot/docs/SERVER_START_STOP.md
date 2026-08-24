# 서버 기동 · 종료 절차

작성 2026-08-21 · 대상 Arty Z7-20 + Jetson Nano + TurtleBot(RPi)
· 실제로 이 순서대로 올리고 내려서 검증함

---

## 0. 요약 — 순서가 반대다

| | 순서 | 이유 |
|---|---|---|
| **기동** | Arty → 젯슨 → RPi | 젯슨은 접속 실패 시 **재시도 없이 즉시 종료**한다 |
| **종료** | 젯슨 → Arty → RPi | 젯슨을 먼저 끊어야 Arty가 **측정 요약을 파일에 남긴다** |

종료 순서는 특히 중요하다. Arty 서버에는 **시그널 핸들러가 없다.** 세션 요약은
`SIGINT` 때가 아니라 **클라이언트 연결이 끊길 때** 출력되고 거기서 `fflush` 된다
(`ps_classifier_server.c` 의 `print_session_summary` 호출 위치). 서버를 먼저
죽이면 그 실행의 측정치가 **통째로 사라진다.**

---

## 1. 기동

### 1.1 Arty — 분류 서버

```bash
ssh petalinux@10.10.16.61
```

> 계정은 `root` 가 아니라 **`petalinux`** 다. root 로 붙으면 키가 멀쩡해도
> `Permission denied` 가 난다.

먼저 가속기가 정상인지 본다. 30초면 끝난다.

```sh
sudo ps_db_golden_test ~/arty_deploy_v2/model
```

```
PASS: 9216 bytes bit-exact, accelerator time=6597 us
```

`PASS` 와 6,500~6,700 µs 가 나와야 한다. 여기서 틀리면 서버를 띄워도 의미가 없다.

서버를 올린다. **환경변수 두 개를 반드시 붙인다.**

```sh
sudo sh -c 'ADAS_UART_PORT=/dev/ttyPS1 ADAS_TCP_NODELAY=1 nohup ps_classifier_server "*" 5000 /home/petalinux/arty_deploy_v2/model 6 1 1467099144 38 1160501223 35 1422046702 38 8.540366656652573e-06 > /home/petalinux/server.log 2>&1 &'
```

| 환경변수 | 빼면 어떻게 되나 |
|---|---|
| `ADAS_UART_PORT=/dev/ttyPS1` | **안전 프레임을 한 개도 안 보낸다.** 조용히 분류만 한다 — 로봇은 계속 STOP |
| `ADAS_TCP_NODELAY=1` | ROI 왕복이 9.7 ms → **51.6 ms** 로 5배 느려진다 (이유는 `PS_TCP_RESPONSE_FIX.md`) |

확인한다. **`server.log` 가 비어 있어도 정상이다** — 파일로 리다이렉트하면
블록 버퍼링이라 배너가 한동안 안 찍힌다. 로그 말고 포트를 본다.

```sh
grep -a ':1388' /proc/net/tcp && echo LISTEN
```

`0x1388` = 10진수 5000 이다.

### 1.2 젯슨 — ROI 클라이언트

```bash
ssh -J ubuntu@10.10.16.200 jetson@192.168.100.2
```

> 계정은 `nvidia`가 아니라 **`jetson`**이며 RPi를 경유해야 한다.

```bash
setsid nohup ~/start_adas.sh > /tmp/jetson.log 2>&1 < /dev/null &
```

확인:

```bash
grep tcp_nodelay /tmp/jetson.log          # tcp_nodelay=on 이어야 한다
grep -o 'rtt_us=[0-9]*' /tmp/jetson.log | tail -3
```

`rtt_us` 가 **9000~11000** 이면 정상이다. **50000 대가 나오면** Arty 를
`ADAS_TCP_NODELAY=1` 없이 띄운 것이다 — 내리고 다시 올린다.

### 1.3 RPi — ROS 일체

```bash
~/start_robot.sh
```

bringup → **모터 토크 ON** → UART 수신 → joy → arbiter → teleop 순으로 올린다.
이미 떠 있는 것은 건너뛴다.

> bringup 은 매번 토크가 **꺼진 채로** 올라온다. 이걸 안 켜면 `/cmd_vel` 에
> 값이 실려도 바퀴가 안 돈다. 스크립트가 대신 해 준다.

### 1.4 전체 확인

```bash
ros2 topic echo /adas/safety_state --field data
```

CLEAR(0)/SLOW(1)/STOP(2)가 장면에 따라 바뀌면 끝까지 살아 있는 것이다.
계속 `2` 만 나오면 젯슨이나 Arty 가 안 떠 있는 것이다.

---

## 2. 종료

### 2.1 젯슨 — 먼저

```bash
ssh -J ubuntu@10.10.16.200 jetson@192.168.100.2 "pkill -INT -f '[j]etson_roi_client'"
```

`-INT` 로 보내야 측정 요약이 나온다. `-KILL` 은 쓰지 않는다.

`[j]` 대괄호는 오타가 아니다. `pkill -f jetson_roi_client` 라고 쓰면 **그 패턴이
자기 명령줄에도 들어 있어서 자기 자신을 죽인다.** 대괄호를 넣으면 정규식은
같은 문자열에 매칭되지만 자기 명령줄에는 매칭되지 않는다.

몇 초 뒤 요약이 나온다:

```
=== Jetson 파이프라인 측정 요약 ===
  완료 프레임    44507   ->  25.65 FPS
  분류한 ROI     47102   ->  27.14 ROI/s
  TCP round-trip (per ROI)   median 9.7 ms
```

### 2.2 Arty — 그 다음

젯슨이 끊긴 시점에 서버가 세션 요약을 파일에 남긴다. 먼저 그것부터 확인한다.

```sh
sudo sed -n '/=== session summary ===/,/=====/p' /home/petalinux/server.log | tail -14
```

```
  requests       47218 (ok 47218, error 0)
  pl_run         mean 6.615 ms      ← 가속기
  server total   mean 7.711 ms
```

그 다음 서버를 내린다. **PID 로 죽인다.**

```sh
PID=$(ps | grep -a ps_classifier_server | grep -av grep | awk '{print $1}')
sudo kill -TERM $PID
```

> Arty 는 busybox 라 **`ps -o pid,args` 를 지원하지 않는다.** 이 문법을 쓰면
> 아무것도 안 나와서 "서버가 없다"고 오판하고, 다시 띄우면
> `Address already in use` 가 난다. 위의 `ps | grep -a` 형태를 쓴다.

**완전히 꺼졌는지 반드시 두 가지를 다 본다.**

```sh
ps | grep -a ps_classifier_server | grep -av grep || echo "프로세스 없음 OK"
grep -a ':1388' /proc/net/tcp || echo "포트 5000 해제됨 OK"
```

프로세스가 사라져도 포트가 남아 있으면 다음 기동이 실패한다. 둘 다 확인한다.
`TERM` 으로 안 죽으면 `sudo kill -KILL $PID` 를 쓴다.

편의를 위해 `/tmp/arty_stop.sh` 에 위 과정을 넣어 뒀다(재부팅하면 사라진다).

### 2.3 RPi — 마지막

```bash
~/stop_robot.sh
```

순서대로 한다:

1. `button_teleop` 정지 — 조종 입력을 먼저 끊어야 이후 단계에서 로봇이 못 움직인다
2. `/cmd_vel` 에 **0 을 명시적으로 발행** — `turtlebot3_node` 가 마지막 명령을 유지할 수 있어서다
3. **모터 토크 OFF** — 여기까지 해야 "완전히 껐다"고 할 수 있다
4. arbiter / uart 수신 / joy 정지
5. bringup 정지
6. 확인 (ROS 노드 · 프로세스 · `/dev/ttyS0` 점유)

### 2.4 다 꺼졌는지 최종 확인

RPi 에서 UART 를 직접 읽어 본다. **0 바이트여야 한다.**

```bash
python3 ~/rawuart.py
```

서버가 살아 있으면 49.7 Hz 로 프레임이 쏟아지고, 내렸으면 0 이다. 이게
가장 확실한 "정말 꺼졌나" 판정이다.

---

## 3. 자주 걸리는 함정

| 증상 | 진짜 원인 |
|---|---|
| `ssh root@10.10.16.61` 거부 | 계정이 `petalinux` 다 |
| `Address already in use` | 이미 떠 있다. busybox `ps` 문법 때문에 못 본 것 |
| `Stopped (SIGTTIN)` | `sudo ... &` 로 백그라운드 기동함. **스크립트 파일**로 만들어 실행할 것 |
| `server.log` 가 비었다 | 실패가 아니라 **버퍼링**. 포트로 확인할 것 |
| 젯슨이 바로 죽는다 | Arty 서버가 아직 안 떴다. 재시도하지 않는다 |
| `rtt_us=51xxx` | Arty 를 `ADAS_TCP_NODELAY=1` 없이 띄웠다 |
| 계속 STOP 만 나온다 | ① 젯슨 정지 ② `ADAS_UART_PORT` 누락 ③ 둘 다 |
| `/cmd_vel` 은 있는데 안 움직인다 | **모터 토크가 꺼져 있다.** `ros2 topic echo /sensor_state --once \| grep torque` |
| pkill 했더니 SSH 가 끊긴다 | 패턴이 자기 명령줄에 매칭됐다. `[j]etson...` 형태나 스크립트 파일을 쓸 것 |
| 노드를 죽였는데 `ros2 node list` 에 남아 있다 | DDS 탐색 캐시 지연. 5~6초 기다렸다 다시 조회 |

---

## 4. 스크립트 위치

| 파일 | 위치 | 재부팅 후 |
|---|---|---|
| `start_robot.sh` | RPi `~/` | 남음 |
| `stop_robot.sh` | RPi `~/` | 남음 |
| `rawuart.py` | RPi `~/` | 남음 |
| `start_adas.sh` | 젯슨 `~/` | 남음 |
| `arty_stop.sh` | Arty `/tmp/` | **사라짐** |

Arty 는 SD 카드 ext4 rootfs 라 홈 디렉터리는 재부팅해도 남는다
(저장소 문서의 "initramfs 라 다 사라진다"는 설명은 지금 이미지와 맞지 않는다).
`/tmp` 만 휘발된다.
