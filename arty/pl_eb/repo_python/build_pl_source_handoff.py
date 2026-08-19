#!/usr/bin/env python3
"""PL 인수인계 패키지 — **다음 PL 엔지니어**가 이어받을 것.

`build_arty_deliverable.py` 와 대상이 다르다:
  - 그쪽은 **PS** 에게 넘긴다 (비트스트림·드라이버·golden). 소스가 없다.
  - 이쪽은 **PL** 에게 넘긴다 (HLS 소스·tcl·게이트·검증 하네스·문서).

판정 기준은 하나다: **이 폴더만 있으면 작업이 되는가.**
그래서 생성 후 격리 복사본에서 게이트를 돌려 확인한다.

    python3 python/build_pl_source_handoff.py hls/arty_96_classifier
"""
import argparse
import hashlib
import shutil
import sys
from pathlib import Path

REPO = Path(__file__).resolve().parents[1]
ENGINES = ("conv_engine_tr8", "conv0_engine", "maxpool_engine")


def die(msg):
    print(f"[FAIL] {msg}")
    sys.exit(1)


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("tree")
    ap.add_argument("--out", default="arty96_pl_source_handoff")
    a = ap.parse_args()
    tree = (REPO / a.tree).resolve()
    if not (tree / "HW/classifier_net.h").exists():
        die(f"분류기 트리가 아니다: {tree}")

    out = REPO / a.out
    stage = out.parent / (out.name + ".staging")
    if stage.exists():
        shutil.rmtree(stage)
    stage.mkdir(parents=True)

    # --- 트리 안에서 가져오는 것 --------------------------------------------
    # ⚠️ `*_prj/` 와 `system/s1/` 은 **빼야 한다** (합쳐 700MB 이상이고 전부
    # 재생성물이다). 소스는 몇 MB 다. 넣으면 인계본이 못 쓸 크기가 된다.
    for rel in ("run.sh",):
        shutil.copy2(tree / rel, stage / rel)
    for d in ("HW", "SW", "python", "verif_host", "forps_golden"):
        shutil.copytree(tree / d, stage / d)
    (stage / "logs").mkdir()
    for lg in sorted((tree / "logs").glob("*.log")):
        shutil.copy2(lg, stage / "logs" / lg.name)
    for eng in ENGINES:
        src = tree / eng
        if not src.is_dir():
            die(f"엔진 디렉터리 없음: {eng}")
        (stage / eng).mkdir()
        shutil.copytree(src / "HW", stage / eng / "HW")
        shutil.copy2(src / "run_hls.tcl", stage / eng / "run_hls.tcl")
    (stage / "system").mkdir()
    tcls = sorted((tree / "system").glob("*.tcl"))
    if not tcls:
        die("system/ 에 tcl 이 없다")
    for t in tcls:
        shutil.copy2(t, stage / "system" / t.name)
    # impl 리포트는 "무엇을 재현해야 하는가"의 기준선이라 같이 보낸다
    for r in sorted((tree / "system").glob("*_arty*_v1.rpt")):
        shutil.copy2(r, stage / "system" / r.name)

    # --- 저장소 문서 (이 작업의 근거) ---------------------------------------
    (stage / "doc").mkdir()
    docs = [
        "doc/02_plans/2026-08-18_arty96-perf-campaign.md",
        "doc/06_troubleshooting/arty-classifier-log.md",
        "doc/04_team/2026-08-19_ps-request-conv0-row-stride.md",
        "CLAUDE.md",
    ]
    missing = [d for d in docs if not (REPO / d).exists()]
    if missing:
        die(f"인계할 문서가 없다: {missing}")
    for d in docs:
        shutil.copy2(REPO / d, stage / "doc" / Path(d).name)

    # 인계본을 다시 만드는 생성기 두 개도 같이 보낸다 (없으면 재생산 불가)
    (stage / "repo_python").mkdir()
    for f in ("build_arty_deliverable.py", "build_pl_source_handoff.py"):
        shutil.copy2(REPO / "python" / f, stage / "repo_python" / f)

    # --- README -------------------------------------------------------------
    n_src = sum(1 for e in ENGINES for _ in (stage / e / "HW").glob("*"))
    (stage / "README.md").write_text(f"""# PL 인수인계 — Arty Z7-20 ROI 분류기 ({tree.name})

**받는 사람: 다음 PL 엔지니어.** PS 인계본(`arty96_pl_handoff/`)과 **내용이 다릅니다** —
그쪽은 비트스트림·드라이버·golden 만 있고 **소스가 없습니다.** 이 폴더에는
HLS 소스 {n_src}개·tcl·게이트·검증 하네스·문서가 들어 있습니다.

## 이 폴더에 없는 것 (일부러 뺐습니다)

`*_prj/` (Vitis HLS 프로젝트)와 `system/s1/` (Vivado 프로젝트)는 **합쳐 700MB 이상이고
전부 재생성물**이라 뺐습니다. `bash run.sh package <엔진>` 과 `bash run.sh build` 가
다시 만듭니다.

## 처음 할 일

```bash
bash run.sh check          # 게이트. 툴 없이 돕니다
```

⚠️ **처음에는 "export 된 IP 가 모두 존재" 하나가 FAIL 합니다.** `*_prj/` 를 안 실었으니
당연하고, 아래를 돌리면 사라집니다.

```bash
bash run.sh package tr8 && bash run.sh package conv0 && bash run.sh package pool
bash run.sh build          # 합성+구현+XSA (약 25분)
bash run.sh check          # 이제 전건 통과해야 합니다
```

`system/*_arty*_v1.rpt` 가 **재현 목표**입니다. 빌드 후 WNS 와 면적이 그 리포트와
크게 다르면 무언가 바뀐 것입니다.

## 반드시 먼저 읽을 문서

| 파일 | 왜 |
|---|---|
| `doc/2026-08-18_arty96-perf-campaign.md` | **채택 3건 / 기각 5건.** 기각 목록이 더 값집니다 — 같은 벽에 다시 부딪히지 않게 합니다 |
| `doc/arty-classifier-log.md` | 트러블슈팅. 증상 → 원인 → 조치 |
| `doc/CLAUDE.md` | 이 저장소의 Gotchas (하드웨어·검사 양쪽) |
| `doc/2026-08-19_ps-request-conv0-row-stride.md` | PS 에 넘긴 요청 (프레임 약 4%) |

## 절대 재시도하지 말 것 (전부 cosim 으로 기각됨)

- `FINISH_WR` 을 행 버퍼(BRAM)로 모으기 — **+2.28%**
- `FINISH_WR` 을 oc_tile 루프 밖으로 병합 — **+16.72%**
- 출력 쓰기 4레인 UNROLL — **+33.7%** (`ofmap` m_axi 쓰기 포트가 하나라 II 1→4)
- `window` 완전 분할로 II 고치기 — II 불변, **LUT 바이트 동일**
- 꼬리 쓰기를 컴파일타임 인덱스로 — II 불변

**출력 쓰기 경로는 워드 패킹·레인 언롤 양방향이 다 닫혔습니다.**

## 남은 레버 (측정된 근거 있음)

`FUSED_SHIFT_STEP` 의 achieved II 가 2 입니다. 창 시프트가 위치당 창 전체를
물리적으로 옮기는 구조 때문이고, 순환 창(인덱스 회전)으로 바꾸면 II=1 이 가능하지만
`MAC_REDUCE` 의 읽기 인덱싱까지 재구조화해야 하고 **이득은 약 1%** 입니다.

## 이 폴더를 다시 만들려면

```bash
python3 repo_python/build_pl_source_handoff.py <트리경로>
```
""", encoding="utf-8")

    # --- CHECKSUMS (자기 자신 제외) -----------------------------------------
    lines = []
    for f in sorted(stage.rglob("*")):
        if f.is_file() and f.name != "CHECKSUMS.sha256":
            lines.append(f"{hashlib.sha256(f.read_bytes()).hexdigest()}  {f.relative_to(stage)}\n")
    (stage / "CHECKSUMS.sha256").write_text("".join(lines), encoding="utf-8")

    if out.exists():
        shutil.rmtree(out)
    stage.rename(out)
    n = sum(1 for f in out.rglob("*") if f.is_file())
    sz = sum(f.stat().st_size for f in out.rglob("*") if f.is_file())
    print(f"[OK] {out.name}: {n}파일 {sz/1e6:.1f}MB · HLS 소스 {n_src}개 · 문서 {len(docs)}편")


if __name__ == "__main__":
    main()
