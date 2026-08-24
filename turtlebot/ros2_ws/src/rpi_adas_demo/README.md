# rpi_adas_demo

TurtleBot Raspberry Pi에서 Arty의 안전 상태를 수신하고 수동 조종 명령을
중재하는 ROS 2 패키지다. 객체 탐지와 영상 스트리밍은 Jetson이 담당하며,
RPi에서는 추론하지 않는다.

## Data flow

```text
Arty UART1
  -> uart_safety_receiver
  -> /adas/safety_state
  -> cmd_vel_arbiter
  -> /cmd_vel
  -> TurtleBot3 OpenCR

Jetson MJPEG :8080
  -> RPi DNAT :8080
  -> browser <img>

RPi ui_server :8090
  -> safety state + velocity + Jetson MJPEG 통합 HTML
```

## Nodes

| 실행 파일 | 역할 |
|---|---|
| `uart_safety_receiver` | CRC-8 UART 프레임을 검증하고 안전 상태 발행 |
| `cmd_vel_arbiter` | `CLEAR/SLOW/STOP`에 따라 조종 명령 제한 |

통합 HTML은 ROS 패키지 밖의 `turtlebot/scripts/ui_server.py`가 제공한다.
영상은 RPi에서 디코딩하거나 재인코딩하지 않고 Jetson MJPEG를 직접 표시한다.

## Build

```bash
cd ~/ros2_ws
source /opt/ros/humble/setup.bash
colcon build --packages-select rpi_adas_demo
source install/setup.bash
```

개별 노드를 직접 실행하기보다 저장소의 `turtlebot/scripts/start_robot.sh`와
`stop_robot.sh`를 사용한다. 전체 기동·종료 절차는
[`../../../docs/SERVER_START_STOP.md`](../../../docs/SERVER_START_STOP.md)를 따른다.
