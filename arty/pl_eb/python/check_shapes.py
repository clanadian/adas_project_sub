#!/usr/bin/env python3
"""Gate: every place the classifier's shapes are written down still agrees.

The shapes live in four files that cannot include each other (C header, two
HLS testbenches, one Tcl build). This repo has been bitten before by a
hand-maintained list going stale while the gate that was supposed to catch it
looked at only two of the places. So this parses ALL of them and compares,
rather than trusting any one to be canonical.

Also verifies the two copied driver headers still match their source, and
that the engines' compile-time bounds still cover the shapes.

Exit code is the verdict. No string matching on output.
    python3 python/check_shapes.py
"""
import hashlib
import re
import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]        # 이 스크립트가 있는 트리
REPO = ROOT.parents[1]                            # project root

fails = []
def check(ok, msg):
    print(("  OK   " if ok else "  FAIL ") + msg)
    if not ok:
        fails.append(msg)


def read(p):
    return Path(p).read_text(encoding="utf-8", errors="surrogateescape")


# --- 1. classifier_net.h: the op table -------------------------------------
net = read(ROOT / "HW/classifier_net.h")
op_re = re.compile(
    r"\{\s*OP_(CONV0|CONV|MAXPOOL),\s*\"(\w+)\",\s*"
    r"(\d+),\s*(\d+),\s*(\d+),\s*(\d+),\s*(\d+),\s*(\d+),\s*(\d+),\s*(\d+)\s*\}")
ops = []
for m in op_re.finditer(net):
    kind, name, h, w, ic, oc, k, st, pad, leaky = m.groups()
    ops.append(dict(kind=kind, name=name, h=int(h), w=int(w), ic=int(ic),
                    oc=int(oc), k=int(k), stride=int(st), pad=int(pad),
                    leaky=int(leaky)))

print("== 1. classifier_net.h op table")
check(len(ops) == 6, f"6 ops parsed (got {len(ops)})")
declared = int(re.search(r"#define CLASSIFIER_NUM_OPS\s+(\d+)", net).group(1))
check(declared == len(ops), f"CLASSIFIER_NUM_OPS {declared} == parsed {len(ops)}")

# stride must be 1 on every conv - the hardware asserts it
for o in ops:
    if o["kind"].startswith("CONV"):
        check(o["stride"] == 1,
              f"{o['name']}: stride={o['stride']} (hardware implements stride=1 ONLY)")

# spatial/channel chain must be self-consistent
roi = int(re.search(r"#define ROI_SIZE\s+(\d+)", net).group(1))
in_ch = int(re.search(r"#define ROI_IN_CH\s+(\d+)", net).group(1))
h, c = roi, in_ch
conv0_pad = int(re.search(r"#define CONV0_PAD\s+(\d+)", net).group(1))
# CONV0_PADDED_SIZE 는 일부러 **식**으로 두었다(하드코딩 66 이 아니라).
# ROI_SIZE 를 바꾸면 따라오게 하기 위함이고, 그래서 int() 가 아니라 식으로 읽는다.
_pad_expr = re.search(r"#define CONV0_PADDED_SIZE\s+\((.*?)\)", net).group(1).strip()
print("== 2. shape chain is self-consistent")
check(_pad_expr == "ROI_SIZE + 2 * CONV0_PAD",
      f"CONV0_PADDED_SIZE 가 ROI_SIZE 에서 유도됨 (실제: '{_pad_expr}')")
check(eval(_pad_expr, {}, {"ROI_SIZE": roi, "CONV0_PAD": conv0_pad}) == roi + 2 * conv0_pad,
      f"CONV0_PADDED_SIZE 식이 {roi + 2*conv0_pad} 로 평가됨")
for o in ops:
    if o["kind"] == "CONV0":
        # conv0_engine takes a PRE-PADDED buffer and runs pad=0, so its
        # declared img is roi + 2*pad and its output is img - k + 1 = roi.
        # Getting this wrong does not fail loudly - it produces a 62x62
        # output that the next op reads as if it were 64x64.
        want = h + 2 * conv0_pad
        check(o["h"] == want and o["w"] == want,
              f"{o['name']}: declared {o['h']}x{o['w']}, expected PRE-PADDED {want}x{want} "
              f"(= chain {h} + 2*CONV0_PAD)")
        check(o["pad"] == 0,
              f"{o['name']}: pad={o['pad']} must be 0 - conv0_engine has no pad port, "
              f"the border is already in the buffer")
        check(o["ic"] == c, f"{o['name']}: in_ch {o['ic']}, chain says {c}")
        check(o["ic"] == 3 and o["oc"] == 16 and o["k"] == 3,
              f"{o['name']}: conv0_engine hardwires IN_CH=3/OUT_CH=16/K=3, "
              f"got {o['ic']}/{o['oc']}/{o['k']}")
        h = o["h"] - o["k"] + 1      # 66 - 3 + 1 = 64
        c = o["oc"]
        continue
    check(o["h"] == h and o["w"] == h,
          f"{o['name']}: declared {o['h']}x{o['w']}, chain says {h}x{h}")
    check(o["ic"] == c, f"{o['name']}: in_ch {o['ic']}, chain says {c}")
    if o["kind"] == "CONV":
        check(o["pad"] * 2 + 1 == o["k"] or o["k"] == 1,
              f"{o['name']}: k={o['k']} pad={o['pad']} preserves spatial size")
        c = o["oc"]
    else:
        h //= 2
        c = o["oc"]
gap_sz = int(re.search(r"#define GAP_IN_SIZE\s+(\d+)", net).group(1))
gap_ch = int(re.search(r"#define GAP_IN_CH\s+(\d+)", net).group(1))
check(gap_sz == h, f"GAP_IN_SIZE {gap_sz} == chain output {h}")
check(gap_ch == c, f"GAP_IN_CH {gap_ch} == chain channels {c}")

# --- 3. testbench tables agree with the header -----------------------------
print("== 3. testbench shape tables match the header")
# 이 트리(128)는 채택 구성만 둔다 - TR=16 참고본은 64 판
# (hls/arty_classifier/conv_engine/)에만 있다. 여기서 그걸 읽으려 하면
# FileNotFoundError 로 죽는다: 게이트가 없는 파일을 보고 있다는 뜻이므로
# 조용히 넘기지 않고 경로를 고치는 것이 맞다.
conv_tb = read(ROOT / "conv_engine_tr8/HW/conv_engine_tb.cpp")
tb_convs = re.findall(
    r'\{\s*"[^"]*",\s*(\d+),\s*(\d+),\s*(\d+),\s*(\d+),\s*(\d+),\s*(\d+)\s*\}', conv_tb)
hdr_convs = [(o["h"], o["w"], o["ic"], o["oc"], o["k"], o["pad"])
             for o in ops if o["kind"] == "CONV"]
tb_convs = [tuple(int(x) for x in t) for t in tb_convs]
# 상위집합을 허용한다: conv TB 는 conv0_engine 이 맡은 첫 층까지 들고 있는데,
# 그건 **폴백 비교용 실측치**로 일부러 남긴 것이다(conv0_engine 없이 갔을 때의
# 수치). 헤더의 OP_CONV 가 전부 TB 에 있으면 통과.
missing = [c for c in hdr_convs if c not in tb_convs]
check(not missing, f"conv TB 가 헤더의 OP_CONV 를 모두 포함 (빠진 것: {missing})")

# 위 검사는 **집합**만 본다. 그런데 `run.sh cosim tr8 <i>` 와
# collect_cosim.py 는 **인덱스**로 레이어를 고른다 - CLS_CONVS 를 재정렬하면
# 로그 이름과 실제로 잰 형상이 조용히 어긋나고, 사이클 숫자는 멀쩡해 보인다.
# ("실측"이라 적혀도 어느 범위인지 확인할 것 - 이 저장소가 하루에 네 번 틀린 지점)
tb_named = re.findall(
    r'\{\s*"([^"]*)",\s*(\d+),\s*(\d+),\s*(\d+),\s*(\d+),\s*(\d+),\s*(\d+)\s*\}', conv_tb)
tb_idx = [(n, tuple(int(x) for x in rest)) for n, *rest in
          [(m[0],) + tuple(m[1:]) for m in tb_named]]
# 인덱스 0 은 conv0 폴백(공유 엔진으로 첫 층을 돌렸을 때의 비교용), 1..n 이 헤더의 OP_CONV.
want0 = (roi, roi, in_ch, int(ndefs_c0), 3, 1) if (ndefs_c0 := re.search(
    r"#define CONV0_OUT_CH\s+(\d+)", net).group(1)) else None
check(len(tb_idx) >= 1 + len(hdr_convs),
      f"CLS_CONVS 가 폴백 1개 + OP_CONV {len(hdr_convs)}개 이상 (실제 {len(tb_idx)}개)")
if tb_idx:
    check(tb_idx[0][1] == want0,
          f"CLS_CONVS[0] 는 conv0 폴백 {want0} (실제 {tb_idx[0][1]}: '{tb_idx[0][0]}')")
for i, hc in enumerate(hdr_convs, start=1):
    if i < len(tb_idx):
        check(tb_idx[i][1] == hc,
              f"CLS_CONVS[{i}] == 헤더 OP_CONV[{i-1}] {hc} (실제 {tb_idx[i][1]}: '{tb_idx[i][0]}')")
    else:
        check(False, f"CLS_CONVS[{i}] 가 없음 - 헤더의 {hc} 를 잴 방법이 없다")

# conv0 TB 는 헤더의 PRE-PADDED 형상을 cosim 해야 한다. 여기가 어긋나면
# 실측 사이클이 "다른 형상의 수치"가 된다.
conv0_tb = read(ROOT / "conv0_engine/HW/conv0_engine_tb.cpp")
hdr_c0 = [(o["h"], o["w"]) for o in ops if o["kind"] == "CONV0"]
check(len(hdr_c0) == 1, f"OP_CONV0 는 정확히 1개 (실제 {len(hdr_c0)})")
if hdr_c0:
    ph, pw = hdr_c0[0]
    # 2026-08-19: 이 검사는 원래 `run_config("conv0-cosim", 98, 98` 를 리터럴로
    # 찾았다. 그날 D3 진단(환경변수로 형상을 바꿔 차분을 재는 경로)을 넣으면서
    # 그 자리가 변수 `dh, dw` 가 됐고, **정규식이 안 맞아 검사가 눈멀었다.**
    # 게이트가 조용히 통과하지 않고 FAIL 을 낸 덕분에 잡혔다 - 이 저장소의
    # "리팩터링이 게이트를 눈멀게 한다" 사례가 또 하나 늘었다.
    #
    # 이제 **기본값**을 본다. 진단 경로는 환경변수가 없으면 이 기본값으로
    # 돌므로, 정본 측정(conv0_cosim_x.log)이 헤더 형상인지는 이것이 정한다.
    m0 = re.search(r'unsigned\s+dh\s*=\s*(\d+)\s*,\s*dw\s*=\s*(\d+)\s*;', conv0_tb)
    check(m0 is not None,
          "conv0 TB 에 cosim 기본 형상(dh/dw)이 있음")
    if m0:
        check((int(m0.group(1)), int(m0.group(2))) == (ph, pw),
              f"conv0 TB cosim 기본 형상 {m0.group(1)}x{m0.group(2)} == 헤더 {ph}x{pw}")
    # 진단 경로가 살아 있다면 TB 가 읽는 환경변수 이름 **집합**과 run.sh 가
    # 넘기는 집합이 같아야 한다. 이름이 어긋나면 run.sh 는 조용히 기본 형상으로
    # 돌아 "다른 형상의 수치"를 만든다 - 값이 맞아서 제일 안 보이는 종류다.
    #
    # ⚠️ 부분 문자열로 검사하지 말 것. 처음에 `"CONV0_DIAG_W" in tb` 로 썼다가
    # 변이 시험에서 통과해 버렸다 - `CONV0_DIAG_WX` 가 그 부분 문자열을 포함한다.
    # 집합 비교로 바꾸니 같은 변이가 FAIL 로 잡힌다.
    tb_env = set(re.findall(r'getenv\("(CONV0_DIAG_[A-Z0-9_]+)"\)', conv0_tb))
    if tb_env:
        sh_env = set(re.findall(r'\b(CONV0_DIAG_[A-Z0-9_]+)=', read(ROOT / "run.sh")))
        check(tb_env == sh_env,
              f"conv0 진단 환경변수 집합 일치 - TB {sorted(tb_env)} == run.sh {sorted(sh_env)}")

pool_tb = read(ROOT / "maxpool_engine/HW/maxpool_engine_tb.cpp")
tb_pools = [tuple(int(x) for x in t) for t in re.findall(
    r'\{\s*"pool\d[^"]*",\s*(\d+),\s*(\d+),\s*(\d+)\s*\}', pool_tb)]
hdr_pools = [(o["h"], o["w"], o["ic"]) for o in ops if o["kind"] == "MAXPOOL"]
check(tb_pools == hdr_pools, f"pool TB {tb_pools} == header {hdr_pools}")

# --- 4. engine bounds cover the shapes -------------------------------------
print("== 4. engine compile-time bounds cover the shapes")
# ⚠️ 채택 구성의 공유 conv 는 **conv_engine_tr8** 이다. conv_engine(TR=16) 은
# conv0_engine 과 합치면 DSP 256 > 220 이라 기각됐고, 참고용으로만 남아 있다.
# 여기서 경로를 틀리면 "쓰지도 않는 포크를 검사하고 통과" 하게 된다.
ce = read(ROOT / "conv_engine_tr8/HW/conv_engine.h")
def const_u(txt, name):
    m = re.search(rf"^const unsigned {name}\s*=\s*(\d+);", txt, re.M)
    return int(m.group(1)) if m else None

max_img_w = const_u(ce, "MAX_IMG_W")
max_in_ch = const_u(ce, "MAX_IN_CH")
max_out_ch = const_u(ce, "MAX_OUT_CH")
tr = const_u(ce, "TR")
pe_oc = const_u(ce, "PE_OC")
# conv_engine 의 경계는 **OP_CONV 만** 대상이다. conv0 은 다른 엔진이 맡고
# 그 엔진의 MAX_IMG_W 는 아래 5번에서 따로 본다.
conv_ops_only = [o for o in ops if o["kind"] == "CONV"]
need_w = max(o["w"] for o in conv_ops_only)
need_ic = max(o["ic"] for o in conv_ops_only)
need_oc = max(o["oc"] for o in conv_ops_only)
check(max_img_w >= need_w, f"conv MAX_IMG_W {max_img_w} >= {need_w}")
check(max_in_ch >= need_ic, f"conv MAX_IN_CH {max_in_ch} >= {need_ic}")
check(max_out_ch >= need_oc, f"conv MAX_OUT_CH {max_out_ch} >= {need_oc}")
check(max_in_ch % tr == 0, f"TR {tr} divides MAX_IN_CH {max_in_ch} evenly")

# 2026-08-18: 위의 세 검사는 **지금 있는 레이어**만 본다. MAX_IN_CH 이 64->32 로
# 내려가 있었는데 실제 레이어의 최대 in_ch 가 32 라 전건 통과했고, 깨진 것은
# TB 의 선행 커버리지 형상(1x1 병목, in_ch=64)이었다 - csim 에서 assert 로
# 터졌다. 그래서 레이어 표와 무관한 **헤더 자체의 자기모순**을 본다:
#   ACCUM_POSITIONS == 1 이면 ic 타일 경로가 통째로 막혀 있다는 뜻이고,
#   그 상태에서 MAX_IN_CH < MAX_TOTAL_IN_CH 이면 그 사이 폭의 레이어는
#   런타임에 assert 로 죽는다. m_axi 는 그 폭을 받겠다고 선언해 놓고
#   온칩은 못 받는, 선언과 능력이 어긋난 상태다.
max_total_ic = const_u(ce, "MAX_TOTAL_IN_CH")
accum_pos = const_u(ce, "ACCUM_POSITIONS")
if accum_pos == 1:
    check(max_in_ch >= max_total_ic,
          f"ACCUM_POSITIONS=1 (ic 타일 경로 없음) 이므로 "
          f"MAX_IN_CH {max_in_ch} >= MAX_TOTAL_IN_CH {max_total_ic}")
else:
    print(f"       (ACCUM_POSITIONS={accum_pos}: ic 타일 경로 살아 있음, "
          f"MAX_IN_CH {max_in_ch} < MAX_TOTAL_IN_CH {max_total_ic} 허용)")
print(f"       (PE_OC={pe_oc}: out_ch {need_oc} needs "
      f"{-(-need_oc // pe_oc)} oc_tile passes on the widest conv)")

# conv0_engine 의 경계: PRE-PADDED 폭을 담아야 한다.
c0h = read(ROOT / "conv0_engine/HW/conv0_engine.h")
c0_img_w = const_u(c0h, "MAX_IMG_W")
c0_ops = [o for o in ops if o["kind"] == "CONV0"]
if c0_ops:
    check(c0_img_w >= max(o["w"] for o in c0_ops),
          f"conv0 MAX_IMG_W {c0_img_w} >= {max(o['w'] for o in c0_ops)} (PRE-PADDED)")
    check(const_u(c0h, "IN_CH") == 3 and const_u(c0h, "OUT_CH") == 16,
          f"conv0_engine IN_CH/OUT_CH = {const_u(c0h,'IN_CH')}/{const_u(c0h,'OUT_CH')} (하드와이어 3/16)")
# depth= 프라그마가 리터럴이면 MAX_IMG_W 축소를 안 따라와 cosim 이 SIGSEGV 난다.
# 2026-08-18 에 실제로 그렇게 죽었다.
c0cpp = read(ROOT / "conv0_engine/HW/conv0_engine.cpp")
for port in ("ifmap", "ofmap"):
    m = re.search(rf"port={port}\s+offset=slave\s+bundle=\w+\s+depth=(\S+)", c0cpp)
    check(m is not None and "MAX_IMG_W" in m.group(1),
          f"conv0 {port} 의 depth= 가 MAX_IMG_W 에서 유도됨 (실제: {m.group(1) if m else '없음'})")

mp = read(ROOT / "maxpool_engine/HW/maxpool_engine.h")
mp_img_w = const_u(mp, "MAX_IMG_W")
mp_ch = const_u(mp, "MAX_CH")
mp_row = const_u(mp, "MAX_ROW_WORDS")
pool_ops = [o for o in ops if o["kind"] == "MAXPOOL"]
check(mp_img_w >= max(o["w"] for o in pool_ops),
      f"pool MAX_IMG_W {mp_img_w} >= {max(o['w'] for o in pool_ops)}")
check(mp_ch >= max(o["ic"] for o in pool_ops),
      f"pool MAX_CH {mp_ch} >= {max(o['ic'] for o in pool_ops)}")
# Exceeding MAX_ROW_WORDS does NOT fail loudly - the engine silently drops to
# the slow per-position path. That is exactly the kind of silent regression
# this gate exists for.
worst_row = max(o["w"] * (o["ic"] // 4) for o in pool_ops)
check(mp_row >= worst_row,
      f"pool MAX_ROW_WORDS {mp_row} >= worst row {worst_row} words "
      f"(below this the row-buffer path silently disengages)")

# --- 5. copied driver headers have not diverged ----------------------------
print("== 5. copied driver headers still match their source")
# 드라이버마다 원본 위치가 다르다. conv/maxpool 은 팀 인계 패키지에 있고,
# conv0 은 그 패키지에 **없어서** HLS 프로젝트 쪽이 원본이다. 한 디렉터리로
# 뭉뚱그리면 conv0 이 "원본 없음"으로 조용히 FAIL 한다.
DRIVER_SRC = {
    "conv_engine_hw_driver.h":    REPO / "zybo_forteammate/05_layer_config",
    "maxpool_engine_hw_driver.h": REPO / "zybo_forteammate/05_layer_config",
    "conv0_engine_hw_driver.h":   REPO / "hls/conv0_engine/SW",
}
# 2026-08-19: 이 절은 **두 가지**를 검사한다. 갈라 놓아야 분리된 트리에서도
# 의미가 남는다.
#   (a) 사본 == DRIVERS.sha256      -> 언제나 검사 가능
#   (b) DRIVERS.sha256 == 상류 원본 -> **저장소 안에서만** 검사 가능
# PL 인수인계 폴더(`arty96_pl_source_handoff/`)처럼 트리만 떼어 놓으면 (b) 의
# 원본 경로가 존재할 수 없다. 그렇다고 조용히 건너뛰면 저장소 안에서 원본이
# 지워져도 통과한다 - 그래서 **전부 없으면 분리된 트리로 보고 건너뛰되,
# 일부만 없으면 FAIL** 한다. "하나가 사라진 것"과 "문맥이 통째로 없는 것"은
# 다른 사건이다.
_rows = [ln.split() for ln in read(ROOT / "SW/DRIVERS.sha256").strip().split("\n")]
_srcs = {name: (DRIVER_SRC.get(name) / name) if DRIVER_SRC.get(name) else None
         for _, name in _rows}
_present = [n for n, sp in _srcs.items() if sp and sp.exists()]
_detached = (len(_present) == 0)
for want, name in _rows:
    got_copy = hashlib.sha256((ROOT / "SW" / name).read_bytes()).hexdigest()
    check(got_copy == want, f"{name}: local copy matches DRIVERS.sha256")
    check(DRIVER_SRC.get(name) is not None, f"{name}: DRIVER_SRC 에 원본 경로가 등록됨")
    src = _srcs[name]
    if src is not None and src.exists():
        got_src = hashlib.sha256(src.read_bytes()).hexdigest()
        check(got_src == want, f"{name}: source {src} has not diverged")
    elif _detached:
        print(f"       (분리된 트리: 상류 원본 {src} 없음 - "
              f"DRIVERS.sha256 대조만 수행. 저장소 안에서는 이 줄이 뜨면 안 된다)")
    else:
        check(False, f"{name}: source {src} missing "
                     f"(다른 원본 {len(_present)}개는 있으므로 분리된 트리가 아니다 "
                     f"- 원본이 지워졌거나 옮겨졌다)")

# --- 6. build script points at these engines -------------------------------
print("== 6. Vivado build script points at this tree's IP")
_tcls = sorted((ROOT / "system").glob("build_*_classifier.tcl"))
check(len(_tcls) == 1, f"system/ 의 build_*_classifier.tcl 이 정확히 1개 (실제 {len(_tcls)})")
bt = read(_tcls[0]) if _tcls else ""
IP_FRAGS = ("$HLS_DIR/conv_engine_tr8/conv_engine_prj/solution1/impl/ip",
            "$HLS_DIR/conv0_engine/conv0_engine_prj/solution1/impl/ip",
            "$HLS_DIR/maxpool_engine/maxpool_engine_prj/solution1/impl/ip")
for frag in IP_FRAGS:
    check(frag in bt, f"IP repo listed: {frag}")

# 2026-08-19: 위 검사는 빌드 스크립트가 경로를 **적고 있는지**만 봤지 그 경로가
# **실제로 있는지**는 안 봤다. `open_project -reset` 이 export 된 IP 를 지우는데,
# 하루에 두 번 그렇게 사라져 빌드가 `== FATAL: IP 저장소 없음` 으로 멈췄다
# (어젯밤 conv_engine_tr8, 오늘 아침 conv0_engine). 빌드 tcl 이 잡아 주긴 했지만
# 그건 툴을 띄운 뒤이고, 게이트는 툴 없이 미리 말해 줄 수 있다.
#
# 손목록을 새로 만들지 않고 **위 조각에서 유도**한다 - 엔진이 늘거나 이름이
# 바뀌면 이 검사도 자동으로 따라온다.
missing_ip = [f.replace("$HLS_DIR/", "") for f in IP_FRAGS
              if not (ROOT / f.replace("$HLS_DIR/", "")).is_dir()]
# 2026-08-19: 이 검사는 **csynth/cosim 이 도는 동안 반드시 FAIL 한다** -
# `open_project -reset` 이 그 엔진의 export 된 IP 를 지우고 시작하기 때문이다.
# 사실 자체는 맞다(지금은 빌드할 수 없다). 다만 이유를 안 적어 두면 사람이
# 이 FAIL 을 "원래 뜨는 것"으로 배우고 진짜일 때도 무시하게 된다.
check(not missing_ip,
      f"세 엔진의 export 된 IP 가 모두 존재 (없는 것: {missing_ip}) "
      f"- 지금 csynth/cosim 이 돌고 있다면 그 도구가 지운 것이고 끝난 뒤 "
      f"`bash run.sh package <엔진>` 로 복구된다. 아니면 그냥 없는 것이다")
# Extract the ENGINES block properly: from "set ENGINES {" to the line that
# is exactly "}". Splitting on the first "}\n" instead only captures the first
# entry, which is how the first version of this check passed an inversion that
# added an engine on the second line.
m = re.search(r"^set ENGINES \{\n(.*?)^\}$", bt, re.M | re.S)
check(m is not None, "ENGINES block found in build script")
eng_block = m.group(1) if m else ""
eng_names = re.findall(r"^\s*\{(\S+)\s", eng_block, re.M)
WANT_ENGINES = ["conv_engine_0", "conv0_engine_0", "maxpool_engine_0"]
check(eng_names == WANT_ENGINES, f"ENGINES == {WANT_ENGINES} (got {eng_names})")
# 두 conv 포크는 IP 이름이 **둘 다 conv_engine** 이라, 저장소 경로를 잘못 쓰면
# 이름 충돌 없이 조용히 TR=16 판이 잡힌다. 경로에 tr8 이 있는지 직접 본다.
check("conv_engine/conv_engine_prj" not in bt.replace("conv_engine_tr8/conv_engine_prj", ""),
      "빌드가 TR=16 포크(conv_engine/)를 가리키지 않음")
check(not (ROOT / "conv_engine").exists(),
      "이 트리에는 TR=16 참고본이 없다 (있으면 어느 쪽을 빌드하는지 헷갈린다)")

# 2026-08-19: HW/ 에 소스 **사본**이 남아 있으면 안 된다. 오늘 하루 동안
# `conv_engine.h` 백업이 되돌린 결함(`MAX_IN_CH=32`)을 그대로 담은 채
# `baseline_W4/` 라는 **틀린 이름**으로 남아 있었다 - 그 이름은 "W4 로
# 되돌리려면 여기서 복원" 이라고 유도하는데, W4 는 `MAX_IN_CH=64` 로
# 측정됐으므로 복원하면 조용히 결함이 되살아난다.
#
# `add_files` 가 명시 파일명이라 합성에는 안 딸려가지만, **사람이 속는 것**이
# 이 저장소의 실제 사고 원인이었다. 사본을 두지 말고 캠페인 문서에 무엇을
# 바꿨는지 적을 것.
print("== 6-b. HW/ 에 소스 사본이 없다")
STRAY = []
for _e in ("conv_engine_tr8", "conv0_engine", "maxpool_engine"):
    _hw = ROOT / _e / "HW"
    if not _hw.is_dir():
        continue
    for _f in sorted(_hw.iterdir()):
        if not _f.is_file():
            continue
        # 정상 확장자만 허용한다. `.cpp.pre_W7`, `.h.bak`, `.cpp.W4_verified`
        # 같은 것이 전부 여기 걸린다.
        if _f.suffix not in (".cpp", ".h", ".hpp", ".md"):
            STRAY.append(str(_f.relative_to(ROOT)))
check(not STRAY, f"HW/ 에 소스 사본·백업이 없음 (발견: {STRAY})")

# --- 5-b. 드라이버 오프셋 == HLS 가 실제로 생성한 오프셋 ---------------------
# 2026-08-19 신설. `conv_engine.cpp` 주석이 이미 경고하고 있던 항목이다:
#   "accum 이 온칩으로 가면서 그 s_axilite 레지스터가 사라지고, **뒤따르는
#    스칼라 오프셋이 전부 0x0c 앞당겨진다** - 생성된 xconv_engine_hw.h 와
#    SW/conv_engine_hw_driver.h 를 다시 대조할 것."
# 그런데 그 대조를 **사람이 기억해야만** 했다. 포트를 하나 지우면 드라이버는
# 조용히 엉뚱한 주소에 쓰고, 보드가 없으니 아무도 모른다.
print("== 5-b. 드라이버 오프셋 == HLS 생성 오프셋")
DRV = [("conv_engine_tr8", "conv_engine", "XCONV_ENGINE_CTRL_ADDR_",
        "conv_engine_hw_driver.h", r'^#define\s+REG_(\w+)\s+0x([0-9A-Fa-f]+)'),
       ("conv0_engine", "conv0_engine", "XCONV0_ENGINE_CTRL_ADDR_",
        "conv0_engine_hw_driver.h", r'^#define\s+C0_REG_(\w+)\s+0x([0-9A-Fa-f]+)'),
       ("maxpool_engine", "maxpool_engine", "XMAXPOOL_ENGINE_CTRL_ADDR_",
        "maxpool_engine_hw_driver.h", r'^#define\s+(?:MP_)?REG_(\w+)\s+0x([0-9A-Fa-f]+)')]
_total_cmp = 0
for _d, _top, _pref, _drv, _pat in DRV:
    _gen = sorted((ROOT / _d).rglob(f"x{_top}_hw.h"))
    # ⚠️ `check()` 는 값을 반환하지 않는다(None). 처음에 `if not check(...)` 로
    # 분기했다가 **모든 엔진에서 continue 가 걸려 0 개를 셌다.** 아래
    # "대조 >= 8" 가드가 그걸 잡았다 - 검사에 검사를 붙여 둔 값을 했다.
    #
    # HLS 는 같은 드라이버 헤더를 여러 곳에 복사한다(.autopilot / impl/ip/drivers
    # 등, 이 트리는 3벌). "정확히 1개" 는 과한 조건이었다. 대신 **3벌이 서로
    # 같은지**를 본다 - 다르면 export 된 IP 가 지금 합성 결과와 다른 세대라는
    # 뜻이고, 그게 정작 위험한 상태다.
    _ok_gen = len(_gen) >= 1
    check(_ok_gen, f"{_top}: 생성 헤더 x{_top}_hw.h 를 찾음 ({len(_gen)}벌) "
                   f"- 0벌이면 csynth 를 먼저 돌릴 것")
    if not _ok_gen:
        continue
    _texts = {read(g) for g in _gen}
    check(len(_texts) == 1,
          f"{_top}: 생성 헤더 {len(_gen)}벌이 전부 동일 "
          f"(서로 다르면 export 된 IP 가 다른 세대다)")
    _g = read(_gen[0])
    _G = {m.group(1): int(m.group(2), 16) for m in
          re.finditer(rf'^#define\s+{_pref}(\w+?)(?:_DATA)?\s+0x([0-9A-Fa-f]+)', _g, re.M)}
    _D = {m.group(1): int(m.group(2), 16) for m in
          re.finditer(_pat, read(ROOT / "SW" / _drv), re.M)}
    _bad, _cmp = [], 0
    for _k, _off in sorted(_D.items()):
        _base = _k[:-8] if _k.endswith(("_ADDR_LO", "_ADDR_HI")) else _k
        if _base not in _G:
            continue
        _cmp += 1
        _exp = _G[_base] + (4 if _k.endswith("_ADDR_HI") else 0)
        if _exp != _off:
            _bad.append(f"{_k} 드라이버 0x{_off:x} vs 생성 0x{_exp:x}")
    _total_cmp += _cmp
    # 대조 건수가 0 이면 "일치"가 아니라 **검사가 눈먼 것**이다 (오늘 실제로
    # 정규식이 안 맞아 0 을 세고 통과할 뻔했다).
    check(_cmp >= 8, f"{_top}: 대조된 오프셋 {_cmp}개 (>=8 이어야 검사가 눈뜬 것)")
    check(not _bad, f"{_top}: 드라이버 오프셋 {_cmp}개 전부 생성값과 일치 (불일치: {_bad})")
print(f"       (세 엔진 합계 {_total_cmp}개 오프셋 대조)")

# --- 6-c. cosim 증거가 CURRENT 소스의 증거인가 ------------------------------
# 2026-08-19 신설. `roi_budget.py` 가 프레임 숫자의 정본인데, 그 입력인 cosim
# 로그가 **소스보다 낡았는지 아무도 안 보고 있었다.** 저장소 루트에
# `python/check_cosim_freshness.py` 가 있지만 이 트리의 게이트는 부르지 않는다.
#
# 이 구멍은 2026-08-09 에 실제로 열렸던 것이다(그때는 conv_engine.cpp 가
# 17:28 에 바뀌었는데 최신 cosim 은 전날 것이었다). 사이클은 그대로 그럴듯하게
# 나오고, 값이 맞아 보여서 제일 안 보인다.
#
# 로그 -> 엔진 대응은 **이름 접두사에서 유도**한다. 손목록을 만들면 op 를
# 추가할 때 그것만 안 따라온다.
print("== 6-c. cosim 로그가 현재 소스보다 새로운가")
# 대상은 **roi_budget 이 실제로 인용하는 로그**로 한정한다. 처음엔 로그 이름에
# `_diag_` 가 있으면 건너뛰게 했는데, 그 규칙은 D1 진단(`tr8_cosim_3.log`,
# 이름에 diag 가 없다)을 못 걸러 정상 상태에서 FAIL 을 냈다. 이름이 아니라
# **인용 여부**가 기준이어야 한다 - 진단 로그는 점 시점 산출물이고 프레임
# 숫자로 인용되지 않는다.
sys.path.insert(0, str(ROOT / "python"))
try:
    from roi_budget import ADOPTED as _ADOPTED          # noqa: E402
    _cited = [lg for _, lg in _ADOPTED]
except Exception as _e:                                  # pragma: no cover
    _cited = []
    check(False, f"roi_budget 의 ADOPTED 목록을 못 읽음: {_e}")
LOG_ENGINE = {"tr8_": "conv_engine_tr8", "conv0_": "conv0_engine",
              "pool_": "maxpool_engine"}
_stale = []
for _name in _cited:
    _lg = ROOT / "logs" / _name
    if not _lg.exists():
        continue      # 미실행은 roi_budget 이 이미 "인용하지 말 것" 이라고 찍는다
    _eng = next((v for k, v in LOG_ENGINE.items() if _name.startswith(k)), None)
    if _eng is None:
        continue
    _srcs = [f for f in (ROOT / _eng / "HW").glob("*")
             if f.suffix in (".cpp", ".h") and not f.name.endswith("_tb.cpp")]
    _newer = [f.name for f in _srcs if f.stat().st_mtime > _lg.stat().st_mtime]
    if _newer:
        _stale.append(f"{_name} < {sorted(_newer)}")
check(not _stale,
      f"인용되는 cosim 로그 {len(_cited)}개가 전부 소스보다 새로움 "
      f"(낡은 것: {_stale}) - 낡았다면 `bash run.sh cosim <엔진> <op>` 를 다시 돌릴 것")

# --- 6-d. 배포 패키지가 이 트리와 정합한가 ----------------------------------
# 2026-08-19 신설. 패키지 무결성(CHECKSUMS)과 "패키지 XSA == 트리 XSA" 는
# **생성기 안에서만** 확인되고 있었다. 게이트만 돌리고 인계하면 낡은 패키지를
# 보낼 수 있다 - 게이트는 트리를 보지 패키지를 안 봤다.
#
# 패키지가 없으면 이 트리는 아직 배포 대상이 아니라는 뜻이므로 검사하지 않는다.
# **있으면 반드시 정합해야 한다.**
print("== 6-d. 배포 패키지 <-> 트리")
# ⚠️ ROI_SIZE 는 `#define` 이라 `const_u`(= `const unsigned` 전용)로는 못 읽는다.
# 처음에 그렇게 썼다가 None -> 이름이 `deliverable_arty_classifier` 가 되어
# **64 판 패키지를 96 트리와 대조**했고, 당연히 XSA 불일치로 FAIL 했다.
# 폴백으로 다른 판을 집는 구조 자체가 위험하다 - 이름을 못 만들면 멈춘다.
_m_roi = re.search(r"#define\s+ROI_SIZE\s+(\d+)", read(ROOT / "HW/classifier_net.h"))
check(_m_roi is not None, "classifier_net.h 에서 ROI_SIZE 를 읽음")
_roi = int(_m_roi.group(1)) if _m_roi else None
# 64 판만 이름에 크기가 없다(그 판이 먼저 생겨서). 그 예외만 명시한다.
_pkg = ROOT.parent.parent / ("deliverable_arty_classifier" if _roi == 64
                             else f"deliverable_arty{_roi}_classifier")
if not _pkg.is_dir():
    print(f"       (배포 패키지 없음 - 이 트리는 아직 배포 대상이 아니다)")
else:
    _px = sorted((_pkg / "bitstream").glob("*.xsa"))
    _tx = sorted((ROOT / "system").glob("*.xsa"))
    check(len(_px) == 1 and len(_tx) == 1,
          f"{_pkg.name}: XSA 가 패키지 {len(_px)}개 / 트리 {len(_tx)}개 (각 1개)")
    if len(_px) == 1 and len(_tx) == 1:
        _h1 = hashlib.sha256(_px[0].read_bytes()).hexdigest()
        _h2 = hashlib.sha256(_tx[0].read_bytes()).hexdigest()
        check(_h1 == _h2,
              f"{_pkg.name}: 패키지 XSA == 트리 XSA (sha256 {_h1[:12]} vs {_h2[:12]})")
    _ck = _pkg / "CHECKSUMS.sha256"
    check(_ck.exists(), f"{_pkg.name}/CHECKSUMS.sha256 가 있음")
    if _ck.exists():
        _bad = []
        _listed = 0
        for _ln in _ck.read_text(encoding="utf-8").splitlines():
            if not _ln.strip():
                continue
            _hh, _rel = _ln.split(None, 1)
            _rel = _rel.strip()
            _f = _pkg / _rel
            _listed += 1
            if not _f.exists() or hashlib.sha256(_f.read_bytes()).hexdigest() != _hh:
                _bad.append(_rel)
        _files = sum(1 for f in _pkg.rglob("*")
                     if f.is_file() and f.name != "CHECKSUMS.sha256")
        check(_listed == _files,
              f"{_pkg.name}: CHECKSUMS 가 {_listed}개 등재 == 실제 파일 {_files}개 "
              f"(적으면 무검증 파일이 실려 나간다)")
        check(not _bad, f"{_pkg.name}: 등재된 {_listed}개 전부 해시 일치 (불일치: {_bad})")

# --- 7. Windows 260 바이트 경로 한계 --------------------------------------
# Vivado 가 HLS IP 를 풀 때 나오는 최장 경로를 미리 계산한다. 이건 40분 뒤
# generate_target 에서 죽는 대신 지금 죽으라는 검사다. 2026-08-18 에 실제로
# 272 바이트로 죽었고, YOLO 판은 255 로 **여유가 5 바이트뿐**이었다.
print("== 7. Windows 260 바이트 경로 한계")
proj = re.search(r"^set PROJ_NAME\s+(\S+)", bt, re.M).group(1)
bdn  = re.search(r"^set BD_NAME\s+(\S+)", bt, re.M).group(1)
# 실측으로 확인된 최장 파일명 (HLS 가 C++ 맹글링을 파일명에 쓴다)
LONGEST = ("conv_engine_conv_engine_Pipeline_FUSED_SHIFT_STEP_p_"
           "ZZL16scan_and_computePK7ap_uintILi32Ebkb.dat")
sysdir = (ROOT / "system").resolve()
# WSL 경로를 Windows 경로 길이로 환산: /mnt/c/... -> c:/...
win_prefix = str(sysdir).replace("/mnt/c/", "c:/")
worst = (f"{win_prefix}/{proj}/{proj}.gen/sources_1/bd/{bdn}/"
         f"ip/{bdn}_conv_engine_0_0/hdl/verilog/{LONGEST}")
check(len(worst) <= 260,
      f"최장 예상 경로 {len(worst)} <= 260 (PROJ='{proj}' BD='{bdn}', 여유 {260-len(worst)})")
if len(worst) > 200:
    print(f"       여유 {260-len(worst)} 바이트. 이름을 늘리기 전에 여기부터 볼 것.")

# --- 8. manifest 와 PS 참조 구현이 같은 GAP 규약을 쓰는가 -----------------
# gap_mode 를 sum 으로 정해놓고 코드가 나누면 모든 logit 이 64배 작아진다.
# **argmax 는 순서가 안 바뀌어서 그대로 맞게 나온다** - 기능 시험을 통과하고
# confidence 에서만 틀린다. 그래서 사람이 아니라 게이트가 봐야 한다.
mpath = ROOT / "forps_golden/manifest.json"
print("== 8. manifest <-> PS 참조 구현 GAP 규약")
if mpath.exists():
    import json
    man = json.loads(read(mpath))
    mode = man.get("ps_tail", {}).get("gap_mode")
    folded = man.get("ps_tail", {}).get("gap_divisor_folded_into_fc")
    src = read(ROOT / "SW/classifier_run.c")
    m = re.search(r"out_64\[c\]\s*=\s*acc\s*(/)?", src)
    divides = bool(m and m.group(1))
    check(mode in ("sum", "mean"), f"manifest gap_mode = '{mode}'")
    # 2026-08-19: 나눗수를 **값으로** 검사한다. 해상도별로 64/144/256 이고,
    # 틀려도 argmax 는 맞고 confidence 만 틀려서 기능 시험을 통과한다.
    _div = man.get("ps_tail", {}).get("gap_divisor")
    check(_div == gap_sz * gap_sz,
          f"manifest gap_divisor {_div} == GAP_IN_SIZE^2 {gap_sz * gap_sz}")
    check((mode == "sum") != divides,
          f"gap_mode='{mode}' 인데 코드가 {'나눈다' if divides else '안 나눈다'}")
    check(bool(folded) == (mode == "sum"),
          f"gap_divisor_folded_into_fc={folded} 가 gap_mode='{mode}' 와 정합")
else:
    check(False, "forps_golden/manifest.json 이 없다 - python/gen_manifest.py 를 돌릴 것")

# --- 9. 주소맵 헤더가 빌드 로그와 일치하는가 ------------------------------
# SW/arty_cls_address_map.h 는 PS 가 XSA 를 열기 전에 쓰라고 만든 것이다.
# 손으로 옮겨적은 값이므로 빌드 로그(정본)와 어긋나면 PS 가 엉뚱한 창에
# 레지스터를 쓰게 된다 - 엔진이 그냥 안 움직이거나 다른 엔진을 건드린다.
print("== 9. 주소맵 헤더 <-> Vivado 주소 할당")
amap = ROOT / "SW/arty_cls_address_map.h"
blog = ROOT / "logs/vivado_build.log"
if amap.exists() and blog.exists():
    hdr = read(amap)
    log = blog.read_bytes().decode("utf-8", errors="replace").replace("\x00", "")
    for inst, macro in [("conv_engine_0", "ARTY_CLS_CONV_ENGINE_0_BASE"),
                        ("conv0_engine_0", "ARTY_CLS_CONV0_ENGINE_0_BASE"),
                        ("maxpool_engine_0", "ARTY_CLS_MAXPOOL_ENGINE_0_BASE")]:
        lm = re.search(rf"/{inst}/s_axi_CTRL/Reg' is being assigned into address space "
                       r"'/ps7_0/Data' at <0x([0-9A-Fa-f_]+)", log)
        hm = re.search(rf"#define {macro}\s+0x([0-9A-Fa-f]+)u", hdr)
        lv = int(lm.group(1).replace("_", ""), 16) if lm else None
        hv = int(hm.group(1), 16) if hm else None
        check(lv is not None and lv == hv,
              f"{inst}: 헤더 0x{hv:08x} == 빌드 로그 0x{lv:08x}" if lv is not None and hv is not None
              else f"{inst}: 주소를 못 읽음 (로그={lv} 헤더={hv})")
else:
    check(False, f"주소맵 헤더 또는 빌드 로그 없음 (amap={amap.exists()} log={blog.exists()})")

# --- 10. run.sh 가 **자기 트리**를 가리키는가 -----------------------------
# 트리를 복사하면 하드코딩된 경로가 따라오지 않는다. 그러면 툴이 **원본 트리에서**
# 돌고, csim 은 원본 소스로 통과하며 종료 코드도 0 이다. 산출물이 안 생긴 것만이
# 단서가 된다 - 2026-08-18 에 실제로 그렇게 3판을 날렸다.
print("== 10. run.sh 가 자기 트리를 가리키는가")
rs = read(ROOT / "run.sh")
tree = ROOT.name
# 2026-08-19: 예전엔 `WIN_SUB` 를 검사했다. 그 변수는 이제 없다 - 툴 호출 경로가
# `WIN_TREE`(wslpath 로 유도) 하나로 합쳐졌기 때문이다. 죽은 변수를 계속 검사하면
# **WIN_TREE 가 틀려도 게이트가 통과한다.** 검사를 실제 동작하는 쪽으로 옮긴다.
# ⚠️ 대입문만 본다. 처음에 `"WIN_SUB" not in rs` 로 썼다가 **주석의 역사 서술**
# 까지 잡아 FAIL 했다 - 오늘 CLAUDE.md 에 넣은 "부분 문자열 검사 금지" 그대로다.
check(re.search(r"^WIN_SUB=", rs, re.M) is None,
      "run.sh 에 죽은 WIN_SUB 대입이 남아 있지 않음")
check("wslpath -w" in rs,
      "run.sh 가 WIN_TREE 를 wslpath 로 유도 (하드코딩하면 다른 트리에서 툴이 돈다)")
m = re.search(r'cd /d \$WIN_TREE', rs)
check(m is not None, "툴 호출이 $WIN_TREE 를 쓴다")
check(rs.count("cd /d $WIN_TREE") >= 2,
      f"vitis_hls/vivado 두 호출 모두 $WIN_TREE 사용 (실제 {rs.count('cd /d $WIN_TREE')}곳)")

# --- 11. PS 버퍼 크기 상수가 형상에서 유도되는가 ---------------------------
# A shape written twice is a shape that rots. This tree was forked from the 64
# build on 2026-08-18: every entry in CLASSIFIER_OPS followed ROI_SIZE, but
# three hardcoded byte counts did not. ACT_BUF_BYTES stayed 64*64*16 = 65,536
# while conv0's output became 96*96*16 = 147,456, so the PS would have written
# 2.25 buffers' worth into act_a and silently overrun act_b. Nothing caught it:
# the host TB allocated with the same wrong macro, so it agreed with itself.
#
# This section recomputes all three from the op table and rejects any literal.
print("== 11. PS 버퍼 크기 상수 <-> op 테이블")

def macro_defs(txt):
    """#define NAME body -> {NAME: body}. Include guards have no body and are skipped."""
    d = {}
    for m in re.finditer(r"^[ \t]*#define[ \t]+(\w+)[ \t]+(.+?)[ \t]*(?://.*|/\*.*)?$", txt, re.M):
        d[m.group(1)] = m.group(2).strip()
    return d

def macro_eval(name, defs, _seen=None):
    """Evaluate a C integer macro. Returns None if it is not arithmetic."""
    _seen = _seen or set()
    if name in _seen:
        return None
    body = defs.get(name)
    if body is None:
        return None
    e = re.sub(r"\(unsigned\)", "", body)
    e = re.sub(r"/\*.*?\*/", "", e)
    e = re.sub(r"\b(\d+)[uU]\b", r"\1", e)
    env = {}
    for ident in set(re.findall(r"[A-Za-z_]\w*", e)):
        v = macro_eval(ident, defs, _seen | {name})
        if v is None:
            return None
        env[ident] = v
    try:
        return int(eval(e, {"__builtins__": {}}, env))
    except Exception:
        return None

def op_out(o):
    """Output spatial size of one op. conv0 is pre-padded with pad=0."""
    if o["kind"] == "CONV0":
        return o["h"] - o["k"] + 1
    if o["kind"] == "CONV":
        return o["h"]                      # pad=1, stride=1 -> SAME
    return o["h"] // o["stride"]           # maxpool 2x2 s2

ndefs = macro_defs(net)
GEOM = ("ROI_SIZE", "ROI_IN_CH", "CONV0_PADDED_SIZE", "CONV0_PAD",
        "CONV0_OUT_CH", "GAP_IN_SIZE", "GAP_IN_CH")

# 11-a. the two macros that restate the op table
c0_oc = int(ndefs.get("CONV0_OUT_CH", -1)) if str(ndefs.get("CONV0_OUT_CH", "")).isdigit() else None
check(c0_oc == ops[0]["oc"],
      f"CONV0_OUT_CH {c0_oc} == CLASSIFIER_OPS[0].out_ch {ops[0]['oc']}")
last = ops[-1]
check(macro_eval("GAP_IN_SIZE", ndefs) == op_out(last),
      f"GAP_IN_SIZE {macro_eval('GAP_IN_SIZE', ndefs)} == {last['name']} 출력 {op_out(last)}")
check(macro_eval("GAP_IN_CH", ndefs) == last["oc"],
      f"GAP_IN_CH {macro_eval('GAP_IN_CH', ndefs)} == {last['name']} out_ch {last['oc']}")

# 11-b. the three byte counts, recomputed from the table
need = {
    # conv0_engine reads the PRE-PADDED buffer the PS stages
    "CLS_WIRE_INPUT_BYTES": ops[0]["h"] * ops[0]["w"] * ops[0]["ic"],
    # one ping-pong slot must hold the LARGEST output any op produces
    "CLS_ACT_BUF_BYTES": max(op_out(o) ** 2 * o["oc"] for o in ops),
    "CLS_PL_OUTPUT_BYTES": op_out(last) ** 2 * last["oc"],
}
biggest = max(ops, key=lambda o: op_out(o) ** 2 * o["oc"])
for name, want in need.items():
    body = ndefs.get(name)
    check(body is not None, f"{name} 가 classifier_net.h 에 있음")
    if body is None:
        continue
    # A literal passes arithmetic today and rots the next time ROI_SIZE moves.
    check(any(g in body for g in GEOM),
          f"{name} 가 형상 매크로에서 유도됨 (실제: '{body}')")
    got = macro_eval(name, ndefs)
    check(got == want, f"{name} = {got} == 형상이 요구하는 {want}")
print(f"       (가장 큰 중간 텐서는 {biggest['name']} 의 출력 "
      f"{op_out(biggest)}x{op_out(biggest)}x{biggest['oc']} = {need['CLS_ACT_BUF_BYTES']:,} B)")

# 11-c. every consumer must take the derived macro, not its own copy
consumers = [
    ("SW/arty_cls_address_map.h", "ARTY_CLS_WIRE_INPUT_BYTES", "CLS_WIRE_INPUT_BYTES"),
    ("SW/arty_cls_address_map.h", "ARTY_CLS_ACT_BUF_BYTES",    "CLS_ACT_BUF_BYTES"),
    ("SW/arty_cls_address_map.h", "ARTY_CLS_PL_OUTPUT_BYTES",  "CLS_PL_OUTPUT_BYTES"),
    ("SW/classifier_run.c",       "ACT_BUF_BYTES",             "CLS_ACT_BUF_BYTES"),
]
for rel, macro, want_ref in consumers:
    body = macro_defs(read(ROOT / rel)).get(macro)
    check(body is not None and want_ref in body,
          f"{rel}: {macro} 가 {want_ref} 를 참조 (실제: '{body}')")

amap_txt = read(ROOT / "SW/arty_cls_address_map.h")
check("classifier_net.h" in amap_txt,
      "arty_cls_address_map.h 가 classifier_net.h 를 include 함")

# 11-d. the host TB must allocate from the same macros. It allocated 64-build
# sizes and still passed, because a TB that agrees with a wrong constant is
# not a check - it is a second copy of the bug.
tb = read(ROOT / "verif_host/tb_sequencer.c")
for var, want_ref in [("roi", "CLS_WIRE_INPUT_BYTES"), ("a", "ACT_BUF_BYTES")]:
    m = re.search(rf"static uint8_t {var}\[([^\]]+)\]", tb)
    check(m is not None and want_ref in m.group(1),
          f"tb_sequencer.c: {var}[] 가 {want_ref} 로 잡힘 (실제: '{m.group(1) if m else None}')")



# --- 4-b. 누산 폭이 최악 MAC 합을 담는가 ------------------------------------
# 2026-08-19 신설. `partial_t` 는 ap_int<28> 인데 필요한 폭은
# **MAX_IN_CH 와 MAX_K 에서 유도**된다: 한 타일의 최악합은
# MAX_IN_CH * MAX_K^2 * 127 * 127 이고 부호 1비트가 더 붙는다.
# 오늘 MAX_IN_CH 가 32 <-> 64 로 두 번 움직였다. 늘리는 방향이면 이 폭이
# 조용히 모자라고, **오버플로는 값이 뒤집혀서 나오지 예외를 안 낸다** -
# csim 은 같은 C 타입을 쓰니 RTL 과 함께 틀려서 비트일치까지 통과한다.
print("== 4-b. 누산 폭 vs 최악 MAC 합")
_pt = re.search(r"typedef\s+ap_int<(\d+)>\s+partial_t", ce)
_at = re.search(r"typedef\s+ap_int<(\d+)>\s+accum_t", ce)
check(_pt is not None and _at is not None, "partial_t / accum_t 폭을 읽음")
if _pt and _at:
    _maxk = const_u(ce, "MAX_K") or 3
    _worst = max_in_ch * _maxk * _maxk * 127 * 127
    _need = _worst.bit_length() + 1          # 부호 비트
    check(int(_pt.group(1)) >= _need,
          f"partial_t {_pt.group(1)}비트 >= 최악합 {_worst:,} 이 요구하는 {_need}비트 "
          f"(MAX_IN_CH={max_in_ch}, MAX_K={_maxk})")
    check(int(_at.group(1)) >= _need,
          f"accum_t {_at.group(1)}비트 >= {_need}비트")

# 2026-08-19: §11 이 검사하던 건# 2026-08-19: §11 이 검사하던 건 위 세 개(WIRE_INPUT / ACT_BUF / PL_OUTPUT)
# 뿐이었다. 그건 H-1 에서 **실제로 터진** 세 개라서 그렇고, 같은 헤더의
# 가중치·바이어스 바이트 수 6개는 **게이트 없는 리터럴**로 남아 있었다.
# 해상도로는 안 변하지만 채널 수가 바뀌면 안 따라오고, 틀리면 PS 가 잘못된
# 바이트 수로 DMA 한다 - 조용히 틀린 가중치로 추론한다는 뜻이다.
# 값 자체는 op 테이블에서 완전히 유도된다: W = oc*ic*k*k, B = oc*4.
print("== 11-b. 가중치/바이어스 바이트 수 <-> op 테이블")
_am = read(ROOT / "SW/arty_cls_address_map.h")
_convs = [o for o in ops if o["kind"] in ("CONV", "CONV0")]
# 주소맵의 이름은 conv0/conv1/conv2 순서다. op 테이블도 체인 순서이므로
# CONV0 를 먼저, 그다음 CONV 들을 순서대로 놓으면 대응한다.
_ordered = ([o for o in _convs if o["kind"] == "CONV0"] +
            [o for o in _convs if o["kind"] == "CONV"])
check(len(_ordered) == 3, f"conv 계열 op 이 3개 (실제 {len(_ordered)})")
for _i, _o in enumerate(_ordered):
    _w_exp = _o["oc"] * _o["ic"] * _o["k"] * _o["k"]
    _b_exp = _o["oc"] * 4
    for _tag, _exp in (("W", _w_exp), ("B", _b_exp)):
        _m = re.search(rf"^#define\s+ARTY_CLS_{_tag}_CONV{_i}_BYTES\s+(\d+)u",
                       _am, re.M)
        check(_m is not None, f"ARTY_CLS_{_tag}_CONV{_i}_BYTES 가 주소맵에 있음")
        if _m:
            check(int(_m.group(1)) == _exp,
                  f"ARTY_CLS_{_tag}_CONV{_i}_BYTES {_m.group(1)} == "
                  f"{'oc*ic*k*k' if _tag == 'W' else 'oc*4'} {_exp} "
                  f"(oc={_o['oc']} ic={_o['ic']} k={_o['k']})")

# --- 12. PS 계약 산출물 <-> op 테이블 --------------------------------------
# manifest.json 과 conv0_engine 의 m_axi depth= 는 **PS 가 그대로 믿는 숫자**다.
# 둘 다 형상을 손으로 옮겨 적은 곳이라, §11 이 고친 것과 똑같이 썩는다.
print("== 12. manifest / depth= 리터럴 <-> op 테이블")

c0h_t = read(ROOT / "conv0_engine/HW/conv0_engine.h")
c0 = {n: const_u(c0h_t, n) for n in ("IN_CH", "OUT_CH", "K")}
c0src = read(ROOT / "conv0_engine/HW/conv0_engine.cpp")
for port, want, how in [
        ("weights", c0["OUT_CH"] * c0["IN_CH"] * c0["K"] * c0["K"], "OUT_CH*IN_CH*K*K"),
        ("bias",    c0["OUT_CH"],                                    "OUT_CH")]:
    m = re.search(rf"port={port}\s+offset=slave\s+bundle=\w+\s+depth=(\S+)", c0src)
    got = m.group(1) if m else None
    val = None
    if got is not None:
        try:
            val = int(eval(got, {"__builtins__": {}}, dict(c0)))
        except Exception:
            val = None
    check(val == want, f"conv0_engine {port} depth= '{got}' = {val} == {how} {want}")

if mpath.exists():
    import json as _json
    man = _json.loads(read(mpath))
    last = ops[-1]
    def op_out_s(o):
        if o["kind"] == "CONV0":
            return o["h"] - o["k"] + 1
        if o["kind"] == "CONV":
            return o["h"]
        return o["h"] // o["stride"]
    pre = ops[0]["h"]
    want_map = [
        ("input.logical_shape", man["input"]["logical_shape"], [roi, roi, in_ch]),
        ("input.wire_shape",    man["input"]["wire_shape"],    [pre, pre, in_ch]),
        ("input.wire_bytes",    man["input"]["wire_bytes"],    macro_eval("CLS_WIRE_INPUT_BYTES", ndefs)),
        ("pl_output.shape",     man["pl_output"]["shape"],     [op_out_s(last), op_out_s(last), last["oc"]]),
        ("pl_output.bytes",     man["pl_output"]["bytes"],     macro_eval("CLS_PL_OUTPUT_BYTES", ndefs)),
        ("weight_layout.conv0.bytes", man["weight_layout"]["conv0"]["bytes"],
         c0["OUT_CH"] * c0["IN_CH"] * c0["K"] * c0["K"]),
        ("fixed_in_bitstream.conv_MAX_IMG_W", man["fixed_in_bitstream"]["conv_MAX_IMG_W"], max_img_w),
    ]
    for name, got, want in want_map:
        check(got == want, f"manifest {name} = {got} == 형상이 요구하는 {want}")
else:
    check(False, "manifest.json 이 없어 PS 계약을 대조할 수 없다")


# --- 13. 재양자화 자리끼우개 상수가 6곳에서 일치하는가 ---------------------
# (mult, shift, leaky) 삼중항이 golden 생성기 / numpy 교차검증 / 두 HLS TB /
# 시퀀서 TB / PS 계약 문서에 **각각 따로** 적혀 있다. 학습이 끝나면 여섯 곳을
# 전부 바꿔야 하는데, 하나를 빠뜨리면 골든과 DUT 가 서로 다른 스케일로
# 통과한다 - 값이 아니라 **출처가 갈라지는** 이 저장소의 단골 실패다.
# 정본은 golden 을 실제로 만드는 verif_host/gen_golden.c 로 둔다.
print("== 13. 재양자화 자리끼우개 상수 일치")

gg = read(ROOT / "verif_host/gen_golden.c")
def c_arr(txt, name, cast=int):
    m = re.search(rf"{name}\[3\]\s*=\s*\{{([^}}]*)\}}", txt)
    return [cast(x.strip()) for x in m.group(1).split(",")] if m else None

CANON_M = c_arr(gg, "MULT")
CANON_S = c_arr(gg, "SHIFT")
check(CANON_M is not None and CANON_S is not None,
      f"정본(gen_golden.c) MULT={CANON_M} SHIFT={CANON_S}")

if CANON_M and CANON_S:
    vg = read(ROOT / "python/verify_golden.py")
    m = re.search(r"MULT\s*=\s*\[([^\]]*)\]", vg)
    s = re.search(r"SHIFT\s*=\s*\[([^\]]*)\]", vg)
    check(m and [int(x) for x in m.group(1).split(",")] == CANON_M,
          f"verify_golden.py MULT == 정본 {CANON_M}")
    check(s and [int(x) for x in s.group(1).split(",")] == CANON_S,
          f"verify_golden.py SHIFT == 정본 {CANON_S}")

    tbs = read(ROOT / "verif_host/tb_sequencer.c")
    seq = [(int(a), int(b)) for a, b in re.findall(
        r"classifier_set_quant\(\s*\d+\s*,\s*(\d+)\s*,\s*(\d+)", tbs)]
    check(seq == list(zip(CANON_M, CANON_S)),
          f"tb_sequencer.c set_quant == 정본 {list(zip(CANON_M, CANON_S))} (실제 {seq})")

    con = read(ROOT / "forps_golden/CONTRACT.txt")
    doc = [(int(a), int(b)) for a, b in re.findall(
        r"mult=(\d+)\s+shift=(\d+)", con)]
    check(doc == list(zip(CANON_M, CANON_S)),
          f"CONTRACT.txt == 정본 (실제 {doc})")

    # 두 HLS TB 는 conv0 값만 쓴다(형상 스윕용이라 레이어별이 아니다).
    for rel in ("conv0_engine/HW/conv0_engine_tb.cpp",
                "conv_engine_tr8/HW/conv_engine_tb.cpp"):
        tb = read(ROOT / rel)
        mm = re.search(r"STANDIN_MULT\w*\s*=\s*(\d+)", tb)
        ss = re.search(r"STANDIN_SHIFT\s*=\s*(\d+)", tb)
        check(mm and int(mm.group(1)) == CANON_M[0],
              f"{rel.split('/')[-1]} STANDIN_MULT == 정본 conv0 {CANON_M[0]}")
        check(ss and int(ss.group(1)) == CANON_S[0],
              f"{rel.split('/')[-1]} STANDIN_SHIFT == 정본 conv0 {CANON_S[0]}")


# --- 13-b. leaky 플래그가 다섯 곳에서 일치하는가 ---------------------------
# §13 의 주석은 삼중항 (mult, shift, leaky) 을 말해 놓고 앞의 둘만 봤다.
# 2026-08-19 에 그 구멍으로 conv2 가 갈라진 채 출하됐다: 계산하는 세 곳
# (gen_golden.c / verify_golden.py / classifier_run.c) 은 linear 인데
# classifier_net.h 의 op 표만 leaky 였고, 그 표가 gen_manifest.py 를 거쳐
# **학습 쪽이 양자화 기준으로 삼는 manifest.json** 이 된다. 하드웨어는 한 사이클도
# 안 변하고 가중치만 못 쓰게 되는 종류라, 어떤 형상·타이밍 게이트도 못 잡는다.
print("== 13-b. leaky 플래그 일치 (계산 3곳 + op 표 + manifest)")

CANON_L = c_arr(gg, "LEAKY")
check(CANON_L is not None and len(CANON_L) == 3,
      f"정본(gen_golden.c) LEAKY={CANON_L}")

if CANON_L:
    vg_t = read(ROOT / "python/verify_golden.py")
    m = re.search(r"LEAKY\s*=\s*\[([^\]]*)\]", vg_t)
    vg_l = [1 if x.strip() == "True" else 0 for x in m.group(1).split(",")] if m else None
    check(vg_l == CANON_L, f"verify_golden.py LEAKY {vg_l} == 정본 {CANON_L}")

    # PS 드라이버가 레지스터에 실제로 넣는 기본값.
    run_t = read(ROOT / "SW/classifier_run.c")
    m = re.search(r"g_quant\[3\]\s*=\s*\{(.*?)\};", run_t, re.S)
    ps_l = [int(a) for a in re.findall(r"\{\s*\d+\s*,\s*\d+\s*,\s*(\d+)\s*\}", m.group(1))] if m else None
    check(ps_l == CANON_L, f"classifier_run.c g_quant leaky {ps_l} == 정본 {CANON_L}")

    con_t = read(ROOT / "forps_golden/CONTRACT.txt")
    con_l = [int(x) for x in re.findall(r"leaky=(\d+)", con_t)]
    check(con_l == CANON_L, f"CONTRACT.txt leaky {con_l} == 정본 {CANON_L}")

    # op 표의 leaky 열 - 여기서 갈라진 게 2026-08-19 사고였다.
    tab_l = [o["leaky"] for o in ops if o["kind"] in ("CONV", "CONV0")]
    check(tab_l == CANON_L,
          f"classifier_net.h op 표 leaky {tab_l} == 정본 {CANON_L}")

    # 그리고 그 표에서 생성된 manifest - 학습 쪽이 실제로 읽는 문서.
    if mpath.exists():
        man_l = [1 if l.get("activation") == "leaky_relu" else 0
                 for l in _json.loads(read(mpath))["layers"]
                 if l["name"].startswith("conv")]
        check(man_l == CANON_L,
              f"manifest.json activation {man_l} == 정본 {CANON_L} "
              f"(다르면 gen_manifest.py 를 다시 돌릴 것)")

print()
if fails:
    print(f"FAILED: {len(fails)} check(s)")
    for f in fails:
        print("  - " + f)
    sys.exit(1)
print("ALL SHAPE CHECKS PASS")
