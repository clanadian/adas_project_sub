#!/usr/bin/env python3
import rclpy
from rclpy.node import Node
from sensor_msgs.msg import Joy
from geometry_msgs.msg import Twist

# 버튼 번호 (측정값): A=0, B=1, X=2, Y=3
A, B, X, Y = 0, 1, 2, 3
# 왼쪽 스틱 축: 0=좌우, 1=상하
AX_LR, AX_FB = 0, 1
# 스틱 부호. 전진으로 밀면 로봇이 앞으로 가도록 맞춘 값이다.
#
# 2026-08-24: axes[1] 이 전진에서 양수로 나온다는 이유로 +1.0 으로 되돌렸다가
# 앞뒤가 다시 반대가 됐다. 축 부호는 조이스틱 규약일 뿐이고 바퀴가 어느 쪽으로
# 도는지와는 별개다. 실제 주행으로 확인된 값은 -1.0 이므로 되돌린다.
#
# 근본 원인은 모터 방향 설정에 있을 가능성이 크지만, 그건 OpenCR 쪽을 만져야
# 하는 별개 작업이다. 그 전까지 여기서 보정한다. 대신 /cmd_vel 과 /odom 의
# 부호가 서로 어긋나 있을 수 있으니, UI 표시가 이상하면 UI 쪽에서 맞춘다.
SIGN_FB = -1.0
SIGN_LR = +1.0

LIN = 0.1        # 선속도 [m/s]
ANG = 0.5        # 각속도 [rad/s]
DEADZONE = 0.15  # 스틱 미세 흔들림 무시

class ButtonTeleop(Node):
    def __init__(self):
        super().__init__('button_teleop')
        self.buttons = []
        self.axes = []
        self.create_subscription(Joy, '/joy', self.cb, 10)
        self.pub = self.create_publisher(Twist, '/cmd_vel_manual', 10)
        self.create_timer(0.05, self.publish)

    def cb(self, msg):
        self.buttons = list(msg.buttons)
        self.axes = list(msg.axes)

    def publish(self):
        b, a = self.buttons, self.axes
        t = Twist()

        lr = a[AX_LR] if len(a) > AX_LR else 0.0
        fb = a[AX_FB] if len(a) > AX_FB else 0.0

        if abs(fb) > DEADZONE or abs(lr) > DEADZONE:
            # 1) 왼쪽 스틱 (밀면 우선)
            t.linear.x = LIN * SIGN_FB * fb      # 위 = 앞으로
            t.angular.z = ANG * SIGN_LR * lr     # 왼쪽 = 좌회전
        elif len(b) > Y:
            # 2) 스틱 중립이면 버튼
            if b[Y]:   t.linear.x = LIN     # Y 앞으로
            elif b[A]: t.linear.x = -LIN    # A 뒤로
            if b[X]:   t.angular.z = ANG    # X 좌회전
            elif b[B]: t.angular.z = -ANG   # B 우회전

        self.pub.publish(t)

def main():
    rclpy.init()
    node = ButtonTeleop()
    try:
        rclpy.spin(node)
    except KeyboardInterrupt:
        pass
    finally:
        node.destroy_node()
        rclpy.shutdown()

if __name__ == '__main__':
    main()
