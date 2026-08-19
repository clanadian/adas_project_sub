#!/usr/bin/env python3
"""ROI 사이클 예산 — **cosim 로그에서 직접 읽는다**.

cycle_model.py 와 다른 점: 저기는 다른 세대·다른 형상의 cyc/pos 를 외삽하는
**예측기**고, 여기는 이 저장소가 이 형상으로 실제로 돌린 cosim 결과를 읽어
합치는 **집계기**다. 값이 없으면 '없다'고 말하지 추정으로 메우지 않는다.

이 저장소는 하루에 프레임 숫자가 네 번 틀린 적이 있고 넷 다 값이 아니라
**출처와 범위가 섞여서** 생겼다. 그래서 행마다 출처를 찍는다.

    python3 python/roi_budget.py
"""
import sys
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent))
import re  # noqa: E402
from collect_cosim import scan, LOGS, CLOCK_NS, ROI, PRE, R2, R4, R8, ROOT  # noqa: E402

CLOCK_HZ = 1e9 / CLOCK_NS

# 채택 구성의 op -> 그 op 를 측정한 cosim 로그
# 라벨은 ROI_SIZE 에서 유도한다. 박아두면 128 트리에서 128 사이클에 64 라벨이
# 붙는다 - 값이 맞아서 제일 안 보이는 종류의 오류다 (2026-08-18 에 실제로 그랬다).
ADOPTED = [
    (f"conv0  3->16 @{PRE}x{PRE} pre-pad", "conv0_cosim_x.log"),
    (f"pool0  {ROI}x{ROI}x16",             "pool_cosim_0.log"),
    (f"conv1  16->32 @{R2}x{R2}",          "tr8_cosim_1.log"),
    (f"pool1  {R2}x{R2}x32",               "pool_cosim_1.log"),
    (f"conv2  32->64 @{R4}x{R4}",          "tr8_cosim_2.log"),
    (f"pool2  {R4}x{R4}x64",               "pool_cosim_2.log"),
]

# 폴백 구성(conv0_engine 없이 공유 엔진으로 첫 층까지): 비교용
FALLBACK_FIRST = (f"conv0  3->16 @{ROI}x{ROI} (공유 conv)", "tr8_cosim_0.log")


def collect(rows):
    total, missing = 0, []
    print(f"{'op':<32}{'사이클':>12}  출처")
    print("-" * 62)
    for label, log in rows:
        cyc, verdict = scan(LOGS / log)
        if cyc is None or verdict != "PASS":
            print(f"{label:<32}{'-':>12}  ❌ {verdict}  ({log})")
            missing.append(label)
            continue
        total += cyc
        print(f"{label:<32}{cyc:>12,}  cosim PASS ({log})")
    return total, missing


def main():
    print("=" * 62)
    # ⚠️ 라벨을 손으로 쓰지 않는다. 2026-08-18 에 TR 8->4 / PE_OC 16->32 로
    # 바꾼 뒤에도 이 줄은 "TR=8" 을 찍고 있었다 - 숫자는 맞는데 **어느 구성의
    # 숫자인지**가 틀리는, 이 저장소에서 가장 비싼 종류의 오류다.
    _ce = (ROOT / "conv_engine_tr8/HW/conv_engine.h").read_text(
        encoding="utf-8", errors="surrogateescape")
    def _c(n):
        m = re.search(rf"^const unsigned {n}\s*=\s*(\d+);", _ce, re.M)
        return m.group(1) if m else "?"
    _tr, _pe, _g = _c("TR"), _c("PE_OC"), _c("OC_GROUP_TILES")
    # DSP 도 리포트에서 읽는다. 없으면 "미측정" 이라고 쓴다(추정치를 찍지 않는다).
    # impl 리포트가 있으면 그것이 정본이다 - csynth 합은 엔진만 세고
    # SmartConnect 몫을 빠뜨려 이 저장소에서 매번 10 안팎 적게 나온다.
    # 이름을 박지 않는다 - 트리를 복사하면 안 따라온다(§4-j).
    _impl_hits = sorted((ROOT / "system").glob("utilization_impl_*.rpt"))
    _impl = _impl_hits[0] if len(_impl_hits) == 1 else None
    _dsp_s = None
    if _impl is not None and _impl.exists():
        _m = re.search(r"^\| DSPs\s*\|\s*(\d+)\s*\|", _impl.read_text(
            encoding="utf-8", errors="surrogateescape"), re.M)
        if _m:
            _dsp_s = f"DSP {_m.group(1)}/220 (impl 실측)"
    _dsp = []
    for _e, _top in (("conv_engine_tr8", "conv_engine"),
                     ("conv0_engine", "conv0_engine"),
                     ("maxpool_engine", "maxpool_engine")):
        _r = ROOT / _e / f"{_top}_prj/solution1/syn/report/{_top}_csynth.rpt"
        if not _r.exists():
            _dsp = None; break
        _m = re.search(r"^\|Total\s*\|\s*\d+\|\s*(\d+)\|", _r.read_text(
            encoding="utf-8", errors="surrogateescape"), re.M)
        _dsp.append(int(_m.group(1)) if _m else 0)
    if _dsp_s is None:
        _dsp_s = f"DSP {sum(_dsp)}/220 (csynth 추정)" if _dsp else "DSP 미측정"
    print(f"채택 구성: conv_engine(TR={_tr}, PE_OC={_pe}, G={_g})"
          " + conv0_engine + maxpool_engine")
    print(f"{_dsp_s} · @100 MHz")
    print("=" * 62)
    total, missing = collect(ADOPTED)
    print("-" * 62)
    if missing:
        print(f"⚠️ {len(missing)}개 op 이 아직 실측되지 않았다: {', '.join(missing)}")
        print("   아래 합계는 **불완전**하다. 인용하지 말 것.")
    ms = total / CLOCK_HZ * 1e3
    tag = "부분합" if missing else "**전부 실측**"
    print(f"{'ROI 1개 ' + tag:<32}{total:>12,}  = {ms:.2f} ms")
    if not missing:
        print(f"{'처리량':<32}{'':>12}  {1000/ms:.0f} ROI/s")
        print()
        print("프레임당 ROI 개수별 FPS (FPGA 만 병목일 때):")
        for n in (5, 8, 10, 15, 20):
            print(f"    {n:>2} ROI/frame -> {1000/(ms*n):6.1f} FPS")

    # 폴백 비교
    cyc, verdict = scan(LOGS / FALLBACK_FIRST[1])
    if cyc is not None and verdict == "PASS" and not missing:
        c0, _ = scan(LOGS / ADOPTED[0][1])
        if c0:
            fb = total - c0 + cyc
            print()
            print("conv0_engine 을 안 쓰면 (공유 conv 가 첫 층까지):")
            print(f"    ROI {fb:,} 사이클 = {fb/CLOCK_HZ*1e3:.2f} ms "
                  f"({fb/total:.2f}배)  -> 10 ROI/frame = {1000/(fb/CLOCK_HZ*1e3*10):.1f} FPS")
            print(f"    conv0_engine 이 버는 것: {cyc - c0:,} 사이클/ROI")
    print()
    print("⚠️ 이 표에는 PS 몫(GAP/FC/softmax), Ethernet 전송, 커널 시작")
    print("   s_axilite 왕복이 **들어 있지 않다**. PL 사이클만이다.")
    return 0


if __name__ == "__main__":
    sys.exit(main())
