#!/bin/bash
# TurtleBot 쪽 ADAS 데모 일괄 기동 (RPi).
#
# 아티/젯슨은 이 스크립트가 건드리지 않는다. 순서는 아티 서버 -> 젯슨 ->
# 이 스크립트 순이어야 안전 상태가 처음부터 들어온다. 먼저 띄워도 되지만
# 그동안은 safety 가 stale 이라 arbiter 가 STOP 을 낸다.
# set -u 는 ROS setup.bash 와 충돌한다 (AMENT_TRACE_SETUP_FILES unbound)

source /opt/ros/humble/setup.bash
source /home/ubuntu/ros2_ws/install/setup.bash
export TURTLEBOT3_MODEL=burger
export LDS_MODEL=LDS-01

log() { echo "[$(date +%H:%M:%S)] $*"; }

start() {   # start <이름> <pgrep패턴> <명령...>
    local name="$1" pat="$2"; shift 2
    if pgrep -f "$pat" >/dev/null 2>&1; then
        log "$name: 이미 떠 있음 - 건너뜀"
        return
    fi
    log "$name 시작"
    setsid nohup "$@" > "/tmp/${name}.log" 2>&1 < /dev/null &
    disown
}

# 1) 모터 (OpenCR). 라이다는 물리적으로 떼어냈으므로 nolidar.
if ! ros2 node list 2>/dev/null | grep -q turtlebot3_node; then
    log "bringup 시작"
    setsid nohup ros2 launch turtlebot3_bringup robot_nolidar.launch.py \
        > /tmp/bringup.log 2>&1 < /dev/null &
    disown
    log "OpenCR 자이로 캘리브레이션 대기 (약 20초)"
    for _ in $(seq 40); do
        ros2 node list 2>/dev/null | grep -q turtlebot3_node && break
        sleep 1
    done
fi
ros2 node list 2>/dev/null | grep -q turtlebot3_node \
    && log "bringup OK" || log "bringup 실패 - /tmp/bringup.log 확인"

# 2) 모터 토크.
#    bringup 은 토크를 꺼진 채로 올린다. 이걸 안 하면 /cmd_vel 에 값이
#    실려도 바퀴가 안 돈다 (2026-08-20 에 이것 때문에 한참 헤맸다).
log "모터 토크 ON"
ros2 service call /motor_power std_srvs/srv/SetBool "{data: true}" 2>&1 | tail -1
sleep 1
echo -n "  torque = "
timeout 5 ros2 topic echo /sensor_state --once 2>/dev/null | grep torque || echo "확인 실패"

# 3) 아티에서 오는 안전 상태 UART 수신.
#    /dev/ttyS0 은 serial-getty 를 mask 해 뒀으므로 비어 있어야 한다.
start uart_safety_receiver "[u]art_safety_receiver" ros2 run rpi_adas_demo uart_safety_receiver \
    --ros-args -p port:=/dev/ttyS0 -p baudrate:=115200

# 4) 조종 입력 + 안전 중재.
start joy_node "[j]oy_node" ros2 run joy joy_node \
    --ros-args -p deadzone:=0.05 -p autorepeat_rate:=0.0
start cmd_vel_arbiter "[c]md_vel_arbiter" ros2 run rpi_adas_demo cmd_vel_arbiter \
    --ros-args -p no_joy_mode:=true
start button_teleop "[b]utton_teleop" python3 /home/ubuntu/button_teleop.py

sleep 3
log "노드 목록:"
ros2 node list 2>/dev/null | sed 's/^/  /'
log "완료. 안전 상태 확인: ros2 topic echo /adas/safety_state"
