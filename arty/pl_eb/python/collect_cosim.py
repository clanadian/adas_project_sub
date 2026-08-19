#!/usr/bin/env python3
"""cosim 로그에서 실측 사이클을 모은다.

cosim 리포트는 같은 프로젝트에 다음 판을 돌리면 덮인다. 그래서 이 저장소는
**로그의 `$finish` 시각**에서 사이클을 복원한다 - 이전 세대 스윕에서 리포트가
남아 있던 레이어 3개와 **20 사이클 이내**로 일치함을 확인한 방법이다.

    python3 python/collect_cosim.py
"""
import re
import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
LOGS = ROOT / "logs"
CLOCK_NS = 10.0          # 100 MHz

# 로그 이름 -> (표시명, 출력 위치 수). 둘 다 HW/classifier_net.h 의 ROI_SIZE 에서
# **유도한다**. 예전에는 여기에 64 판 형상이 박혀 있어서, 128 트리에서 돌린
# 실측치에 64 라벨이 붙고 cyc/pos 가 4배로 부풀려 나왔다 - 사이클은 맞는데
# 옆 칸이 틀려서 알아채기 어렵다. 하드코딩으로 되돌리지 말 것.
_NET = (ROOT / "HW/classifier_net.h").read_text(encoding="utf-8", errors="surrogateescape")
ROI = int(re.search(r"#define ROI_SIZE\s+(\d+)", _NET).group(1))
PRE = ROI + 2 * int(re.search(r"#define CONV0_PAD\s+(\d+)", _NET).group(1))
R2, R4, R8 = ROI // 2, ROI // 4, ROI // 8

JOBS = {
    "conv0_cosim_x.log":  (f"conv0  3->16 @{ROI}x{ROI}  (conv0_engine)", ROI * ROI),
    "tr8_cosim_0.log":    (f"conv0  3->16 @{ROI}x{ROI}  (공유 conv, TR=8) [폴백]", ROI * ROI),
    "tr8_cosim_1.log":    (f"conv1  16->32 @{R2}x{R2} (공유 conv, TR=8)", R2 * R2),
    "tr8_cosim_2.log":    (f"conv2  32->64 @{R4}x{R4} (공유 conv, TR=8)", R4 * R4),
    "conv_cosim_0.log":   (f"conv0  3->16 @{ROI}x{ROI}  (공유 conv, TR=16)", ROI * ROI),
    "conv_cosim_1.log":   (f"conv1  16->32 @{R2}x{R2} (공유 conv, TR=16)", R2 * R2),
    "conv_cosim_2.log":   (f"conv2  32->64 @{R4}x{R4} (공유 conv, TR=16)", R4 * R4),
    "pool_cosim_0.log":   (f"pool0  {ROI}x{ROI}x16", R2 * R2),
    "pool_cosim_1.log":   (f"pool1  {R2}x{R2}x32", R4 * R4),
    "pool_cosim_2.log":   (f"pool2  {R4}x{R4}x64", R8 * R8),
}

FINISH = re.compile(r"\$finish called at time\s*:\s*([\d.]+)\s*ns")
PASS   = re.compile(r"C/RTL co-simulation finished:\s*(PASS|FAIL)")


def scan(path):
    """(cycles, verdict) 또는 (None, 이유)."""
    try:
        txt = path.read_bytes().decode("utf-8", errors="replace").replace("\x00", "")
    except OSError as e:
        return None, f"읽기 실패: {e}"
    v = PASS.findall(txt)
    verdict = v[-1] if v else None
    m = FINISH.findall(txt)
    if not m:
        return None, (f"cosim {verdict}" if verdict else "미완/실패 - $finish 없음")
    ns = float(m[-1])
    # 2026-08-19 기록: `$finish` 는 **항상 半클럭**에 찍힌다(ns 가 …5 로 끝난다).
    # 그래서 `round` 의 은행가 반올림이 **패리티에 따라 방향을 바꾼다** -
    # 150893.5 -> 150894(올림), 50096.5 -> 50096(내림). 프레임 777,678 대비
    # ±3 사이클(0.0004%)이라 무의미하지만, 로그를 손으로 나눠 본 사람이
    # 1 사이클 차이를 버그로 오해하기 딱 좋다.
    #
    # **환산식을 바꾸지 말 것.** floor/ceil 로 통일하면 이 저장소의 과거
    # 비교값이 전부 몇 사이클씩 흔들려 A/B 가 어긋난다. 규약으로 고정한다.
    return int(round(ns / CLOCK_NS)), (verdict or "?")


def main():
    print(f"{'op':<44}{'사이클':>12}{'cyc/pos':>10}{'판정':>8}")
    print("-" * 74)
    any_row = False
    for name, (label, pos) in JOBS.items():
        p = LOGS / name
        if not p.exists():
            print(f"{label:<44}{'-':>12}{'-':>10}{'미실행':>8}")
            continue
        cyc, verdict = scan(p)
        if cyc is None:
            print(f"{label:<44}{'-':>12}{'-':>10}{verdict:>8}")
            continue
        any_row = True
        print(f"{label:<44}{cyc:>12,}{cyc/pos:>10.1f}{verdict:>8}")
    print()
    print("사이클은 로그의 `$finish` 시각 / 10 ns 다. 리포트가 덮이는 환경이라")
    print("이 방식을 쓴다 - 이전 세대에서 리포트와 20 사이클 이내로 일치했다.")
    print("판정이 PASS 가 아닌 행의 숫자는 **인용하지 말 것**.")
    return 0 if any_row else 1


if __name__ == "__main__":
    sys.exit(main())
