#!/usr/bin/env bash
#
# Arty Z7 부팅 SD 카드에 이미지를 굽는다.
#
#   ./burn_sd.sh db          db 카드에 deploy/db_sd_boot/ 를 복사
#   ./burn_sd.sh db -n       실제로 쓰지 않고 무엇을 할지만 출력
#   ./burn_sd.sh db -s DIR   소스 디렉터리를 직접 지정
#
# 대상 장치를 사람이 고르지 않는다. 최종 DB 카드의 FAT UUID로 찾는다.
#
# 파티션을 만들거나 포맷하지 않는다 — 이미 준비된 카드에 파일만 갱신한다.
# 새 카드를 처음 준비하는 절차는 README 를 본다.

set -euo pipefail

# ── 카드 UUID 대응 ──────────────────────────────────────────────────
# 카드를 다시 포맷하면 UUID 가 바뀐다. 그때 이 값을 갱신한다.
#   확인: lsblk -o NAME,SIZE,LABEL,UUID /dev/sdX
CARD_UUID="7AEA-B01B"

FILES=(BOOT.BIN boot.scr image.ub)

HERE="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"

die()  { printf '\n[실패] %s\n' "$*" >&2; exit 1; }
info() { printf '  %s\n' "$*"; }
step() { printf '\n== %s\n' "$*"; }

usage() {
  sed -n '3,16p' "${BASH_SOURCE[0]}" | sed 's/^# \{0,1\}//'
  exit "${1:-0}"
}

# ── 인자 ────────────────────────────────────────────────────────────
VARIANT=""
SRC=""
DRY=0

while (($#)); do
  case "$1" in
    db)        VARIANT="$1" ;;
    -n|--dry-run) DRY=1 ;;
    -s|--source)  SRC="${2:-}"; shift ;;
    -h|--help)    usage 0 ;;
    *) die "모르는 인자: $1  (--help 참고)" ;;
  esac
  shift
done

[[ -n $VARIANT ]] || usage 1

UUID="$CARD_UUID"
SRC="${SRC:-$HERE/${VARIANT}_sd_boot}"

# ── 소스 확인 ───────────────────────────────────────────────────────
step "소스: $SRC"
[[ -d $SRC ]] || die "소스 디렉터리가 없다: $SRC
      PetaLinux 빌드 후 images/linux/ 에서 복사했는지 확인한다."

for f in "${FILES[@]}"; do
  [[ -f $SRC/$f ]] || die "$f 가 없다: $SRC/$f"
done

declare -A SRC_MD5
for f in "${FILES[@]}"; do
  SRC_MD5[$f]=$(md5sum "$SRC/$f" | cut -d' ' -f1)
  info "$(printf '%-10s %10s  %s' "$f" "$(stat -c%s "$SRC/$f")" "${SRC_MD5[$f]}")"
done

# ── 카드 찾기 ───────────────────────────────────────────────────────
step "카드 찾는 중: $VARIANT (UUID $UUID)"
LINK="/dev/disk/by-uuid/$UUID"

if [[ ! -e $LINK ]]; then
  printf '\n[실패] %s 카드(UUID %s)가 안 보인다.\n' "$VARIANT" "$UUID" >&2
  printf '\n지금 꽂혀 있는 것:\n' >&2
  lsblk -o NAME,SIZE,TYPE,FSTYPE,LABEL,UUID,TRAN 2>/dev/null \
    | grep -Ev 'loop|^NAME.*$' | sed 's/^/  /' >&2 || true
  printf '\n카드를 바꿔 끼우거나, 다시 포맷했다면 이 스크립트 상단의\n' >&2
  printf 'CARD_UUID 를 갱신한다.\n' >&2
  exit 1
fi

PART=$(readlink -f "$LINK")
DISK="/dev/$(lsblk -no PKNAME "$PART")"
info "파티션 : $PART"
info "디스크 : $DISK"

# ── 안전 확인 ───────────────────────────────────────────────────────
step "안전 확인"

[[ $(lsblk -no TYPE "$PART") == part ]] \
  || die "$PART 는 파티션이 아니다."

REMOVABLE=$(cat "/sys/block/$(basename "$DISK")/removable" 2>/dev/null || echo 0)
[[ $REMOVABLE == 1 ]] \
  || die "$DISK 는 이동식 장치가 아니다. 내장 디스크일 수 있으므로 중단한다."
info "이동식 장치      OK"

TRAN=$(lsblk -no TRAN "$DISK" | head -1)
[[ $TRAN == usb ]] \
  || die "$DISK 의 연결 방식이 usb 가 아니다 ($TRAN). 중단한다."
info "USB 연결         OK"

PTYPE=$(udevadm info -q property -n "$PART" 2>/dev/null \
        | sed -n 's/^ID_PART_ENTRY_TYPE=//p')
[[ $PTYPE == 0xc ]] \
  || info "주의: 파티션 타입이 0xc(FAT32 LBA) 가 아니다 ($PTYPE). Zynq BootROM 이 못 읽을 수 있다."

FSTYPE=$(lsblk -no FSTYPE "$PART")
[[ $FSTYPE == vfat ]] || die "$PART 가 vfat 이 아니다 ($FSTYPE)."
info "FAT32            OK"

FREE=$(lsblk -bno FSAVAIL "$PART" 2>/dev/null || echo 0)
NEED=$(du -cb "${FILES[@]/#/$SRC/}" | tail -1 | cut -f1)
if [[ -n $FREE && $FREE != 0 ]]; then
  (( FREE > NEED )) || die "여유 공간 부족: 필요 $NEED, 남음 $FREE"
fi

if ((DRY)); then
  step "--dry-run: 여기서 멈춘다. 실제로 쓰지 않았다."
  info "쓸 대상: $PART  ($VARIANT 카드)"
  exit 0
fi

# ── 복사 ────────────────────────────────────────────────────────────
MNT=$(mktemp -d /tmp/burn_sd.XXXXXX)
cleanup() {
  mountpoint -q "$MNT" && sudo umount "$MNT" || true
  rmdir "$MNT" 2>/dev/null || true
}
trap cleanup EXIT

step "복사 중"
sudo mount "$PART" "$MNT"
for f in "${FILES[@]}"; do
  sudo cp "$SRC/$f" "$MNT/$f"
  info "$f"
done
sync
sudo umount "$MNT"

# ── 검증 (재마운트해서 페이지 캐시 우회) ────────────────────────────
step "검증 (재마운트 후 재계산)"
sudo mount "$PART" "$MNT"
FAIL=0
for f in "${FILES[@]}"; do
  got=$(md5sum "$MNT/$f" | cut -d' ' -f1)
  if [[ $got == "${SRC_MD5[$f]}" ]]; then
    info "$(printf '%-10s OK   %s' "$f" "$got")"
  else
    printf '  %-10s 불일치\n     기대 %s\n     실제 %s\n' "$f" "${SRC_MD5[$f]}" "$got" >&2
    FAIL=1
  fi
done
sudo umount "$MNT"

((FAIL == 0)) || die "검증 실패. 카드를 다시 굽는다."

step "완료 — $VARIANT 카드 ($PART)"
info "카드를 빼서 보드에 꽂고, 콘솔을 먼저 연 다음 RESET 을 누른다:"
info "  sudo picocom -b 115200 /dev/ttyUSB1"
