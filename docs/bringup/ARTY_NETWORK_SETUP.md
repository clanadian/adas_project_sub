# Arty Z7-20 현재 네트워크 구성

이 문서는 현재 조립된 DB 데모 장비의 네트워크 기준만 기록한다. 데모 기동 시
수동으로 IP나 route를 설정하지 않는다. 전체 기동 명령의 정본은
[`../THREE_BOARD_QUICK_START.md`](../THREE_BOARD_QUICK_START.md)다.

## 현재 주소와 접속 경로

```text
작업 PC
  └─ SSH → RPi              ubuntu@10.10.16.200
             ├─ jump → Arty petalinux@10.10.16.61
             └─ jump → Jetson jetson@192.168.100.2
```

Arty와 Jetson은 작업 PC에서 직접 접속하지 않는다. 항상 RPi를 점프 호스트로
사용한다.

```bash
ssh -J ubuntu@10.10.16.200 petalinux@10.10.16.61
ssh -J ubuntu@10.10.16.200 jetson@192.168.100.2
```

## Arty DB 고정 설정

| 항목 | 값 |
|---|---|
| 인터페이스 | `enx020000000020` |
| IPv4 | `10.10.16.61/24` |
| Linux 콘솔 | `/dev/ttyUSB1` |
| 설정 유지 | SD ext4 rootfs에 포함되어 재부팅 후에도 유지 |

현재 PetaLinux 이미지에 설정이 포함돼 있으므로 수동 네트워크 설정을 하지
않는다.

## 확인

먼저 작업 PC에서 RPi에 접속한 뒤 RPi에서 두 보드를 확인한다.

```bash
ssh ubuntu@10.10.16.200
ping -c 2 10.10.16.61
ping -c 2 192.168.100.2
```

응답이 없으면 IP를 변경하지 말고 전원, 케이블, RPi 부팅 여부부터 확인한다.

## 현재 웹 주소

| 기능 | 주소 |
|---|---|
| Jetson MJPEG | `http://10.10.16.200:8080` |
| 통합 UI | `http://10.10.16.200:8090` |

RPi가 라우팅과 포트 전달을 담당한다. RPi가 꺼져 있으면 Arty·Jetson SSH,
Jetson–Arty 통신과 외부 MJPEG 접근이 모두 동작하지 않는다.
