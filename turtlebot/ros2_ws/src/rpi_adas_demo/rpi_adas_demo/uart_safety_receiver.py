"""UART 3-byte safety frame receiver: [0xA5][state][CRC-8/ATM]."""

from __future__ import annotations

import time

import rclpy
from rclpy.node import Node
from std_msgs.msg import Int32

from rpi_adas_demo.config import SafetyState

try:
    import serial
except ImportError:  # Makes the error actionable if python3-serial is absent.
    serial = None

_MAGIC = 0xA5


def crc8_atm(data: bytes) -> int:
    """CRC-8/ATM: poly 0x07, init 0x00, xorout 0x00, no reflection."""
    crc = 0
    for value in data:
        crc ^= value
        for _ in range(8):
            crc = ((crc << 1) ^ 0x07) & 0xFF if crc & 0x80 else (crc << 1) & 0xFF
    return crc


class UartSafetyReceiver(Node):
    def __init__(self) -> None:
        super().__init__('uart_safety_receiver')
        self.declare_parameter('port', '/dev/ttyUSB0')
        self.declare_parameter('baudrate', 115200)
        self.declare_parameter('poll_hz', 100.0)

        if serial is None:
            raise RuntimeError('pyserial missing: sudo apt install python3-serial')
        port = str(self.get_parameter('port').value)
        baudrate = int(self.get_parameter('baudrate').value)
        try:
            self._serial = serial.Serial(port, baudrate, timeout=0)
        except serial.SerialException as exc:
            raise RuntimeError(f'Cannot open UART {port}: {exc}') from exc
        self._buffer = bytearray()
        self._last_valid = 0.0  # Kept only for diagnostics; arbiter owns timeout.
        self._pub = self.create_publisher(Int32, '/adas/safety_state', 10)
        self.create_timer(1.0 / float(self.get_parameter('poll_hz').value), self._poll)
        self.get_logger().info(f'UART safety receiver: {port} @ {baudrate}')

    def _poll(self) -> None:
        waiting = self._serial.in_waiting
        if waiting:
            self._buffer.extend(self._serial.read(waiting))
        # On any error, discard one byte and seek the next magic byte.  A bad
        # frame never updates last_valid, therefore the arbiter safely times out.
        while len(self._buffer) >= 3:
            if self._buffer[0] != _MAGIC:
                del self._buffer[0]
                continue
            state, received_crc = self._buffer[1], self._buffer[2]
            if crc8_atm(bytes(self._buffer[:2])) != received_crc:
                del self._buffer[0]
                continue
            del self._buffer[:3]
            safe_state = state if state in (0, 1, 2) else int(SafetyState.STOP)
            self._pub.publish(Int32(data=safe_state))
            self._last_valid = time.monotonic()

    def destroy_node(self) -> None:
        self._serial.close()
        super().destroy_node()


def main(args=None) -> None:
    rclpy.init(args=args)
    node = UartSafetyReceiver()
    try:
        rclpy.spin(node)
    except KeyboardInterrupt:
        pass
    finally:
        node.destroy_node()
        if rclpy.ok():
            rclpy.shutdown()


if __name__ == '__main__':
    main()
