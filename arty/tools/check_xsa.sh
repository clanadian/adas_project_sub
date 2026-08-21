#!/usr/bin/env bash
# 들어온 XSA 가 Arty Z7-20 보드 설정을 만족하는지 검사한다.
#
#   arty/tools/check_xsa.sh ~/Downloads/classifier_z7.xsa
#
# 기준값 출처: 최종 DB XSA(보드 부팅 검증) 및 Arty Z7-20 물리 사양
# 판정 기준 문서: docs/contracts/PL_HANDOFF_CHECKLIST.md §1

set -uo pipefail
XSA="${1:-}"
[[ -f $XSA ]] || { echo "사용법: $0 <XSA 경로>" >&2; exit 2; }

# 보드 물리 사양 — 협의 대상 아님
declare -a REQUIRED=(
  "CRYSTAL_PERIPHERAL_FREQMHZ|50"
  "UIPARAM_DDR_PARTNO|MT41J256M16 RE-125"
  "UIPARAM_DDR_BUS_WIDTH|16 Bit"
  "UIPARAM_DDR_DRAM_WIDTH|16 Bits"
  "UIPARAM_DDR_DEVICE_CAPACITY|4096 MBits"
  "UIPARAM_DDR_ROW_ADDR_COUNT|15"
  "UART0_PERIPHERAL_ENABLE|1"
  "UART0_UART0_IO|MIO 14 .. 15"
  "SD0_PERIPHERAL_ENABLE|1"
  "SD0_SD0_IO|MIO 40 .. 45"
)

hwh=$(unzip -l "$XSA" 2>/dev/null | grep -oE '[^ ]+\.hwh' | head -1)
[[ -n $hwh ]] || { echo "XSA 안에서 .hwh 를 못 찾았다: $XSA" >&2; exit 2; }

vals=$(unzip -p "$XSA" "$hwh" 2>/dev/null | tr '<' '\n' \
       | grep -oE 'PARAMETER NAME="PCW_[^"]*" VALUE="[^"]*"' \
       | sed 's/PARAMETER NAME="PCW_//; s/" VALUE="/=/; s/"$//' | sort -u)

printf '검사 대상 : %s\n' "$XSA"
printf 'hwh       : %s\n\n' "$hwh"

fail=0
for spec in "${REQUIRED[@]}"; do
  key=${spec%%|*}; want=${spec#*|}
  got=$(printf '%s\n' "$vals" | grep "^${key}=" | head -1 | cut -d= -f2-)
  if [[ $got == "$want" ]]; then
    printf '  OK    %-34s %s\n' "$key" "$got"
  else
    printf '  FAIL  %-34s 기대=%-22s 실제=%s\n' "$key" "$want" "${got:-<없음>}"
    fail=$((fail+1))
  fi
done

# PLL 분주비는 크리스탈에서 파생되므로 같이 확인한다
echo
xtal=$(printf '%s\n' "$vals" | grep '^CRYSTAL_PERIPHERAL_FREQMHZ=' | cut -d= -f2)
for p in ARMPLL DDRPLL IOPLL; do
  fb=$(printf '%s\n' "$vals" | grep "^${p}_CTRL_FBDIV=" | cut -d= -f2)
  [[ -n ${fb:-} && -n ${xtal:-} ]] && \
    printf '  참고  %-12s FBDIV=%-4s → %.1f MHz\n' "$p" "$fb" "$(echo "$xtal * $fb" | bc -l)"
done

echo
if ((fail == 0)); then
  echo "통과 — 보드 설정 이상 없음. PetaLinux 재빌드로 진행한다."
  exit 0
else
  echo "미달 $fail 건 — 반려한다. docs/contracts/PL_HANDOFF_CHECKLIST.md §1 참조."
  exit 1
fi
