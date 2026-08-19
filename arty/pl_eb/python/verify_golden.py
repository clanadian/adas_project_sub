#!/usr/bin/env python3
"""golden 을 **독립 구현**으로 교차 검증한다.

verif_host/gen_golden.c 의 참조가 틀리면 PS 는 틀린 기준을 쫓게 된다. 참조가
하나뿐이면 그 자체를 검사할 방법이 없으므로, 여기서 numpy 로 다시 구현해
바이트 단위로 대조한다. 두 구현은 코드 경로를 공유하지 않는다.

    python3 python/verify_golden.py forps_golden
"""
import sys
from pathlib import Path
import numpy as np

MULT  = [1264723901, 987654321, 123456789]
SHIFT = [37, 36, 35]
LEAKY = [True, True, False]


def round_shift(x, s):
    """C 의 round_shift 와 동일: 0 에서 멀어지는 반올림, 부호 대칭."""
    if s == 0:
        return x
    half = np.int64(1) << (s - 1)
    pos = (x + half) >> s
    neg = -(((-x) + half) >> s)
    return np.where(x >= 0, pos, neg)


def conv(x, w, b, pad, leaky, mult, shift):
    """x: (H,W,IC) int8 NHWC · w: (OC,IC,3,3) int8 OIHW"""
    x = x.astype(np.int64)
    oc_n, ic_n, k, _ = w.shape
    if pad:
        x = np.pad(x, ((pad, pad), (pad, pad), (0, 0)))
    H, W, _ = x.shape
    oh, ow = H - k + 1, W - k + 1
    acc = np.empty((oh, ow, oc_n), dtype=np.int64)
    for oc in range(oc_n):
        s = np.full((oh, ow), b[oc], dtype=np.int64)
        for ky in range(k):
            for kx in range(k):
                patch = x[ky:ky + oh, kx:kx + ow, :]            # (oh,ow,ic)
                s += patch @ w[oc, :, ky, kx].astype(np.int64)  # (oh,ow)
        acc[:, :, oc] = s
    if leaky:
        acc = np.where(acc < 0, round_shift(acc * 13, 7), acc)
    return np.clip(round_shift(acc * np.int64(mult), shift), -128, 127).astype(np.int8)


def maxpool(x):
    H, W, C = x.shape
    return x.reshape(H // 2, 2, W // 2, 2, C).max(axis=(1, 3))


def rd(d, name, shape, dt=np.int8):
    a = np.fromfile(d / name, dtype=dt)
    exp = int(np.prod(shape))
    assert a.size == exp, f"{name}: {a.size} 개, {exp} 기대"
    return a.reshape(shape)


def net_consts():
    """형상을 HW/classifier_net.h 에서 읽는다. 하드코딩하면 ROI 를 바꿀 때
    버퍼는 따라오는데 검사만 안 따라와서 죽는다 - 실제로 그렇게 죽었다."""
    txt = (Path(__file__).resolve().parents[1] / "HW/classifier_net.h").read_text(
        encoding="utf-8", errors="surrogateescape")
    import re as _re
    g = lambda n: int(_re.search(rf"#define {n}\s+(\d+)", txt).group(1))
    R = g("ROI_SIZE")
    return R, R + 2 * g("CONV0_PAD")


def main(dirname):
    d = Path(dirname)
    R, P = net_consts()
    fails = []

    def ck(ok, msg):
        print(("  OK   " if ok else "  FAIL ") + msg)
        if not ok:
            fails.append(msg)

    pre = rd(d, f"in_prepad_{P}x{P}x3_int8.bin", (P, P, 3))
    roi = rd(d, f"in_roi_{R}x{R}x3_int8.bin", (R, R, 3))

    print("== 0. pre-padded 입력이 ROI 를 0 테두리로 감싼 것인가")
    ck(np.array_equal(pre[1:1+R, 1:1+R, :], roi), f"pre[1:{1+R},1:{1+R}] == ROI")
    border_ok = (pre[0].sum() == 0 and pre[P - 1].sum() == 0
                 and pre[:, 0].sum() == 0 and pre[:, P - 1].sum() == 0)
    ck(border_ok, "테두리 4변이 전부 0")

    w0 = rd(d, "w_conv0_16x3x3x3_int8.bin", (16, 3, 3, 3))
    b0 = rd(d, "b_conv0_16_int32.bin", (16,), np.int32)
    w1 = rd(d, "w_conv1_32x16x3x3_int8.bin", (32, 16, 3, 3))
    b1 = rd(d, "b_conv1_32_int32.bin", (32,), np.int32)
    w2 = rd(d, "w_conv2_64x32x3x3_int8.bin", (64, 32, 3, 3))
    b2 = rd(d, "b_conv2_64_int32.bin", (64,), np.int32)

    print("== 1. 체인 재계산 (numpy, 독립 구현)")
    a0 = conv(pre, w0, b0, 0, LEAKY[0], MULT[0], SHIFT[0])
    ck(np.array_equal(a0, rd(d, f"out_conv0_{R}x{R}x16_int8.bin", (R, R, 16))),
       f"conv0  {P}x{P}x3 -> {R}x{R}x16")
    p0 = maxpool(a0)
    ck(np.array_equal(p0, rd(d, f"out_pool0_{R//2}x{R//2}x16_int8.bin", (R//2, R//2, 16))),
       f"pool0  {R}x{R}x16 -> {R//2}x{R//2}x16")
    a1 = conv(p0, w1, b1, 1, LEAKY[1], MULT[1], SHIFT[1])
    ck(np.array_equal(a1, rd(d, f"out_conv1_{R//2}x{R//2}x32_int8.bin", (R//2, R//2, 32))),
       f"conv1  {R//2}x{R//2}x16 -> {R//2}x{R//2}x32")
    p1 = maxpool(a1)
    ck(np.array_equal(p1, rd(d, f"out_pool1_{R//4}x{R//4}x32_int8.bin", (R//4, R//4, 32))),
       f"pool1  {R//2}x{R//2}x32 -> {R//4}x{R//4}x32")
    a2 = conv(p1, w2, b2, 1, LEAKY[2], MULT[2], SHIFT[2])
    ck(np.array_equal(a2, rd(d, f"out_conv2_{R//4}x{R//4}x64_int8.bin", (R//4, R//4, 64))),
       f"conv2  {R//4}x{R//4}x32 -> {R//4}x{R//4}x64")
    p2 = maxpool(a2)
    ck(np.array_equal(p2, rd(d, f"out_pl_final_{R//8}x{R//8}x64_int8.bin", (R//8, R//8, 64))),
       f"pool2  {R//4}x{R//4}x64 -> {R//8}x{R//8}x64  (PL 최종 출력)")

    # SHA256SUMS 대조. golden 은 회귀가 매번 재생성하므로 이 검사는
    # **결정론 검증**이기도 하다 - 같은 seed 가 같은 바이트를 못 내면 여기서
    # 걸린다. PS 가 요청한 "파일별 SHA-256" 의 정본이기도 하다.
    print("== 2. SHA256SUMS 대조")
    import hashlib
    sums = d / "SHA256SUMS"
    if sums.exists():
        listed = set()
        for i, line in enumerate(sums.read_text().strip().split("\n")):
            # 2026-08-19: 빈 줄/깨진 줄에서 ValueError 로 죽어 **진단 대신
            # traceback** 이 나왔다. rc 는 1 이라 안전하지만 원인을 못 읽는다.
            parts = line.split()
            if len(parts) != 2:
                ck(False, f"SHA256SUMS {i+1}번째 줄의 형식이 깨졌다: {line!r}")
                continue
            want, name = parts
            name = name.lstrip("*")
            listed.add(name)
            f = d / name
            if not f.exists():
                ck(False, f"{name}: SHA256SUMS 에 있는데 파일이 없음")
                continue
            got = hashlib.sha256(f.read_bytes()).hexdigest()
            ck(got == want, f"{name}: sha256 일치")
        # 목록에 없는 파일이 있으면 SHA256SUMS 가 낡은 것이다
        on_disk = {f.name for f in d.iterdir() if f.name != "SHA256SUMS"}
        missing = sorted(on_disk - listed)
        ck(not missing, f"SHA256SUMS 가 모든 파일을 덮음 (빠진 것: {missing})")
    else:
        ck(False, "SHA256SUMS 없음")

    # 이름과 내용이 어긋난 파일은 위의 어느 검사에도 안 걸린다. §0/§1 은
    # net_consts() 로 만든 이름만 읽고, §2 는 SHA256SUMS 에 **등재된** 것만
    # 본다 - "있어서는 안 될 파일"은 아무도 안 본다. 실제로 ROI 를 64->128 로
    # 바꿨을 때 옛 이름(64 형상)에 새 내용(128 형상)이 담긴 파일 8개가 남았고
    # 체크섬은 내용과 맞아서 전부 통과했다. PS 가 in_roi_64x64x3 을 12,288 B
    # 로 읽으면 조용히 틀린다.
    print("== 3. 파일 이름 <-> 내용 정합")
    import re as _re
    ITEM = {"int8": 1, "int32": 4}
    for f in sorted(d.glob("*.bin")):
        m = _re.search(r"_((?:\d+x)*\d+)_(int8|int32)\.bin$", f.name)
        if not m:
            ck(False, f"{f.name}: 이름에서 형상을 못 읽는다 (형상을 이름에 넣을 것)")
            continue
        exp = ITEM[m.group(2)]
        for dim in m.group(1).split("x"):
            exp *= int(dim)
        ck(f.stat().st_size == exp,
           f"{f.name}: 이름이 뜻하는 {exp} B == 실제 {f.stat().st_size} B")

    # 이름이 스스로 일관돼도(예: 64 판 파일 한 벌이 통째로 남으면) 위 검사는
    # 통과한다. 그래서 **이 net 이 내야 할 파일 목록**과 전수 대조한다.
    expect = {f"in_prepad_{P}x{P}x3_int8.bin", f"in_roi_{R}x{R}x3_int8.bin",
              "w_conv0_16x3x3x3_int8.bin", "b_conv0_16_int32.bin",
              "w_conv1_32x16x3x3_int8.bin", "b_conv1_32_int32.bin",
              "w_conv2_64x32x3x3_int8.bin", "b_conv2_64_int32.bin",
              f"out_conv0_{R}x{R}x16_int8.bin",
              f"out_pool0_{R//2}x{R//2}x16_int8.bin",
              f"out_conv1_{R//2}x{R//2}x32_int8.bin",
              f"out_pool1_{R//4}x{R//4}x32_int8.bin",
              f"out_conv2_{R//4}x{R//4}x64_int8.bin",
              f"out_pl_final_{R//8}x{R//8}x64_int8.bin"}
    have = {f.name for f in d.glob("*.bin")}
    ck(not (have - expect), f"이 net 것이 아닌 .bin 이 없음 (군더더기: {sorted(have - expect)})")
    ck(not (expect - have), f"필요한 .bin 이 전부 있음 (빠진 것: {sorted(expect - have)})")

    print()
    if fails:
        print(f"FAILED: {len(fails)}건 - golden 을 배포하지 말 것")
        return 1
    print("GOLDEN 검증 통과: C 참조와 numpy 참조가 전 단계 비트 일치")
    return 0


if __name__ == "__main__":
    sys.exit(main(sys.argv[1] if len(sys.argv) > 1 else "forps_golden"))
