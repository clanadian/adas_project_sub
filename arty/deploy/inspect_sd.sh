#!/usr/bin/env bash
# SD 카드의 파티션 테이블과 FAT 구조를 덤프한다.
#   sudo ./inspect_sd.sh /dev/sdd
# db 카드와 eb 카드를 각각 꽂고 한 번씩 돌려서 출력을 비교한다.

set -uo pipefail
DISK="${1:-/dev/sdd}"
PART="${DISK}1"

[[ $EUID -eq 0 ]] || { echo "sudo 로 실행한다: sudo $0 $DISK" >&2; exit 1; }
[[ -b $PART ]] || { echo "$PART 가 없다" >&2; exit 1; }

hr() { printf '\n──── %s\n' "$*"; }

hr "장치"
lsblk -o NAME,SIZE,TYPE,FSTYPE,LABEL,UUID "$DISK"

hr "파티션 테이블 (MBR)"
sfdisk -d "$DISK" 2>/dev/null
echo
printf 'PART_TABLE_TYPE=%s\n' "$(udevadm info -q property -n "$DISK" | sed -n 's/^ID_PART_TABLE_TYPE=//p')"
udevadm info -q property -n "$PART" | grep -E 'ID_PART_ENTRY_(TYPE|FLAGS|OFFSET|SIZE)='

hr "MBR 부트섹터 첫 16바이트 + 파티션 엔트리"
dd if="$DISK" bs=512 count=1 status=none | od -A d -t x1 -v | sed -n '1,2p;28,32p'

hr "FAT BPB (파티션 첫 섹터)"
bpb=$(dd if="$PART" bs=512 count=1 status=none | od -A n -t u1 -v | tr -s ' ' '\n' | grep -v '^$')
g() { echo "$bpb" | sed -n "$((${1}+1))p"; }               # 1바이트
g2() { echo $(( $(g $1) + 256 * $(g $(($1+1))) )); }        # 2바이트 LE
g4() { echo $(( $(g $1) + 256*$(g $(($1+1))) + 65536*$(g $(($1+2))) + 16777216*$(g $(($1+3))) )); }

printf '  %-24s %s\n' "OEM name"          "$(dd if=$PART bs=1 skip=3 count=8 status=none)"
printf '  %-24s %s\n' "bytes/sector"      "$(g2 11)"
printf '  %-24s %s\n' "sectors/cluster"   "$(g 13)"
printf '  %-24s %s\n' "reserved sectors"  "$(g2 14)"
printf '  %-24s %s\n' "number of FATs"    "$(g 16)"
printf '  %-24s %s\n' "root entries(F16)" "$(g2 17)"
printf '  %-24s %s\n' "total sectors 16"  "$(g2 19)"
printf '  %-24s %s\n' "media descriptor"  "$(printf '0x%02x' "$(g 21)")"
printf '  %-24s %s\n' "sectors/FAT (F16)" "$(g2 22)"
printf '  %-24s %s\n' "sectors/track"     "$(g2 24)"
printf '  %-24s %s\n' "heads"             "$(g2 26)"
printf '  %-24s %s\n' "hidden sectors"    "$(g4 28)"
printf '  %-24s %s\n' "total sectors 32"  "$(g4 32)"
printf '  %-24s %s\n' "sectors/FAT (F32)" "$(g4 36)"
printf '  %-24s %s\n' "root cluster"      "$(g4 44)"
printf '  %-24s %s\n' "FS type string"    "$(dd if=$PART bs=1 skip=82 count=8 status=none)"
printf '  %-24s %s\n' "boot signature"    "$(dd if=$PART bs=1 skip=510 count=2 status=none | od -A n -t x1 | tr -d ' ')"

hr "fsck.vfat -n -v (읽기 전용 점검)"
fsck.vfat -n -v "$PART" 2>&1 | head -30

hr "루트 디렉터리 내용"
M=$(mktemp -d); mount -o ro "$PART" "$M" 2>/dev/null && {
  ls -la "$M"
  echo
  echo "  md5:"; md5sum "$M"/* 2>/dev/null | sed 's/^/    /'
  umount "$M"
}; rmdir "$M" 2>/dev/null

hr "끝"
