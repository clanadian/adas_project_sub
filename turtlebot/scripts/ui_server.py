#!/usr/bin/env python3
"""RPi 통합 UI - 젯슨 영상 + Arty 안전상태 + 속도를 한 페이지로 서비스한다.

설계 원칙
---------
1. **영상은 손대지 않는다.** 젯슨이 이미 MJPEG(multipart/x-mixed-replace)을
   내보내고 RPi 가 8080 을 젯슨으로 DNAT 하고 있다. 브라우저의 <img> 가 그
   주소를 직접 물게 두면 RPi 는 디코딩도 재인코딩도 하지 않는다. 여기서
   프록시를 만들면 RPi CPU 만 쓰고 지연만 는다.

2. **안전상태는 `/adas/safety_state` 만 본다.** 이 토픽의 발행자는
   `uart_safety_receiver` 하나뿐이고, 그 노드는 Arty 가 UART 로 보낸
   유효 CRC 프레임이 올 때만 publish 한다. 안전 판단과 UART 송신은 Arty PS가
   전담한다.

3. **오래된 값을 CLEAR 로 보여주지 않는다.** 안전 표시에서 가장 위험한 오류는
   "데이터가 끊겼는데 안전해 보이는 것"이다. stale_sec 을 넘으면 상태를
   NO SIGNAL 로 바꾼다 - 마지막 값을 그대로 두지 않는다.

4. **확장은 JSON 키 추가로.** 페이지는 `/api/state` 를 폴링하고 없는 키는
   조용히 건너뛴다. 새 값을 붙일 때 HTML 을 안 고쳐도 되고, 반대로 HTML 만
   먼저 고쳐도 깨지지 않는다.

실행:
    python3 ~/ui_server.py
    # 파라미터를 바꾸려면
    python3 ~/ui_server.py --ros-args -p port:=8090 -p mjpeg_port:=8080

브라우저: http://10.10.16.200:8090
"""

from __future__ import annotations

import json
import threading
from http.server import BaseHTTPRequestHandler, ThreadingHTTPServer

import rclpy
from rclpy.node import Node
from std_msgs.msg import Int32
from geometry_msgs.msg import Twist
from nav_msgs.msg import Odometry

# UART_PROTOCOL 의 state 값과 같다. 숫자가 클수록 위험하다.
STATE_NAMES = {0: 'CLEAR', 1: 'SLOW', 2: 'STOP'}


class UiState:
    """노드 스레드가 쓰고 HTTP 스레드가 읽는 공유 상태.

    값과 함께 **받은 시각**을 같이 들고 있는 것이 요점이다. 시각이 없으면
    "끊긴 것"과 "안 변하는 것"을 구분할 수 없다.
    """

    def __init__(self) -> None:
        self._lock = threading.Lock()
        self._data: dict[str, dict] = {}

    def put(self, key: str, value: dict, stamp: float) -> None:
        with self._lock:
            self._data[key] = {'value': value, 'stamp': stamp}

    def snapshot(self, now: float, stale_sec: float) -> dict:
        with self._lock:
            items = dict(self._data)

        out: dict = {'ts': now, 'stale_sec': stale_sec}
        for key, entry in items.items():
            age = now - entry['stamp']
            out[key] = dict(entry['value'])
            out[key]['age_s'] = round(age, 3)
            out[key]['stale'] = age > stale_sec
        return out


class UiServerNode(Node):
    def __init__(self) -> None:
        super().__init__('ui_server')
        self.declare_parameter('port', 8090)
        self.declare_parameter('mjpeg_port', 8080)
        # 안전상태가 이 시간보다 오래되면 NO SIGNAL 로 표시한다.
        # arbiter 의 safety_timeout(2.0s)보다 짧게 두어 화면이 먼저 알린다.
        self.declare_parameter('stale_sec', 1.0)

        self.port = int(self.get_parameter('port').value)
        self.mjpeg_port = int(self.get_parameter('mjpeg_port').value)
        self.stale_sec = float(self.get_parameter('stale_sec').value)

        self.state = UiState()

        self.create_subscription(Int32, '/adas/safety_state', self._on_safety, 10)
        self.create_subscription(Twist, '/cmd_vel', self._on_cmd_vel, 10)
        self.create_subscription(Twist, '/cmd_vel_manual', self._on_cmd_manual, 10)
        self.create_subscription(Odometry, '/odom', self._on_odom, 10)

        self.get_logger().info(
            f'UI server: http://<this-host>:{self.port}  '
            f'(MJPEG {self.mjpeg_port}, stale {self.stale_sec}s)')

    def now(self) -> float:
        return self.get_clock().now().nanoseconds * 1e-9

    # --- 구독 콜백 -----------------------------------------------------
    def _on_safety(self, msg: Int32) -> None:
        self.state.put('safety', {
            'state': int(msg.data),
            'name': STATE_NAMES.get(int(msg.data), f'UNKNOWN({msg.data})'),
        }, self.now())

    def _on_cmd_vel(self, msg: Twist) -> None:
        self.state.put('cmd_vel', {
            'linear_x': round(msg.linear.x, 4),
            'angular_z': round(msg.angular.z, 4),
        }, self.now())

    def _on_cmd_manual(self, msg: Twist) -> None:
        self.state.put('cmd_vel_manual', {
            'linear_x': round(msg.linear.x, 4),
            'angular_z': round(msg.angular.z, 4),
        }, self.now())

    def _on_odom(self, msg: Odometry) -> None:
        # 실제로 나온 속도. 바퀴 엔코더에서 계산된 값이라 적분이 필요 없다.
        # IMU 가속도를 적분하면 정지 상태에서도 바이어스가 쌓여 10초에 1 m/s
        # 넘게 틀어진다 - 최고 속도가 0.22 m/s 인 로봇에서는 쓸 수 없다.
        self.state.put('odom', {
            'linear_x': round(msg.twist.twist.linear.x, 4),
            'angular_z': round(msg.twist.twist.angular.z, 4),
        }, self.now())


def build_handler(node: UiServerNode):
    class Handler(BaseHTTPRequestHandler):
        protocol_version = 'HTTP/1.1'

        def log_message(self, *_args) -> None:
            pass  # 폴링 때문에 로그가 초당 수십 줄 쌓인다. 끈다.

        def _send(self, code: int, body: bytes, ctype: str) -> None:
            self.send_response(code)
            self.send_header('Content-Type', ctype)
            self.send_header('Content-Length', str(len(body)))
            self.send_header('Cache-Control', 'no-store')
            self.end_headers()
            self.wfile.write(body)

        def do_GET(self) -> None:
            path = self.path.split('?', 1)[0]
            if path in ('/', '/index.html'):
                body = PAGE.replace('__MJPEG_PORT__', str(node.mjpeg_port)) \
                           .encode('utf-8')
                self._send(200, body, 'text/html; charset=utf-8')
            elif path == '/api/state':
                snap = node.state.snapshot(node.now(), node.stale_sec)
                self._send(200, json.dumps(snap).encode('utf-8'),
                           'application/json; charset=utf-8')
            else:
                self._send(404, b'not found', 'text/plain; charset=utf-8')

    return Handler


# ---------------------------------------------------------------------------
# 페이지. 외부 CDN 을 쓰지 않는다 - 실습망에서 인터넷이 없어도 떠야 한다.
# MJPEG 주소는 브라우저가 접속한 호스트에서 만든다. 그래야 RPi IP 가 바뀌거나
# 다른 PC 에서 열어도 그대로 동작한다.
# ---------------------------------------------------------------------------
PAGE = r"""<!doctype html>
<html lang="ko">
<head>
<meta charset="utf-8">
<meta name="viewport" content="width=device-width,initial-scale=1">
<title>ADAS UI</title>
<style>
  :root{
    --bg:#eef1f5; --card:#fff; --ink:#1b2b3a; --muted:#64748b; --line:#d7dee7;
    --clear:#16a34a; --slow:#eab308; --stop:#dc2626; --dead:#94a3b8;
  }
  *{box-sizing:border-box}
  body{margin:0;padding:20px;background:var(--bg);color:var(--ink);
       font-family:system-ui,-apple-system,"Segoe UI",Roboto,"Noto Sans KR",sans-serif}
  .wrap{max-width:1000px;margin:0 auto}
  h1{font-size:22px;margin:0 0 16px;letter-spacing:.02em}
  .card{background:var(--card);border:1px solid var(--line);border-radius:12px;
        padding:16px;margin-bottom:16px}
  .label{font-size:12px;color:var(--muted);margin-bottom:8px;font-weight:600}
  .videobox{background:#dfe5ec;border-radius:10px;padding:12px}
  .videobox img{width:100%;display:block;border-radius:6px;background:#000;
                border:3px solid #22384d}
  .videobox img.dead{display:none}
  .cammsg{display:none;padding:40px 16px;text-align:center;color:var(--muted);
          font-size:14px;background:#111a24;border-radius:6px;
          border:3px solid #22384d;color:#c9d4e0}
  .cammsg.show{display:block}
  .sysinfo{margin-top:10px;font-size:13px;font-weight:700}
  .grid{display:grid;grid-template-columns:1fr 1fr;gap:16px}
  @media(max-width:760px){.grid{grid-template-columns:1fr}}
  .statebig{display:flex;align-items:center;gap:14px}
  .pill{padding:12px 22px;border-radius:8px;color:#fff;font-weight:800;
        font-size:18px;letter-spacing:.04em}
  .pill.clear{background:var(--clear)} .pill.slow{background:var(--slow);color:#3a2f00}
  .pill.stop{background:var(--stop)}  .pill.dead{background:var(--dead)}
  .statetext{font-size:34px;font-weight:800}
  .speedrow{display:flex;align-items:baseline;gap:10px}
  .speednum{font-size:40px;font-weight:800;font-variant-numeric:tabular-nums}
  .unit{font-size:15px;color:var(--muted)}
  table{width:100%;border-collapse:collapse;font-size:14px}
  td{padding:5px 0;border-bottom:1px solid #eef2f6}
  td:last-child{text-align:right;font-variant-numeric:tabular-nums;font-weight:600}
  .warn{color:var(--stop);font-weight:700}
  .foot{font-size:12px;color:var(--muted);text-align:right}
</style>
</head>
<body>
<div class="wrap">
  <h1>UI SYSTEM</h1>

  <div class="card">
    <div class="videobox">
      <!-- 젯슨이 내보내는 MJPEG 를 브라우저가 직접 받는다. RPi 는 중계만 한다
           (iptables DNAT 8080 -> 192.168.100.2:8080). -->
      <img id="cam" alt="Jetson MJPEG">
      <div id="camMsg" class="cammsg">영상 연결 중...</div>
    </div>
    <div class="sysinfo">시스템 정보: Jetson 검출 + Arty 판단, RPi 시각화</div>
  </div>

  <div class="grid">
    <div class="card">
      <div class="label">현재 상태 (Arty UART)</div>
      <div class="statebig">
        <span id="pill" class="pill dead">NO SIGNAL</span>
        <span id="stateText" class="statetext">--</span>
      </div>
      <div id="stateAge" class="foot" style="text-align:left;margin-top:8px"></div>
    </div>

    <div class="card">
      <div class="label">속도 정보 (실측 /odom)</div>
      <div class="speedrow">
        <span id="mps" class="speednum">--</span><span class="unit">m/s</span>
        <span id="kmh" class="unit"></span>
      </div>
      <table style="margin-top:12px">
        <tr><td>명령 직진 /cmd_vel linear.x</td><td id="cvx">--</td></tr>
        <tr><td>명령 회전 /cmd_vel angular.z</td><td id="cvz">--</td></tr>
        <tr><td>실측 직진 /odom linear.x (부호 유지)</td><td id="odx">--</td></tr>
        <tr><td>실측 회전 /odom angular.z</td><td id="odz">--</td></tr>
      </table>
    </div>
  </div>

  <div class="foot" id="foot">연결 중...</div>
</div>

<script>
// 영상: 접속한 호스트 그대로 + MJPEG 포트. RPi IP 가 바뀌어도 따라간다.
var CAM_URL = location.protocol + '//' + location.hostname + ':__MJPEG_PORT__/';
var cam = document.getElementById('cam');
var camMsg = document.getElementById('camMsg');

// MJPEG 스트림이 끊기면 <img> 는 조용히 빈 칸이 된다. 데모 중에 그러면
// "왜 안 나오지"로 시간을 버리므로, 이유를 적고 스스로 다시 붙는다.
function camConnect() {
  camMsg.textContent = '영상 연결 중...';
  camMsg.classList.add('show');
  cam.classList.add('dead');
  cam.src = CAM_URL + '?t=' + Date.now();
}
cam.onload = function () {
  camMsg.classList.remove('show');
  cam.classList.remove('dead');
};
cam.onerror = function () {
  camMsg.innerHTML = '영상 없음 - 젯슨 클라이언트가 떠 있는지 확인<br>'
    + '<span style="font-size:12px">' + CAM_URL + '</span>';
  camMsg.classList.add('show');
  cam.classList.add('dead');
  setTimeout(camConnect, 3000);   // 젯슨이 다시 뜨면 알아서 붙는다
};
camConnect();

var CLS = {0:'clear', 1:'slow', 2:'stop'};

function fmt(v, digits) {
  return (v === undefined || v === null) ? '--' : Number(v).toFixed(digits);
}

// 없는 키는 조용히 건너뛴다. 서버가 새 값을 추가해도 이 함수는 안 고쳐도 된다.
function render(s) {
  var pill = document.getElementById('pill');
  var text = document.getElementById('stateText');
  var age  = document.getElementById('stateAge');

  var sf = s.safety;
  if (!sf || sf.stale) {
    // 끊긴 값을 CLEAR 로 보여주지 않는다. 그게 이 화면에서 제일 위험한 오류다.
    pill.className = 'pill dead';
    pill.textContent = 'NO SIGNAL';
    text.textContent = '--';
    age.innerHTML = sf
      ? '<span class="warn">신호 끊김 ' + fmt(sf.age_s, 1) + ' s</span>'
      : '<span class="warn">Arty UART 수신 없음</span>';
  } else {
    pill.className = 'pill ' + (CLS[sf.state] || 'dead');
    pill.textContent = sf.name;
    text.textContent = sf.name;
    age.textContent = '수신 ' + fmt(sf.age_s * 1000, 0) + ' ms 전';
  }

  var od = s.odom, cv = s.cmd_vel;
  if (od) {
    // 터틀봇 Burger 최고 속도가 0.22 m/s 라 km/h 로 쓰면 0.0~0.8 사이에서만
    // 움직여 변화가 안 보인다. m/s 를 소수 3자리로 주고 km/h 는 보조로 둔다.
    //
    // 큰 숫자는 부호를 뗀 **속력**이다. 후진하면 /odom 의 linear.x 가 음수로
    // 오는데, 화면에서 보고 싶은 것은 "얼마나 빠른가"이지 진행 방향이 아니다.
    // 방향이 필요하면 아래 표의 원본 값(부호 유지)을 보면 된다.
    var speed = Math.abs(od.linear_x);
    document.getElementById('mps').textContent = fmt(speed, 3);
    document.getElementById('kmh').textContent =
      '(' + fmt(speed * 3.6, 2) + ' km/h)';
    document.getElementById('odx').textContent = fmt(od.linear_x, 3);
    document.getElementById('odz').textContent = fmt(od.angular_z, 3);
  }
  if (cv) {
    document.getElementById('cvx').textContent = fmt(cv.linear_x, 3);
    document.getElementById('cvz').textContent = fmt(cv.angular_z, 3);
  }
}

function poll() {
  fetch('/api/state', {cache: 'no-store'})
    .then(function (r) { return r.json(); })
    .then(function (s) {
      render(s);
      document.getElementById('foot').textContent =
        '갱신 ' + new Date().toLocaleTimeString();
    })
    .catch(function () {
      document.getElementById('foot').innerHTML =
        '<span class="warn">ui_server 응답 없음</span>';
    });
}
setInterval(poll, 100);   // 10 Hz. 토픽이 20 Hz 라 이 정도면 충분하다.
poll();
</script>
</body>
</html>
"""


def main() -> None:
    rclpy.init()
    node = UiServerNode()

    httpd = ThreadingHTTPServer(('0.0.0.0', node.port), build_handler(node))
    httpd.daemon_threads = True
    threading.Thread(target=httpd.serve_forever, daemon=True).start()

    try:
        rclpy.spin(node)
    except KeyboardInterrupt:
        pass
    finally:
        httpd.shutdown()
        node.destroy_node()
        if rclpy.ok():
            rclpy.shutdown()


if __name__ == '__main__':
    main()
