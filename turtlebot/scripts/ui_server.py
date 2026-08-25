#!/usr/bin/env python3
"""RPi 통합 UI - 젯슨 영상 + Arty 안전상태 + 속도를 한 페이지로 서비스한다.

설계 원칙
---------
1. **영상은 손대지 않는다.** 젯슨이 이미 MJPEG(multipart/x-mixed-replace)을
   내보내고 RPi 가 8080 을 젯슨으로 DNAT 하고 있다. 브라우저의 <img> 가 그
   주소를 직접 물게 두면 RPi 는 디코딩도 재인코딩도 하지 않는다. 여기서
   프록시를 만들면 RPi CPU 만 쓰고 지연만 는다.

2. **안전상태는 `/adas/safety_state` 만 본다.** 이 토픽의 발행자는
   `uart_safety_receiver` 하나뿐이고, 그 노드는 Arty PS가 UART로 보낸
   유효 CRC 프레임이 올 때만 publish한다. Jetson은 객체 후보와 화면을
   담당하며 안전 상태를 직접 결정하거나 UART로 송신하지 않는다.

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
    --bg:#f2f5f9; --card:#fff; --line:#dbe3ec;
    --ink:#16202e; --muted:#6b7c93;
    --clear:#16a34a; --clear-d:#0f7a37;
    --slow:#eab308;  --slow-d:#c08d00;
    --stop:#dc2626;  --stop-d:#a81a1a;
    --dead:#8496ad;  --dead-d:#6b7c93;
  }
  *{box-sizing:border-box}
  html,body{height:100%}
  body{margin:0;padding:14px;background:var(--bg);color:var(--ink);overflow:hidden;
       font-family:system-ui,-apple-system,"Segoe UI",Roboto,"Noto Sans KR",sans-serif}
  .wrap{height:100%;display:flex;flex-direction:column;gap:10px}

  header{display:flex;align-items:center;gap:12px;flex:0 0 auto}
  h1{font-size:17px;margin:0;letter-spacing:.12em;font-weight:800}
  .sub{font-size:11px;color:var(--muted)}
  .live{margin-left:auto;display:flex;align-items:center;gap:7px;
        font-size:11px;color:var(--muted)}
  .dot{width:9px;height:9px;border-radius:50%;background:var(--dead)}
  .dot.on{background:var(--clear);box-shadow:0 0 0 4px rgba(22,163,74,.15)}

  /* 좌: 영상 / 우: 정보 — 스크롤 없이 한 화면 */
  main{flex:1 1 auto;min-height:0;display:grid;gap:12px;
       grid-template-columns:minmax(0,1fr) 290px}
  @media(max-width:900px){main{grid-template-columns:1fr;overflow:auto}}

  .card{background:var(--card);border:1px solid var(--line);border-radius:14px;
        padding:14px;min-height:0}
  .cap{font-size:10px;letter-spacing:.14em;color:var(--muted);
       font-weight:800;margin-bottom:8px;text-transform:uppercase}

  /* 영상 */
  /* 카드를 늘리지 않고 영상 크기에 맞춘다. 늘리면 카드 배경(검정)이
     영상 위아래로 남아 띠처럼 보인다. */
  .vcard{padding:0;overflow:hidden;display:flex;align-self:start;
         background:#0b0f16;border-color:#0b0f16}
  /* 박스를 영상 비율에 맞춘다. 박스가 더 길면 위아래에 검은 띠가 남는다.
     가로·세로 중 먼저 막히는 쪽에 맞춰 최대 크기로 채운다. */
  .videobox{aspect-ratio:16/9;width:100%;
            position:relative;overflow:hidden;background:#0b0f16;
            display:flex;align-items:center;justify-content:center}
  .videobox img{width:100%;height:100%;object-fit:cover;display:block}
  .videobox img.dead{display:none}
  .cammsg{display:none;padding:20px;text-align:center;color:#c9d4e0;
          font-size:13px;line-height:1.7}
  .cammsg.show{display:block}

  /* 우측 열 */
  /* 높이는 JS 가 영상 높이에 맞춰 넣는다(syncHeight). 그래야 창을 줄여도
     한쪽만 남거나 비어 보이지 않는다. */
  .side{display:flex;flex-direction:column;gap:12px;min-height:0;overflow:hidden}

  #stateCard{border:none;color:#fff;flex:0 0 auto;transition:background .18s}
  #stateCard .cap{color:rgba(255,255,255,.8)}
  .statebig{font-size:clamp(34px,5.2vh,58px);font-weight:900;letter-spacing:.05em;
            line-height:1;margin:2px 0 8px}
  .statenote{font-size:12px;color:rgba(255,255,255,.85)}
  .s-clear{background:linear-gradient(140deg,var(--clear),var(--clear-d))}
  .s-slow {background:linear-gradient(140deg,var(--slow),var(--slow-d));color:#231c00}
  .s-slow .statenote,.s-slow .cap{color:rgba(0,0,0,.62)}
  .s-stop {background:linear-gradient(140deg,var(--stop),var(--stop-d))}
  .s-dead {background:linear-gradient(140deg,var(--dead),var(--dead-d))}

  /* 속도 카드가 남는 높이를 흡수한다. 모자라면 표부터 접는다(JS). */
  .speedcard{flex:1 1 auto;min-height:0;display:flex;flex-direction:column;
             justify-content:flex-start}
  .speedrow{display:flex;align-items:baseline;gap:10px}
  .speednum{font-size:clamp(36px,6vh,64px);font-weight:900;line-height:1;
            font-variant-numeric:tabular-nums}
  .unit{font-size:15px;color:var(--muted);font-weight:700}
  .kmh{font-size:13px;color:var(--muted)}
  .bar{height:8px;border-radius:99px;background:#e6ecf4;margin:10px 0 5px;
       overflow:hidden}
  .bar i{display:block;height:100%;width:0;border-radius:99px;
         background:linear-gradient(90deg,#38bdf8,#2563eb);transition:width .15s}
  .barcap{display:flex;justify-content:space-between;font-size:10px;color:var(--muted)}

  /* 방향 패드 */
  .pad{display:grid;grid-template-columns:repeat(3,1fr);gap:6px;margin-top:12px}
  .dir{background:#eef3f9;border:1px solid var(--line);border-radius:9px;
       padding:9px 4px;text-align:center;font-size:12px;font-weight:700;
       color:#9aa9bd;transition:all .12s}
  .dir .ar{display:block;font-size:17px;line-height:1.1}
  .dir.on{background:#2563eb;border-color:#2563eb;color:#fff;
          box-shadow:0 3px 10px rgba(37,99,235,.28)}
  .dir.stopped{background:#e9edf3;color:#8496ad}
  /* 조종 입력은 들어왔는데 안전 판단이 막아서 실제로는 안 나가는 상태.
     파란 .on 과 확실히 달라 보여야 한다 - 이게 안전 개입의 증거다. */
  .dir.blk{background:#fee2e2;border-color:#ef4444;color:#b91c1c}
  .padcap{margin-top:12px;font-size:11px;font-weight:700;letter-spacing:.04em;
          color:#8496ad;display:flex;justify-content:space-between;align-items:center}
  .padcap .blkbadge{color:#b91c1c;font-weight:800}
  .note{margin-top:8px;font-size:11px;color:#c2820a}


  /* 연결 상태 - 어디가 끊겼는지 한눈에 */
  .links{display:grid;grid-template-columns:repeat(3,1fr);gap:7px}
  .lk{background:#f5f8fc;border:1px solid var(--line);border-radius:9px;
      padding:8px 6px;text-align:center}
  .lk .nm{font-size:10px;color:var(--muted);font-weight:700;letter-spacing:.03em}
  .lk .st{font-size:12px;font-weight:800;margin-top:3px;
          display:flex;align-items:center;justify-content:center;gap:5px}
  .lk .d{width:8px;height:8px;border-radius:50%;background:var(--dead);flex:0 0 auto}
  .lk.ok  .d{background:var(--clear)} .lk.ok  .st{color:var(--clear-d)}
  .lk.bad .d{background:var(--stop)}  .lk.bad .st{color:var(--stop-d)}
  .lk.bad{background:#fdf2f2;border-color:#f3c9c9}
</style>
</head>
<body>
<div class="wrap">
  <header>
    <div>
      <h1>ADAS UI</h1>
      <div class="sub">Jetson 후보 탐지 · Arty 분류·판단 · RPi 시각화</div>
    </div>
    <div class="live"><span id="dot" class="dot"></span><span id="foot">연결 중</span></div>
  </header>

  <main>
    <div class="card vcard">
      <div class="videobox">
        <img id="cam" alt="Jetson MJPEG">
        <div id="camMsg" class="cammsg">영상 연결 중...</div>
      </div>
    </div>

    <div class="side">
      <div class="card s-dead" id="stateCard">
        <div class="cap">현재 상태 · Arty UART</div>
        <div class="statebig" id="statePill">NO SIGNAL</div>
        <div class="statenote" id="stateAge"></div>
      </div>

      <div class="card speedcard">
        <div class="cap">속도</div>
        <div class="speedrow">
          <span id="mps" class="speednum">--</span>
          <span class="unit">m/s</span>
          <span id="kmh" class="kmh"></span>
        </div>
        <div class="bar"><i id="bar"></i></div>
        <div class="barcap"><span>0</span><span>0.22 (최고)</span></div>

        <div class="padcap">
          <span>조종 입력</span><span class="blkbadge" id="padBlk"></span>
        </div>
        <div class="pad">
          <div></div>
          <div class="dir" id="d-fwd"><span class="ar">&#9650;</span>전진</div>
          <div></div>
          <div class="dir" id="d-left"><span class="ar">&#9664;</span>좌회전</div>
          <div class="dir stopped" id="d-stop"><span class="ar">&#9632;</span>정지</div>
          <div class="dir" id="d-right"><span class="ar">&#9654;</span>우회전</div>
          <div></div>
          <div class="dir" id="d-back"><span class="ar">&#9660;</span>후진</div>
          <div></div>
        </div>
        <div class="note" id="odomNote"></div>
      </div>

      <div class="card" id="linkCard">
        <div class="cap">연결 상태</div>
        <div class="links">
          <div class="lk" id="lk-uart">
            <div class="nm">ARTY UART</div>
            <div class="st"><span class="d"></span><span id="lk-uart-v">--</span></div>
          </div>
          <div class="lk" id="lk-cam">
            <div class="nm">젯슨 영상</div>
            <div class="st"><span class="d"></span><span id="lk-cam-v">--</span></div>
          </div>
          <div class="lk" id="lk-motor">
            <div class="nm">모터 /odom</div>
            <div class="st"><span class="d"></span><span id="lk-motor-v">--</span></div>
          </div>
        </div>
      </div>
    </div>
  </main>
</div>

<script>
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
var camOk = false;
cam.onload = function () {
  camOk = true;
  camMsg.classList.remove('show');
  cam.classList.remove('dead');
};
cam.onerror = function () {
  camOk = false;
  camMsg.innerHTML = '영상 없음 — 젯슨 클라이언트 확인<br>'
    + '<span style="font-size:11px;opacity:.7">' + CAM_URL + '</span>';
  camMsg.classList.add('show');
  cam.classList.add('dead');
  setTimeout(camConnect, 3000);
};
camConnect();

var CLS = {0:'s-clear', 1:'s-slow', 2:'s-stop'};
var MAX_MPS = 0.22;
var LIN_EPS = 0.01;    // m/s   - 이 아래는 정지로 본다
var ANG_EPS = 0.05;    // rad/s

// 전후 방향 표시 부호. 이 로봇은 **전진할 때 linear.x 가 음수**로 나온다.
// button_teleop 의 SIGN_FB = -1.0 때문이고, /odom 도 같은 부호로 나오므로
// 두 소스 모두 이 한 값으로 맞는다.
//
// 근본 원인은 OpenCR 의 모터 방향 설정에 있을 가능성이 크다. 그쪽을 고치면
// 이 값을 +1 로 되돌려야 한다. 제어 코드를 건드려 표시를 맞추지는 않는다 -
// 실제로 잘 가는 주행을 깨뜨리기 때문이다 (2026-08-24 에 한 번 그랬다).
var DIR_SIGN = -1;

function fmt(v, d) {
  return (v === undefined || v === null) ? '--' : Number(v).toFixed(d);
}

// 패드는 **조종 입력**(/cmd_vel_manual)을 그린다. 중재 후 값(/cmd_vel)이
// 아니다. STOP 이 걸리면 중재 후 값은 0 이 되어버려서, 조종간을 밀고 있어도
// 화면에 아무것도 안 나온다. 그러면 "안전 판단이 막고 있다" 는 것을 보여줄
// 수가 없다. 입력은 입력대로 띄우고, 실제로 안 나가면 blk 로 표시한다.
function setDir(lin, ang, blocked) {
  var ids = ['d-fwd','d-back','d-left','d-right','d-stop'];
  for (var i = 0; i < ids.length; i++) {
    var e = document.getElementById(ids[i]);
    e.classList.remove('on'); e.classList.remove('blk');
  }
  var cls = blocked ? 'blk' : 'on';
  var moving = false;
  if (lin > LIN_EPS)  { document.getElementById('d-fwd').classList.add(cls);   moving = true; }
  if (lin < -LIN_EPS) { document.getElementById('d-back').classList.add(cls);  moving = true; }
  if (ang > ANG_EPS)  { document.getElementById('d-left').classList.add(cls);  moving = true; }
  if (ang < -ANG_EPS) { document.getElementById('d-right').classList.add(cls); moving = true; }
  if (!moving) { document.getElementById('d-stop').classList.add('on'); }
  return moving;
}

function setLink(id, ok, text) {
  var el = document.getElementById(id);
  el.className = 'lk ' + (ok ? 'ok' : 'bad');
  document.getElementById(id + '-v').textContent = text;
}

// 없는 키는 조용히 건너뛴다. 서버가 새 값을 추가해도 이 함수는 안 고쳐도 된다.
function render(s) {
  var card = document.getElementById('stateCard');
  var pill = document.getElementById('statePill');
  var age  = document.getElementById('stateAge');

  var sf = s.safety;
  if (!sf || sf.stale) {
    // 끊긴 값을 CLEAR 로 보여주지 않는다. 그게 이 화면에서 제일 위험한 오류다.
    card.className = 'card s-dead';
    pill.textContent = 'NO SIGNAL';
    age.textContent = sf ? ('신호 끊김 ' + fmt(sf.age_s, 1) + ' s')
                         : 'Arty UART 수신 없음';
  } else {
    card.className = 'card ' + (CLS[sf.state] || 's-dead');
    pill.textContent = sf.name;
    age.textContent = '수신 ' + fmt(sf.age_s * 1000, 0) + ' ms 전';
  }

  // 속도·방향은 실측(/odom)이 원칙이다. 모터 노드가 죽어 실측이 없을 때만
  // 명령값(/cmd_vel)으로 대신 보여주고, 그 사실을 화면에 적는다.
  var od = s.odom, cv = s.cmd_vel;
  var live = (od && !od.stale) ? od : null;
  var src = live ? od : ((cv && !cv.stale) ? cv : null);
  var note = document.getElementById('odomNote');

  // 속도 숫자·게이지 = 실제로 나간 값. 방향 패드 = 조종 입력. 둘을 일부러
  // 다른 소스에서 뽑는다 - 그래야 "밀고 있는데 안 나간다" 가 보인다.
  if (src) {
    var speed = Math.abs(src.linear_x);
    document.getElementById('mps').textContent = fmt(speed, 3);
    document.getElementById('kmh').textContent = '(' + fmt(speed * 3.6, 2) + ' km/h)';
    document.getElementById('bar').style.width =
      Math.min(100, speed / MAX_MPS * 100) + '%';
  }

  var mn = s.cmd_vel_manual;
  var inp = (mn && !mn.stale) ? mn : null;
  var inLin = inp ? inp.linear_x * DIR_SIGN : 0;
  var inAng = inp ? inp.angular_z : 0;
  // 실제로 바퀴가 도는가. 회전만 하는 경우도 있으니 각속도까지 본다.
  var actMoving = !!src && (Math.abs(src.linear_x) > LIN_EPS ||
                            Math.abs(src.angular_z) > ANG_EPS);
  var hasInput = Math.abs(inLin) > LIN_EPS || Math.abs(inAng) > ANG_EPS;
  var blocked = hasInput && !actMoving;
  setDir(inLin, inAng, blocked);
  document.getElementById('padBlk').textContent =
    blocked ? '차단됨 - 안전 개입' : (hasInput ? '' : '입력 없음');
  note.textContent = live ? ''
    : (src ? '실측 /odom 없음 — 명령값 기준 표시 (turtlebot3_node 확인)'
           : '/odom · /cmd_vel 둘 다 수신 없음');

  // 어디가 끊겼는지. 값의 age 로 판단하므로 별도 조회가 필요 없다.
  var uartOk = !!(sf && !sf.stale);
  setLink('lk-uart',  uartOk, uartOk ? (fmt(sf.age_s * 1000, 0) + ' ms') : '끊김');
  setLink('lk-cam',   camOk,  camOk ? '수신' : '끊김');
  setLink('lk-motor', !!live, live ? '수신' : '끊김');
}

// 영상은 16:9 라 폭이 정해지면 높이가 따라온다. 우측 열을 그 높이에 맞추고,
// 들어갈 자리가 없으면 표 -> 게이지 순으로 접어 잘려 보이지 않게 한다.
//
// ResizeObserver / requestAnimationFrame 에 의존하지 않는다 - 탭이 화면에
// 안 보이면 둘 다 안 도는 환경이 있다. 어차피 10 Hz 로 poll() 이 도니
// 거기서 부른다. 높이가 안 바뀌었으면 즉시 반환하므로 부담은 없다.
var lastVH = -1;
function syncHeight() {
  var vcard = document.querySelector('.vcard');
  var side  = document.querySelector('.side');
  if (!vcard || !side) { return; }

  if (window.innerWidth <= 900) {          // 세로 배치일 땐 높이를 고정하지 않는다
    if (side.style.height) { side.style.height = ''; lastVH = -1; }
    return;
  }

  var h = Math.round(vcard.getBoundingClientRect().height);
  if (h === lastVH) { return; }
  lastVH = h;
  side.style.height = h + 'px';

  var bar = side.querySelector('.bar');
  var cap = side.querySelector('.barcap');
  var speed = side.querySelector('.speedcard');
  [bar, cap].forEach(function (el) { if (el) { el.style.display = ''; } });

  // 넘치면 게이지부터 접는다. scrollHeight 읽기가 강제 레이아웃이라 즉시 반영된다.
  if (bar && speed.scrollHeight > speed.clientHeight + 1) {
    bar.style.display = 'none';
    if (cap) { cap.style.display = 'none'; }
  }
}
window.addEventListener('resize', syncHeight);

function poll() {
  fetch('/api/state', {cache: 'no-store'})
    .then(function (r) { return r.json(); })
    .then(function (s) {
      render(s);
      syncHeight();
      document.getElementById('dot').className = 'dot on';
      document.getElementById('foot').textContent = new Date().toLocaleTimeString();
    })
    .catch(function () {
      document.getElementById('dot').className = 'dot';
      document.getElementById('foot').textContent = 'ui_server 응답 없음';
    });
}
setInterval(poll, 100);
syncHeight();   // 10 Hz. 토픽이 20 Hz 라 이 정도면 충분하다.
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
