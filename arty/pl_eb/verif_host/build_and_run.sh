#!/usr/bin/env bash
# 호스트 검증 2종. Xilinx 툴 불필요 - gcc + python3 만.
#   TB-S       : 시퀀서의 s_axilite 트랜잭션 순서/값
#   golden     : PL 경계 golden 생성 + **독립 numpy 구현과 교차 검증**
# 판정은 종료 코드.
set -uo pipefail
HERE="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
CLS="$(cd "$HERE/.." && pwd)"
TMP="${TMPDIR:-/tmp}"
rc=0

echo "== TB-S: s_axilite 시퀀스"
gcc -O0 -Wall -Wextra -Wno-unused-parameter -I"$HERE/shim" \
    -o "$TMP/tb_sequencer_$$" "$HERE/tb_sequencer.c" || exit 1
"$TMP/tb_sequencer_$$" | tail -1; rc=$(( rc | ${PIPESTATUS[0]} ))
rm -f "$TMP/tb_sequencer_$$"

echo "== golden: 생성 + 교차 검증"
gcc -O2 -Wall -Wextra -o "$TMP/gen_golden_$$" "$HERE/gen_golden.c" || exit 1
"$TMP/gen_golden_$$" 42 "$CLS/forps_golden" >/dev/null || { echo "  golden 생성 실패"; rc=1; }
rm -f "$TMP/gen_golden_$$"
# manifest 를 **체크섬보다 먼저** 만든다. 예전에는 이 스크립트가 체크섬만 만들고
# manifest 는 손으로 따로 돌렸는데, 그러면 나중에 만든 manifest 가 SHA256SUMS 에
# 안 잡힌다 - 순서를 기억해야만 맞는 구조였고 2026-08-18 에 실제로 틀렸다.
python3 "$CLS/python/gen_manifest.py" > "$CLS/forps_golden/manifest.json" \
    || { echo "  manifest 생성 실패"; rc=1; }
# SHA256SUMS 는 생성 직후 여기서 만든다. 손으로 만들면 파일이 늘 때 낡는다
# (2026-08-18: manifest.json / CONTRACT.txt 가 빠진 채로 배포될 뻔했다).
# ⚠️ 이 줄은 `ls` 로 목록을 만든다 - CLAUDE.md 의 Gotcha 그대로다("자동 생성된
# 목록으로 검증하지 말 것: 잔재를 스스로 등재해 통과시킨다"). 그 위험은
# verify_golden.py §3 이 **집합 검사**로 막고 있고(잔재 2종·부분 등재·빈 목록을
# 실제 변이로 확인함), 여기서는 목록을 만들기만 한다.
#
# 2026-08-19 수정: 이 서브셸의 종료 코드가 **어디에도 누적되지 않았다.**
# sha256sum 이 실패하면 SHA256SUMS 가 잘린 채로 남고 스크립트는 rc=0 을 냈다.
if ! ( cd "$CLS/forps_golden" && ls | grep -v '^SHA256SUMS$' | sort | xargs sha256sum > SHA256SUMS ); then
    echo "  SHA256SUMS 생성 실패"; rc=1
fi
python3 "$CLS/python/verify_golden.py" "$CLS/forps_golden" | tail -1
rc=$(( rc | ${PIPESTATUS[0]} ))

exit $rc
