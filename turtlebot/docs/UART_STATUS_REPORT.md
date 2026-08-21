# UART 점검 보고서 — "설정이 덜 되어 있다"는 진단에 대하여

점검 2026-08-21 12:00~12:10 · 점검자 Claude · 대상 Arty Z7-20 ↔ TurtleBot(RPi)

---

## 0. 결론

**UART는 정상 동작한다. 설정이 덜 된 곳은 없다.**

원시 바이트를 직접 떠서 확인했다. 3초에 447바이트, **유효 프레임 149개,
버린 바이트 0개, 49.7 Hz.** 모든 바이트가 CRC까지 맞는 정상 프레임이었다.

외부 PC 쪽에서 나온 두 진단은 **둘 다 사실이 아니다.**

| 그쪽 진단 | 실제 |
|---|---|
| "Arty UART 설정이 안 돼 있다" | 아니다. `/dev/ttyPS1` 로 49.7 Hz 송신 중 |
| "터틀봇 UART 설정이 안 돼서 서버를 켤 수 없다" | 아니다. 그리고 **인과가 성립하지 않는다** — RPi UART 설정은 Arty 서버 기동과 아무 상관이 없다 |

서버가 안 켜졌던 진짜 원인은 §5 에 정리했다. UART 와 무관한 함정들이다.

---

## 1. 실측 증거

### 1.1 원시 바이트 (RPi `/dev/ttyS0` 직접 read, 수신 노드 정지 상태)

```
수신 바이트 수: 447
앞 60바이트 : a5 02 57 a5 02 57 a5 02 57 a5 02 57 a5 02 57 ...
유효 프레임 : 149  / 버린 바이트: 0
프레임률    : 49.7 Hz
  state=2 STOP   149회
```

**버린 바이트 0개**가 핵심이다. 수신 루프는 매직(`0xA5`)이 아니거나 CRC 가
틀리면 1바이트씩 버리고 다시 찾는다. 0개라는 것은 **한 바이트도 어긋나지
않았다**는 뜻이다. 보율·프레이밍·배선·GND 가 전부 맞아야 나오는 결과다.

이때 STOP 인 이유는 젯슨이 안 떠 있어서다. 판단 근거가 없으면 Stop —
설계대로다. 고장이 아니다.

### 1.2 프레임 형식

`[0xA5][state][CRC-8/ATM]`, 115200 8N1. CRC 는 poly 0x07, init 0x00,
xorout 0x00, 반전 없음. 앞 2바이트에 대해 계산한다.

| 상태 | 바이트 |
|---|---|
| CLEAR | `a5 00 59` |
| SLOW | `a5 01 5e` |
| STOP | `a5 02 57` |

### 1.3 End-to-end — 값이 실제로 "살아 있다"는 증거

`/adas/safety_state` 의 **Publisher count 는 1** 이다. 즉 이 토픽에 값을
넣는 것은 `uart_safety_receiver` 하나뿐이고, 이 노드는 **유효 CRC 프레임이
올 때만** publish 한다. 기본값도, 자체 heartbeat 도 없다
(`rpi_adas_demo/uart_safety_receiver.py` 의 `_poll()`).

그리고 파이프라인 상태에 따라 값이 **바뀌었다.**

| 시점 | 10초간 분포 |
|---|---|
| 젯슨 정지 | STOP 100 % |
| 젯슨 기동 후 | **CLEAR 159 / SLOW 161** |

SLOW(state=1)까지 나온 것이 결정적이다. 고정값이나 우연이 아니라 Arty 가
장면을 보고 판단한 결과가 그대로 넘어오고 있다는 뜻이다.

---

## 2. 실제 UART 구성

| | |
|---|---|
| 송신 | Arty PS `/dev/ttyPS1` (PMOD JA 의 UART1) |
| 수신 | RPi `/dev/ttyS0` (= `/dev/serial0`, GPIO14/15) |
| 보율 | 115200 8N1 |
| 방향 | **단방향** — Arty → RPi 만. RPi 는 아무것도 안 보낸다 |
| 켜는 법 | 서버에 `ADAS_UART_PORT=/dev/ttyPS1` 환경변수. **없으면 UART 를 아예 안 연다** |

마지막 줄이 중요하다. 서버는 `ADAS_UART_PORT` 가 없으면 조용히 분류만 하고
안전 프레임을 한 개도 안 보낸다. **"Arty UART 설정이 안 돼 있다"는 진단은
아마 이 환경변수를 빼고 서버를 띄운 상태를 본 것**일 가능성이 높다. 설정이
없는 게 아니라 **안 켠 것**이다.

---

## 3. 오늘(2026-08-21) 바뀐 것

### 3.1 RPi `cmdline.txt` — `console=serial0,115200` 제거 (11:57, 팀원)

```
전: console=serial0,115200 multipath=off ... console=tty1 ...
후:                        multipath=off ... console=tty1 ...
```

**좋은 수정이다.** 커널 콘솔이 UART 를 쓰고 있으면 그 포트가 깨끗하지 않다.
`enable_uart=1` 은 그대로라 `/dev/ttyS0`, `/dev/serial0` 는 정상 존재한다.

이 변경 때문에 RPi 가 11:57:41 에 재부팅됐고, 그 시점에 돌던 ROS 노드가
전부 죽었다. **"UART 설정이 안 되어 있다"고 보였다면 이 재부팅 직후를 본
것일 수 있다** — 설정 문제가 아니라 노드가 안 떠 있던 것이다.

### 3.2 RPi `serial-getty@ttyS0` — mask (어제, 나)

`console=serial0` 이 있으면 systemd 가 매 부팅마다 getty 를 자동 생성해
**들어오는 안전 프레임을 먹어버린다**(실제로 겪었다 — `c2c2c2...` 만 보였다).
mask 로 영구 차단해 뒀다. 3.1 로 근본 원인이 사라졌으니 지금은 이중 안전장치다.

> 부작용: RPi 의 시리얼 콘솔 로그인이 안 된다. 되돌리려면
> `sudo systemctl unmask serial-getty@ttyS0.service`

### 3.3 Arty 서버 코드 교체 (오늘, 팀원)

교체 자체는 확인했다. 다만 **응답 write 합치기(조치 B)는 아직 안 들어갔다** — §6 참고.

---

## 4. 성능 — 정상 범위

Arty 서버가 남긴 직전 세션 요약 (61,710 requests, **errors 0**):

```
  preprocess     mean 0.509 ms
  pl_run         mean 6.611 ms      ← 가속기
  postprocess    mean 0.583 ms
  server total   mean 7.707 ms
```

젯슨이 잰 왕복은 **9.0~9.2 ms**. 서버가 7.7 ms 를 쓰니 네트워크·시스템콜이
1.3~1.5 ms — 정상이다.

golden 검증도 통과했다: **9216 bytes bit-exact, accelerator 6,597 µs.**

참고로 어제까지는 이 왕복이 **51.6 ms** 였다. 44 ms 가 TCP 대기였고,
그 원인과 수정은 [`PS_TCP_RESPONSE_FIX.md`](PS_TCP_RESPONSE_FIX.md) 에 있다.

---

## 5. 서버가 안 켜졌던 진짜 원인 — 함정 목록

UART 와 무관하다. 실제로 오늘 다 겪은 것들이다.

### 5.1 SSH 계정이 `root` 가 아니라 **`petalinux`**

```bash
ssh petalinux@10.10.16.61      # O
ssh root@10.10.16.61           # X — Permission denied
```

`root` 로 시도하면 키가 멀쩡해도 막힌다. 나도 어제 이것 때문에 "SSH 불가"로
잘못 판단했다.

### 5.2 `sudo ... &` 로 백그라운드 기동 → `Stopped (SIGTTIN)`

sudo 가 비밀번호를 터미널에서 읽으려다 그 자리에서 멈춘다. 화면에
`Password:` 가 떠도 이미 멈춘 프로세스라 입력이 안 먹는다. **스크립트 파일로
만들어 실행**하는 쪽이 안전하다.

### 5.3 "Address already in use" — 이미 떠 있는데 못 본 것

Arty 는 busybox 라 **`ps -o pid,args` 를 지원하지 않는다.** 이 문법을 쓰면
아무것도 안 나와서 "서버가 안 떠 있다"고 오판하게 된다.

```sh
ps | grep -a classifier_server | grep -av grep    # O
ps -o pid,args | grep classifier                  # X — busybox 는 -o 무시
grep -a ':1388' /proc/net/tcp                     # 포트 5000 리스닝 직접 확인
```

`timeout`, `head -c`, `base64` 도 없다. 파일 전송은 `scp` 를 쓴다.

### 5.4 `server.log` 가 비어 보인다 → 안 뜬 게 아니라 **버퍼링**

stdout 을 파일로 리다이렉트하면 블록 버퍼링이라 기동 배너가 한동안 안
찍힌다. 로그가 비었다고 실패로 판단하면 안 된다. 리스닝 포트로 확인할 것.

### 5.5 젯슨 클라이언트는 **재시도하지 않는다**

첫 접속에 실패하면 `failed to connect to PS server` 를 찍고 즉시 종료한다.
**반드시 Arty 서버를 먼저 띄운다.**

---

## 6. 아직 남은 것 — 응답 write 합치기

오늘 패킷을 다시 떠 봤다. 응답이 여전히 **20 B + 12 B 두 패킷**이다.

```
0.007785  아티 → 젯슨  length 20   응답 헤더
0.000000  아티 → 젯슨  length 12   응답 본문   ← 간격 0. NODELAY 가 막아주고 있다
```

간격이 0 이라 지금은 문제가 없다. 하지만 그건 **`ADAS_TCP_NODELAY=1` 을
켰기 때문**이다. 이 환경변수를 빼고 띄우는 순간 40 ms 가 그대로 돌아온다.

`PS_TCP_RESPONSE_FIX.md` §4 의 수정(헤더+본문을 32바이트 한 버퍼로 묶어
`send_all` 한 번)을 넣으면 환경변수와 무관해진다. `ps_eb` 도 같은 구조다.

**당분간은 서버를 띄울 때 `ADAS_TCP_NODELAY=1` 을 반드시 붙여야 한다.**

---

## 7. 전체 기동 절차 (순서 지킬 것)

### 7.1 Arty — 서버

```bash
ssh petalinux@10.10.16.61
```

```sh
sudo ps_db_golden_test ~/arty_deploy_v2/model
```

`PASS: 9216 bytes bit-exact` 가 나와야 한다. 그 다음 서버:

```sh
sudo sh -c 'ADAS_UART_PORT=/dev/ttyPS1 ADAS_TCP_NODELAY=1 nohup ps_classifier_server "*" 5000 /home/petalinux/arty_deploy_v2/model 6 1 1467099144 38 1160501223 35 1422046702 38 8.540366656652573e-06 > /home/petalinux/server.log 2>&1 &'
```

확인 — 로그가 비어 있어도 이게 나오면 정상이다:

```sh
grep -a ':1388' /proc/net/tcp && echo LISTEN
```

### 7.2 젯슨 — 클라이언트

```bash
ssh -J ubuntu@10.10.16.200 jetson@192.168.100.2
```

```bash
setsid nohup ~/start_adas.sh > /tmp/jetson.log 2>&1 < /dev/null &
```

`tcp_nodelay=on` 과 `rtt_us=9xxx` 를 확인한다. 50000 대가 나오면 §6 문제다.

### 7.3 RPi — ROS 일체

```bash
~/start_robot.sh
```

bringup → **모터 토크 ON** → UART 수신 → joy → arbiter → teleop 을 순서대로
띄운다. 이미 떠 있는 것은 건너뛴다.

> bringup 은 매번 토크가 **꺼진 채로** 올라온다. 이걸 안 하면 `/cmd_vel` 에
> 값이 실려도 바퀴가 안 돈다. 스크립트가 대신 해 준다.

---

## 8. 확인 명령 모음

| 무엇 | 명령 |
|---|---|
| UART 원시 바이트 | RPi 에서 수신 노드 정지 후 `python3 ~/rawuart.py` |
| 안전 상태 | `ros2 topic echo /adas/safety_state --field data` |
| 발행자가 UART 뿐인지 | `ros2 topic info /adas/safety_state` → Publisher count 1 |
| 모터 토크 | `ros2 topic echo /sensor_state --once \| grep torque` |
| Arty 서버 | `ps \| grep -a classifier_server \| grep -av grep` |
| 5000 리스닝 | `grep -a ':1388' /proc/net/tcp` |
| 응답 패킷 모양 | RPi 에서 `sudo tcpdump -i enx00e04c680398 -nn -ttt 'tcp port 5000'` |

RPi 가 젯슨↔아티 사이를 라우팅하므로 **그 둘 사이 트래픽이 RPi 에서 다 잡힌다.**

---

## 9. 문서 정정 — Arty 는 initramfs 가 아니다

`docs/ARTY_NETWORK_SETUP.md` 와 `ARTY_SD_BOOT_USAGE.md` 는 `root=/dev/ram0`
(RAM 기반 rootfs)라 **재부팅하면 IP·파일이 전부 사라진다**고 적고 있다.
지금 보드는 그렇지 않다.

```
/proc/cmdline : console=ttyPS0,115200 earlycon root=/dev/mmcblk0p2 ro rootwait
mount         : /dev/root on / type ext4 (rw,relatime)
```

**SD 카드의 ext4 rootfs 다.** 그래서 `~/arty_deploy_v2`, SSH 키,
`/etc/sudoers.d` 설정이 전원을 껐다 켜도 남아 있다. 실제로 오늘 확인했다.

나도 어제 이 문서를 근거로 "아티는 매 부팅마다 전부 다시 올려야 한다"고
안내했는데, **틀린 안내였다.** 문서가 실물보다 오래된 것이다. 두 문서를
고치는 것이 좋겠다.

---

## 10. 현재 상태 (12:10 기준)

| | |
|---|---|
| Arty 서버 | 실행 중 (PID 650), `ADAS_UART_PORT=/dev/ttyPS1` + `ADAS_TCP_NODELAY=1` |
| golden | PASS, bit-exact, 6,597 µs |
| 젯슨 | 실행 중 (PID 7861), RTT 9.0~9.2 ms, 26 FPS |
| RPi ROS | 7개 노드 전부, `torque: true`, 배터리 11.47 V |
| UART | **49.7 Hz, CRC 오류 0** |
| 안전 상태 | CLEAR / SLOW 전환 중 (정상 판단) |
| 조이스틱 | **미연결** — `/dev/input/js0` 없음. 배터리 교체 후 페어링 필요 |
