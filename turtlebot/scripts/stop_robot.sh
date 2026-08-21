#!/bin/bash
# TurtleBot 쪽 ADAS 데모 일괄 정지 (RPi).
#
# 이 스크립트는 아티/젯슨을 건드리지 않는다. 전체를 내릴 때의 순서는
#   1) 젯슨 클라이언트   2) 아티 서버   3) 이 스크립트
# 다. 젯슨을 먼저 끊어야 아티 서버가 세션 요약을 파일에 flush 한다
# (서버에는 시그널 핸들러가 없어서, 요약은 "클라이언트가 끊길 때"만 남는다).
#
# pkill 패턴을 명령줄에 직접 쓰지 않고 스크립트 파일로 두는 이유:
# 패턴이 호출한 셸의 명령줄에도 들어가서 자기 자신을 죽이기 때문이다.

source /opt/ros/humble/setup.bash
source /home/ubuntu/ros2_ws/install/setup.bash

PATTERN='turtlebot3_ros|robot_nolidar|robot_state_publisher|cmd_vel_arbiter|uart_safety_receiver|joy_node|button_teleop'

log() { echo "[$(date +%H:%M:%S)] $*"; }

# 1) 조종 입력을 먼저 끊는다. 이게 없어야 이후 단계에서 로봇이 못 움직인다.
log "button_teleop 정지"
pkill -f button_teleop 2>/dev/null
sleep 1

# 2) 정지 명령을 명시적으로 넣는다.
#    turtlebot3_node 는 마지막 명령을 유지할 수 있으므로, 노드를 죽이기 전에
#    0 을 확실히 실어 준다.
log "정지 명령 발행"
timeout 3 ros2 topic pub -1 /cmd_vel geometry_msgs/msg/Twist \
    '{linear: {x: 0.0}, angular: {z: 0.0}}' >/dev/null 2>&1

# 3) 모터 토크 OFF. 여기까지 해야 "완전히 껐다"고 할 수 있다.
log "모터 토크 OFF"
timeout 10 ros2 service call /motor_power std_srvs/srv/SetBool \
    "{data: false}" 2>&1 | grep -E 'success|response' | tail -1

# 4) 나머지 노드.
for name in cmd_vel_arbiter uart_safety_receiver joy_node; do
    log "$name 정지"
    pkill -f "$name" 2>/dev/null
done
sleep 1

# 5) 모터 bringup (launch 는 자식까지 같이 내려간다).
log "turtlebot3 bringup 정지"
pkill -f robot_nolidar 2>/dev/null
pkill -f turtlebot3_ros 2>/dev/null
pkill -f robot_state_publisher 2>/dev/null

# DDS 탐색 캐시가 정리될 때까지 기다린다. 짧으면 이미 죽은 노드가
# `ros2 node list` 에 남아 보여서 "안 죽었다"고 오판하게 된다.
sleep 6

# 6) 확인.
log "확인:"
left=$(ros2 node list 2>/dev/null | grep -v '^$')
if [ -z "$left" ]; then
    echo "  ROS 노드     : 없음 OK"
else
    echo "  !! 아직 남은 노드:"; echo "$left" | sed 's/^/    /'
fi

# avahi 가 'turtlebot3.local' 을 광고하고 있어서 turtlebot3 패턴에 걸린다.
# 프로세스가 아니라 광고 문자열이므로 제외한다.
leftp=$(pgrep -af "$PATTERN" | grep -v avahi)
if [ -z "$leftp" ]; then
    echo "  프로세스     : 없음 OK"
else
    echo "  !! 아직 남은 프로세스:"; echo "$leftp" | sed 's/^/    /'
fi

if sudo -n fuser /dev/ttyS0 >/dev/null 2>&1; then
    echo "  !! /dev/ttyS0 를 아직 누가 잡고 있다"
else
    echo "  /dev/ttyS0   : 해제됨 OK"
fi
log "완료"
