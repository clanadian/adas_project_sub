"""
cmd_vel_arbiter  —  /joy + /adas/safety_state → /cmd_vel

Combines Xbox controller input with YOLO-derived safety state to produce
a safety-gated cmd_vel for TurtleBot3.  Implements the arbiter logic
described in TurtleBot_안전제어_UART수신이후.md.

Xbox One mapping (ros2 joy package, Linux evdev):
  Axes  [0]=LX  [1]=LY  [2]=LT  [3]=RX  [4]=RY  [5]=RT
  Buttons [0]=A [1]=B [2]=X [3]=Y [4]=LB [5]=RB [6]=Back [7]=Start

  LY (axis 1): forward/back    LX (axis 0): turn left/right
  Y/A        : forward/back    X/B         : turn left/right
  LB (btn 4) : deadman hold    Back (btn 6): software e-stop toggle

Safety rules
  1. No deadman held                  → zero twist
  2. Controller timeout               → zero twist
  3. /adas/safety_state stale/STOP   → zero twist  + arm resume gate
  4. SLOW                             → twist capped to slow limits
  5. CLEAR                            → full manual passthrough
  6. Resume gate (after any STOP):    stick must return to neutral, then
                                       deadman + new input to re-enable.

Parameters
  safety_timeout_sec  float  stale safety_state → STOP (default 2.0)
  joy_timeout_sec     float  no joy message → zero twist (default 1.0)
  publish_hz          float  cmd_vel publish rate (default 10.0)
  slow_linear_max     float  SLOW linear cap m/s (default 0.10)
  slow_angular_max    float  SLOW angular cap rad/s (default 0.8)
  linear_axis_sign    float  use -1.0 if forward/backward is reversed
  stick_deadzone      float  ignore small stick movement (default 0.12)
  cardinal_snap_ratio float  suppress perpendicular-axis wobble (default 0.45)
"""

from __future__ import annotations

import rclpy
from rclpy.node import Node
from geometry_msgs.msg import Twist
from sensor_msgs.msg import Joy
from std_msgs.msg import Int32

from rpi_adas_demo.config import SafetyState

# --- Xbox One axis / button indices (ros2 joy, Linux evdev) ---
_LINEAR_AXIS = 1     # Left stick Y  (+1 = forward)
_ANGULAR_AXIS = 0    # Left stick X  (+1 = turn left)
_DEADMAN_BTN = 4     # LB — must be held for any movement
_A_BTN = 0           # backward
_B_BTN = 1           # turn right
_X_BTN = 2           # turn left
_Y_BTN = 3           # forward
_ESTOP_BTN = 6       # Back/View — toggles software emergency stop

# --- TurtleBot3 Burger physical limits ---
_MAX_LINEAR = 0.22   # m/s
_MAX_ANGULAR = 2.84  # rad/s

# --- Speed caps for SLOW safety state ---
_SLOW_LINEAR = 0.10   # m/s
_SLOW_ANGULAR = 0.8   # rad/s

# Stick dead zone (in raw [-1, 1] axis space; joy_node also applies deadzone)
_NEUTRAL_THRESH = 0.05
_STICK_DEADZONE = 0.12
_CARDINAL_SNAP_RATIO = 0.45


class CmdVelArbiter(Node):
    def __init__(self) -> None:
        super().__init__('cmd_vel_arbiter')

        self.declare_parameter('safety_timeout_sec', 2.0)
        self.declare_parameter('joy_timeout_sec', 1.0)
        self.declare_parameter('publish_hz', 10.0)
        self.declare_parameter('slow_linear_max', _SLOW_LINEAR)
        self.declare_parameter('slow_angular_max', _SLOW_ANGULAR)
        self.declare_parameter('linear_axis_sign', -1.0)
        self.declare_parameter('stick_deadzone', _STICK_DEADZONE)
        self.declare_parameter('cardinal_snap_ratio', _CARDINAL_SNAP_RATIO)
        # no_joy_mode: skip deadman/joy check; subscribe /cmd_vel_manual instead
        self.declare_parameter('no_joy_mode', False)

        self._safety_timeout = float(self.get_parameter('safety_timeout_sec').value)
        self._joy_timeout = float(self.get_parameter('joy_timeout_sec').value)
        self._slow_lin = float(self.get_parameter('slow_linear_max').value)
        self._slow_ang = float(self.get_parameter('slow_angular_max').value)
        self._linear_axis_sign = float(self.get_parameter('linear_axis_sign').value)
        self._stick_deadzone = max(
            0.0, min(0.95, float(self.get_parameter('stick_deadzone').value)))
        self._cardinal_snap_ratio = max(
            0.0, min(1.0, float(
                self.get_parameter('cardinal_snap_ratio').value)))
        self._no_joy_mode = bool(self.get_parameter('no_joy_mode').value)

        self._safety: int = int(SafetyState.STOP)   # safe default before first message
        self._safety_stamp: float = 0.0

        self._joy: Joy | None = None
        self._joy_stamp: float = 0.0

        # Resume gate: armed after any STOP period ends
        self._require_resume: bool = False
        self._seen_neutral: bool = False
        self._prev_safety: int = int(SafetyState.STOP)

        # Software e-stop state (Back/View button toggle)
        self._estop: bool = False
        self._prev_estop_btn: bool = False

        self._sub_safety = self.create_subscription(
            Int32, '/adas/safety_state', self._on_safety, 10)
        self._sub_joy = self.create_subscription(
            Joy, '/joy', self._on_joy, 10)
        self._pub_cmd = self.create_publisher(Twist, '/cmd_vel', 10)

        # no_joy_mode: accept /cmd_vel_manual from teleop_twist_keyboard
        self._manual_twist = Twist()
        self._manual_stamp: float = 0.0
        if self._no_joy_mode:
            self._sub_manual = self.create_subscription(
                Twist, '/cmd_vel_manual', self._on_manual, 10)
            self.get_logger().warning(
                'no_joy_mode=True: deadman disabled, using /cmd_vel_manual. '
                'Run: ros2 run teleop_twist_keyboard teleop_twist_keyboard '
                '--ros-args --remap cmd_vel:=/cmd_vel_manual'
            )

        hz = float(self.get_parameter('publish_hz').value)
        self._timer = self.create_timer(1.0 / hz, self._timer_cb)
        self.get_logger().info(
            f'cmd_vel_arbiter ready  '
            f'safety_timeout={self._safety_timeout}s  '
            f'SLOW cap: lin={self._slow_lin} ang={self._slow_ang}  '
            f'stick_deadzone={self._stick_deadzone}  '
            f'cardinal_snap_ratio={self._cardinal_snap_ratio}'
        )

    # ------------------------------------------------------------------
    def _on_safety(self, msg: Int32) -> None:
        # A malformed publisher must never crash or make the robot move.
        self._safety = (msg.data if msg.data in [int(s) for s in SafetyState]
                        else int(SafetyState.STOP))
        self._safety_stamp = self._now()
        # Do not wait for the periodic arbiter tick on an emergency transition.
        # The timer still republishes zero while STOP remains active.
        if self._safety == int(SafetyState.STOP):
            self._arm_resume()
            self._pub_cmd.publish(Twist())

    def _on_joy(self, msg: Joy) -> None:
        self._joy = msg
        self._joy_stamp = self._now()

    def _on_manual(self, msg: Twist) -> None:
        self._manual_twist = msg
        self._manual_stamp = self._now()

    def _now(self) -> float:
        return self.get_clock().now().nanoseconds * 1e-9

    def _arm_resume(self) -> None:
        """Require neutral then a new command after any forced stop."""
        self._require_resume = True
        self._seen_neutral = False

    # ------------------------------------------------------------------
    def _timer_cb(self) -> None:
        now = self._now()
        zero = Twist()

        # --- no_joy_mode: bypass deadman, use /cmd_vel_manual directly ---
        if self._no_joy_mode:
            if (now - self._safety_stamp) > self._safety_timeout:
                self._arm_resume()
                self._pub_cmd.publish(zero)
                return
            safety = SafetyState(self._safety)
            if safety == SafetyState.STOP:
                self._arm_resume()
                self._pub_cmd.publish(zero)
                return
            # manual input timeout check
            if (now - self._manual_stamp) > self._joy_timeout:
                self._arm_resume()
                self._pub_cmd.publish(zero)
                return
            m = self._manual_twist
            at_neutral = (abs(m.linear.x) < _NEUTRAL_THRESH and
                          abs(m.angular.z) < _NEUTRAL_THRESH)
            if self._require_resume:
                if at_neutral:
                    self._seen_neutral = True
                if self._seen_neutral and not at_neutral:
                    self._require_resume = False
                    self.get_logger().info('Resume gate cleared — movement allowed')
                else:
                    self._pub_cmd.publish(zero)
                    return
            twist = Twist()
            if safety == SafetyState.SLOW:
                twist.linear.x = max(-self._slow_lin, min(self._slow_lin, m.linear.x))
                twist.angular.z = max(-self._slow_ang, min(self._slow_ang, m.angular.z))
            else:
                twist.linear.x = m.linear.x
                twist.angular.z = m.angular.z
            self._pub_cmd.publish(twist)
            return

        # --- normal joy mode ---
        if self._joy is not None:
            btn_val = self._btn(_ESTOP_BTN)
            if btn_val and not self._prev_estop_btn:
                self._estop = not self._estop
                self.get_logger().warning(
                    f'Software e-stop {"ACTIVE" if self._estop else "CLEARED"}'
                )
            self._prev_estop_btn = btn_val

        if self._estop:
            self._pub_cmd.publish(zero)
            return

        # --- Controller timeout ---
        if now - self._joy_stamp > self._joy_timeout:
            self._arm_resume()
            self.get_logger().warning(
                'Controller timeout — zero twist', throttle_duration_sec=5.0)
            self._pub_cmd.publish(zero)
            return

        # --- Deadman check (LB must be held) ---
        if not self._btn(_DEADMAN_BTN):
            self._pub_cmd.publish(zero)
            return

        # --- Safety state freshness ---
        safety_stale = (now - self._safety_stamp) > self._safety_timeout
        if safety_stale:
            self._arm_resume()
            self.get_logger().warning(
                '/adas/safety_state stale — treating as STOP',
                throttle_duration_sec=5.0)
            self._pub_cmd.publish(zero)
            return

        safety = SafetyState(self._safety)

        # --- Arm resume gate when exiting STOP ---
        if self._prev_safety == int(SafetyState.STOP) and int(safety) != int(SafetyState.STOP):
            self._require_resume = True
            self._seen_neutral = False
            self.get_logger().info(
                'STOP cleared — resume gate armed (return stick to neutral first)')
        self._prev_safety = int(safety)

        # --- Hard STOP: safety always overrides the Xbox command.  Publish a
        # zero command every cycle, so OpenCR's velocity controller brakes and
        # stays stopped even if joystick input continues. ---
        if safety == SafetyState.STOP:
            self._arm_resume()
            self._pub_cmd.publish(zero)
            return

        # --- Read controls.  A pressed face button overrides the stick and
        # produces an exact cardinal command, with no perpendicular drift. ---
        lin_raw, ang_raw = self._direction_input()

        # --- Resume gate: require neutral → new input after any STOP ---
        if self._require_resume:
            at_neutral = (abs(lin_raw) < _NEUTRAL_THRESH and
                          abs(ang_raw) < _NEUTRAL_THRESH)
            if at_neutral:
                self._seen_neutral = True
            if self._seen_neutral and not at_neutral:
                self._require_resume = False
                self.get_logger().info('Resume gate cleared — movement allowed')
            else:
                self._pub_cmd.publish(zero)
                return

        # --- Build twist ---
        twist = Twist()
        if safety == SafetyState.SLOW:
            twist.linear.x = max(-self._slow_lin, min(self._slow_lin, lin_raw * _MAX_LINEAR))
            twist.angular.z = max(-self._slow_ang, min(self._slow_ang, ang_raw * _MAX_ANGULAR))
        else:  # CLEAR
            twist.linear.x = lin_raw * _MAX_LINEAR
            twist.angular.z = ang_raw * _MAX_ANGULAR

        self._pub_cmd.publish(twist)

    # ------------------------------------------------------------------
    def _axis(self, idx: int) -> float:
        if self._joy is None or idx >= len(self._joy.axes):
            return 0.0
        return float(self._joy.axes[idx])

    def _direction_input(self) -> tuple[float, float]:
        """Return corrected stick input or an exact ABXY cardinal command."""
        button_linear = float(self._btn(_Y_BTN)) - float(self._btn(_A_BTN))
        button_angular = float(self._btn(_X_BTN)) - float(self._btn(_B_BTN))
        if button_linear != 0.0 or button_angular != 0.0:
            return button_linear, button_angular

        linear = self._apply_deadzone(
            self._linear_axis_sign * self._axis(_LINEAR_AXIS))
        angular = self._apply_deadzone(self._axis(_ANGULAR_AXIS))

        # When one axis clearly dominates, force the smaller perpendicular
        # axis to zero.  This keeps forward/back motion straight despite
        # normal thumb-stick wobble, while still allowing deliberate diagonals.
        if abs(linear) > abs(angular):
            if abs(angular) <= abs(linear) * self._cardinal_snap_ratio:
                angular = 0.0
        elif abs(angular) > abs(linear):
            if abs(linear) <= abs(angular) * self._cardinal_snap_ratio:
                linear = 0.0
        return linear, angular

    def _apply_deadzone(self, value: float) -> float:
        """Remove centre drift and smoothly rescale the remaining axis."""
        magnitude = abs(value)
        if magnitude <= self._stick_deadzone:
            return 0.0
        scaled = (magnitude - self._stick_deadzone) / (1.0 - self._stick_deadzone)
        return min(1.0, scaled) if value > 0.0 else -min(1.0, scaled)

    def _btn(self, idx: int) -> bool:
        if self._joy is None or idx >= len(self._joy.buttons):
            return False
        return bool(self._joy.buttons[idx])


def main(args=None) -> None:
    rclpy.init(args=args)
    node = CmdVelArbiter()
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
