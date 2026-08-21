#!/usr/bin/env python3
import rclpy
from rclpy.node import Node
from sensor_msgs.msg import Joy

class JoyCheck(Node):
    def __init__(self):
        super().__init__('joy_check')
        self.create_subscription(Joy, '/joy', self.cb, 10)
    def cb(self, msg):
        pressed = [i for i, v in enumerate(msg.buttons) if v]
        axes = [f'{i}:{v:+.2f}' for i, v in enumerate(msg.axes) if abs(v) > 0.3]
        if pressed or axes:
            print(f'buttons={pressed}   axes=[{", ".join(axes)}]', flush=True)

def main():
    rclpy.init()
    rclpy.spin(JoyCheck())

if __name__ == '__main__':
    main()
