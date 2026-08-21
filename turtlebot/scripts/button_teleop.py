#!/usr/bin/env python3
import rclpy
from rclpy.node import Node
from sensor_msgs.msg import Joy
from geometry_msgs.msg import Twist

# 버튼 번호 (측정값): A=0, B=1, X=2, Y=3
A, B, X, Y = 0, 1, 2, 3
# 왼쪽 스틱 축: 0=좌우, 1=상하
AX_LR, AX_FB = 0, 1
# 스틱 부호 보정. 이 패드는 위로 밀면 axes[1]이 음수로 나와,
# 그대로 쓰면 앞뒤가 반대로 간다 (실측 2026-08-20).
# 좌우는 정상이라 그대로 둔다.
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
