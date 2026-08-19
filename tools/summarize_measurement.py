#!/usr/bin/env python3
"""ADAS_MEASURE_CSV / ADAS_PS_CSV 를 읽어 보고서용 표를 만든다.

Jetson 요약 출력은 실행이 끝날 때 한 번 나오지만, 백분위수를 다시 뽑거나
두 실행(예: TCP_NODELAY on/off)을 나란히 비교하려면 CSV 를 다시 읽어야 한다.
PS 쪽은 평균·최소·최대만 찍으므로 백분위수는 여기서만 나온다.

사용법:
    python3 tools/summarize_measurement.py jetson.csv
    python3 tools/summarize_measurement.py ps.csv
    python3 tools/summarize_measurement.py --compare nodelay_off.csv nodelay_on.csv

표준 라이브러리만 쓴다 - Jetson 과 Arty PS 어디서 돌려도 의존성이 없다.
"""

import argparse
import csv
import math
import sys

# CSV 헤더로 어느 쪽 파일인지 가른다.
JETSON_COLUMNS = ["capture_us", "propose_us", "crop_us", "rtt_us",
                  "publish_us", "frame_us"]
PS_COLUMNS = ["preprocess_us", "pl_run_us", "postprocess_us", "server_us"]

# Jetson CSV 는 ROI 한 건이 한 행이라, 프레임 단위 값은 같은 프레임의 ROI 수만큼
# 반복된다. 그대로 세면 ROI 가 많은 프레임에 가중치가 붙어 중앙값이 왜곡된다.
FRAME_LEVEL_COLUMNS = {"capture_us", "propose_us", "publish_us", "frame_us"}


def percentile(sorted_values, ratio):
    """nearest-rank 백분위수. Jetson 쪽 LatencyStats 와 같은 정의를 쓴다."""
    if not sorted_values:
        return 0.0
    rank = math.ceil(ratio * len(sorted_values))
    index = max(0, min(len(sorted_values) - 1, rank - 1))
    return sorted_values[index]


def load(path):
    with open(path, newline="", encoding="utf-8") as handle:
        rows = list(csv.DictReader(handle))
    if not rows:
        raise SystemExit("행이 없다: %s" % path)
    return rows


def columns_for(rows):
    header = rows[0].keys()
    known = [name for name in JETSON_COLUMNS + PS_COLUMNS if name in header]
    if not known:
        raise SystemExit("알 수 없는 CSV 형식이다 - 헤더: %s" % ",".join(header))
    return known


def series(rows, name):
    """빈 칸은 '측정하지 않은 구간'이므로 0 으로 채우지 않고 버린다."""
    per_frame = name in FRAME_LEVEL_COLUMNS and "frame_id" in rows[0]
    seen = {}
    values = []
    for row in rows:
        text = (row.get(name) or "").strip()
        if text == "":
            continue
        value = int(text) / 1000.0   # us -> ms
        if per_frame:
            seen[row["frame_id"]] = value
        else:
            values.append(value)
    if per_frame:
        values = list(seen.values())
    values.sort()
    return values


def print_table(path, rows):
    print("\n=== %s ===" % path)
    print("  행 수: %d" % len(rows))
    print("  %-16s %7s %9s %9s %9s %9s %9s"
          % ("구간", "n", "median", "mean", "p95", "p99", "max"))
    for name in columns_for(rows):
        values = series(rows, name)
        if not values:
            continue
        mean = sum(values) / len(values)
        print("  %-16s %7d %9.3f %9.3f %9.3f %9.3f %9.3f"
              % (name, len(values), percentile(values, 0.5), mean,
                 percentile(values, 0.95), percentile(values, 0.99),
                 values[-1]))

    frames = frame_stats(rows)
    if frames is not None:
        count, span_ms = frames
        if span_ms > 0.0:
            print("  프레임 %d 건, 누적 프레임 시간 %.2f s -> %.2f FPS"
                  % (count, span_ms / 1000.0, count / (span_ms / 1000.0)))


def frame_stats(rows):
    """Jetson CSV 면 (프레임 수, 프레임 시간 합계 ms) 를 돌려준다."""
    if "frame_us" not in rows[0]:
        return None
    seen = {}
    for row in rows:
        text = (row.get("frame_us") or "").strip()
        if text == "":
            continue
        seen[row["frame_id"]] = int(text) / 1000.0
    if not seen:
        return None
    return len(seen), sum(seen.values())


def compare(paths):
    loaded = [(path, load(path)) for path in paths]
    names = []
    for _, rows in loaded:
        for name in columns_for(rows):
            if name not in names:
                names.append(name)

    print("\n=== 비교 (median / p95, ms) ===")
    header = "  %-16s" % "구간"
    for path, _ in loaded:
        header += " %22s" % path[-22:]
    print(header)
    for name in names:
        line = "  %-16s" % name
        for _, rows in loaded:
            values = series(rows, name)
            if values:
                line += " %10.3f /%10.3f" % (percentile(values, 0.5),
                                             percentile(values, 0.95))
            else:
                line += " %22s" % "-"
        print(line)
    print("")


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("csv", nargs="+", help="측정 CSV 경로")
    parser.add_argument("--compare", action="store_true",
                        help="여러 실행을 median/p95 로 나란히 비교한다")
    args = parser.parse_args()

    if args.compare:
        if len(args.csv) < 2:
            raise SystemExit("--compare 는 CSV 두 개 이상이 필요하다")
        compare(args.csv)
        return 0

    for path in args.csv:
        print_table(path, load(path))
    print("")
    return 0


if __name__ == "__main__":
    sys.exit(main())
