#!/usr/bin/env bash
# ===========================================================================
# run.sh - ROI classifier PL driver (Arty Z7-20)
#
#   bash run.sh status                 # 무엇이 돌고 있나 + 무엇이 만들어졌나
#   bash run.sh check                  # 형상 게이트 + TB-S + 사이클 견적 (툴 불필요)
#   bash run.sh csynth conv|tr8|conv0|pool # csim + csynth  (면적 게이트)
#   bash run.sh package conv|tr8|conv0|pool # + export IP  (Vivado 빌드 전제)
#   bash run.sh cosim conv|tr8|conv0|pool <0-2>  # 사이클 실측  ⚠️ 단독 실행만
#   bash run.sh build                  # Vivado 시스템 (합성+구현+XSA)
#   bash run.sh bd                     # BD 만 (약 3분, PS7 프리셋 검증)
#
# DRYRUN=1 을 붙이면 무거운 툴을 띄우기 **직전에** 멈춘다. 가드가 "지나가는
# 경우"를 부작용 없이 시험할 수 있는 유일한 경로다 - 실제 실행으로 시험하다가
# vivado 2개를 동시에 띄운 전례가 있다.
#
# 가드는 zybo_opt.sh 것을 그대로 가져왔다. 핵심은 **"모른다"는 "비었다"가
# 아니다** - WSL->cmd.exe 경로가 가끔 빈 출력을 주므로, "없음"은 세 번 연속
# 나와야 믿고 "있음"은 한 번이면 충분하다.
# ===========================================================================
set -uo pipefail

HERE="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
LOGS="$HERE/logs"
# 2026-08-19: REPO_WIN/WIN_SUB 를 **이 스크립트의 실제 위치에서 유도**한다.
# 예전엔 REPO_WIN 이 'C:\Users\kccistc\Documents\final_project' 로 박혀 있어서,
# 트리를 다른 곳으로 복사하면 cmd.exe 가 **원본 트리에서** 툴을 돌렸다 -
# WIN_SUB 가 같은 이유로 이미 유도식이 된 것과 정확히 같은 부류다(§4-j).
# 종료 코드는 0 이고 리포트도 나오므로 로그만 봐서는 알 수 없다.
#
# `WIN_TREE` 하나로 합친다: 이 트리의 Windows 절대경로.
# wslpath 가 없거나 실패하면 옛 하드코딩 값으로 폴백하되 **경고를 찍는다** -
# 조용히 다른 트리에서 도는 것보다 시끄러운 편이 낫다.
if command -v wslpath >/dev/null 2>&1 && WIN_TREE="$(wslpath -w "$HERE" 2>/dev/null)" \
   && [ -n "$WIN_TREE" ]; then
    :
else
    WIN_TREE='C:\Users\kccistc\Documents\final_project\hls\arty_96_classifier'
    echo "== ⚠️ wslpath 실패 - WIN_TREE 를 하드코딩 값으로 폴백: $WIN_TREE" >&2
    echo "==    이 트리가 그 경로가 아니면 **다른 트리에서 툴이 돈다.**" >&2
fi
# 2026-08-19: WSL **네이티브** 경로(/tmp, ~, /home ...)에 트리를 두면 wslpath 가
# `\\wsl.localhost\...` UNC 를 만들고, cmd.exe 는 UNC 를 현재 디렉터리로 못 쓴다:
#     UNC paths are not supported.  Defaulting to Windows directory.
# 그러면 vitis_hls 가 **C:\Windows 에서** 뜨고, 프로젝트가 없으니 아무것도 안 한 채
# **종료 코드 0** 을 낸다. run.sh 도 exit=0 을 찍는다. 산출물이 안 생긴 것만이
# 유일한 단서다 - 이 저장소가 "종료 코드로 판정하지 말라"고 적어 둔 그 상황을
# run.sh 자신이 만들고 있었다. 여기서 크게 멈춘다.
case "$WIN_TREE" in
    '\\'*)
        echo "== 중단: 이 트리가 WSL 네이티브 경로에 있다 ($HERE)" >&2
        echo "==   Windows 경로로 변환하면 UNC 가 된다: $WIN_TREE" >&2
        echo "==   cmd.exe 는 UNC 를 현재 디렉터리로 쓸 수 없어 vitis_hls/vivado 가" >&2
        echo "==   C:\\Windows 에서 뜨고 **아무것도 안 한 채 종료 코드 0** 을 낸다." >&2
        echo "==   트리를 /mnt/c/... 아래로 옮긴 뒤 다시 실행할 것." >&2
        exit 3 ;;
esac
VITIS_BAT="${VITIS_BAT:-C:\Xilinx\Vitis\2024.2\settings64.bat}"
VIVADO_BAT="${VIVADO_BAT:-C:\Xilinx\Vivado\2024.2\settings64.bat}"
mkdir -p "$LOGS"

busy() {
    local i out
    for i in 1 2 3; do
        out=$(cmd.exe /c "tasklist" 2>/dev/null | tr -d '\r')
        [ -z "$out" ] && { sleep 2; continue; }
        grep -qiE "xsimk\.exe|vitis_hls\.exe|vivado\.exe" <<<"$out" && return 0
        [ $i -lt 3 ] && sleep 2
    done
    [ -z "$out" ] && return 0     # 판단 불가 -> 도는 것으로 친다
    return 1
}

dry_stop() { if [ -n "${DRYRUN:-}" ]; then echo "== DRYRUN: 실행 직전 중단 -> $1"; exit 0; fi; }

# 무엇을 막을지는 **무엇을 띄우려는지**에 달렸다. 이 저장소의 규칙은
# "cosim 과 impl 은 절대 병렬 금지, **csynth/csim 은 1.3 GB 라 예외**" 다.
#
#   heavy  (cosim / vivado) : 아무 Xilinx 툴이라도 돌면 중단.
#                             xsimk 하나가 peak 11.2 GB 인데 호스트는 16 GB 다.
#   light  (csim / csynth)  : vivado 는 허용(문서화된 예외), 그러나
#                             **xsimk 와 vitis_hls 는 여전히 차단**한다 -
#                             xsimk 는 메모리 때문이고, vitis_hls 는
#                             `open_project -reset` 이 상대 프로젝트를 지우기
#                             때문이다(메모리가 아니라 상호 파괴 문제).
require_idle() {   # <heavy|light>
    [ -n "${DRYRUN:-}" ] && return 0
    local mode="${1:-heavy}" pat i out
    case "$mode" in
        light) pat="xsimk\.exe|vitis_hls\.exe" ;;
        *)     pat="xsimk\.exe|vitis_hls\.exe|vivado\.exe" ;;
    esac
    # 2026-08-19: tasklist 만으로는 **못 본다**. 실제로 vitis_hls(PID 20200)가
    # 돌고 있는데 `cmd.exe /c tasklist` 259줄 안에 그 PID 도 이름도 없었다
    # (WSL interop 이 다른 세션의 프로세스를 안 보여 준다). 출력이 비지 않았으니
    # 아래의 "빈 출력" 검사도 통과한다 - 즉 **가드가 조용히 열려 있었다.**
    # PowerShell Get-Process 는 같은 순간에 정확히 잡았으므로 그쪽을 1차로 쓰고,
    # tasklist 는 2차로 남긴다. 둘 중 하나라도 보이면 BUSY, 둘 다 못 읽으면 BUSY.
    local ps_out
    for i in 1 2 3; do
        ps_out=$(powershell.exe -NoProfile -Command \
                 "Get-Process | Select-Object -ExpandProperty ProcessName" \
                 2>/dev/null | tr -d '\r')
        out=$(cmd.exe /c "tasklist" 2>/dev/null | tr -d '\r')
        if [ -z "$ps_out" ] && [ -z "$out" ]; then sleep 2; continue; fi  # 모른다 != 비었다
        # Get-Process 는 이름에 .exe 가 없다. 패턴에서 확장자를 떼고 본다.
        if grep -qiE "${pat//\\.exe/}" <<<"$ps_out" || grep -qiE "$pat" <<<"$out"; then
            echo "== 중단($mode): 충돌하는 Xilinx 툴이 돌고 있다."
            printf '%s\n%s\n' "$ps_out" "$out" | grep -iE "xsimk|vitis_hls|vivado" | sort -u
            exit 1
        fi
        [ $i -lt 3 ] && sleep 2
    done
    if [ -z "$ps_out" ] && [ -z "$out" ]; then
        echo "== 중단: 두 감시 도구가 모두 빈 출력. 모르는 것은 도는 것으로 친다."
        exit 1
    fi
    if [ "$mode" = light ] && grep -qiE "vivado" <<<"$ps_out$out"; then
        echo "== 주의: vivado 가 돌고 있지만 csim/csynth 는 문서화된 예외라 진행한다."
    fi
    return 0
}

# $(engine_dir ...) 는 서브셸이라 그 안의 `exit` 은 스크립트를 못 멈춘다
# (부모는 그대로 진행해서 결국 툴을 띄운다). 그래서 값을 전역에 담고
# 반환 코드로 판정한다.
ENG_DIR=""
engine_dir() {
    case "${1:-}" in
        conv)  ENG_DIR="conv_engine" ;;      # TR=16 참고용(기각된 구성)
        tr8)   ENG_DIR="conv_engine_tr8" ;;  # **채택 구성**의 공유 conv
        conv0) ENG_DIR="conv0_engine" ;;
        pool)  ENG_DIR="maxpool_engine" ;;
        *)     echo "== 엔진은 conv | tr8 | conv0 | pool (받은 값: '${1:-}')" >&2; return 2 ;;
    esac
    return 0
}

run_hls() {   # <엔진디렉터리> <로그이름> [KEY=VAL ...]
    local dir="$1" log="$2"; shift 2
    local setvars=""
    # 환경변수는 `&&` 로 cmd.exe 에 안 넘어간다. `set VAR=val&` 처럼 & 를 값에
    # 바로 붙이는 것이 이 저장소에서 확인된 유일한 방법이다.
    for kv in "$@"; do setvars="${setvars}set ${kv}& "; done
    cmd.exe /c "call $VITIS_BAT >nul 2>&1 & ${setvars}cd /d $WIN_TREE\\$dir & vitis_hls -f run_hls.tcl" \
        > "$LOGS/$log" 2>&1
    return $?
}

case "${1:-status}" in

status)
    echo "== Windows 프로세스"
    if busy; then
        cmd.exe /c "tasklist" 2>/dev/null | tr -d '\r' | grep -iE "xsimk|vitis_hls|vivado"
        echo "   -> 도는 중. 새로 띄우지 말 것."
    else
        echo "   (Xilinx 툴 없음 - 3회 연속 확인)"
    fi
    echo
    echo "== 산출물"
    # ⚠️ 이 목록은 **손으로 쓰지 않는다.** 2026-08-18 까지 여기에 64 판 이름
    # (arty_classifier_v1.xsa, *_arty_cls_v1.rpt)과 이 트리에 없는 TR=16 포크가
    # 박혀 있어서, 있는 산출물에 "-" 를, 없는 것에 줄을 낭비하고 있었다.
    # 엔진 목록은 **run_hls.tcl 을 가진 디렉터리**에서, 시스템 산출물은 세대
    # 이름을 모르는 채로 글롭에서 얻는다.
    shopt -s nullglob
    for tcl in "$HERE"/*/run_hls.tcl; do
        d="$(basename "$(dirname "$tcl")")"
        prj=("$HERE/$d"/*_prj)
        [ ${#prj[@]} -eq 1 ] || { echo "   ?  $d: *_prj 가 ${#prj[@]}개"; continue; }
        # ⚠️ 패턴은 **작은따옴표**로 둔다. 큰따옴표+글롭이면 for 목록에서 먼저
        # 확장돼(cwd 기준) nullglob 이 통째로 지운다 - 방금 그렇게 csynth
        # 리포트가 목록에서 사라졌다.
        # 최상위 리포트만 본다. HLS top 이름은 프로젝트 디렉터리에서 유도한다
        # (conv_engine_tr8/conv_engine_prj -> top 은 conv_engine).
        top="$(basename "${prj[0]}")"; top="${top%_prj}"
        for sub in "syn/report/${top}_csynth.rpt" 'impl/ip'; do
            hit=(${prj[0]}/solution1/$sub)
            if [ ${#hit[@]} -gt 0 ]; then
                for h in "${hit[@]}"; do echo "   O  $d/${h#${prj[0]}/solution1/}"; done
            else
                echo "   -  $d/solution1/$sub"
            fi
        done
    done
    for pat in "system/*.xsa" "system/timing_impl_*.rpt" "system/utilization_impl_*.rpt"; do
        hit=("$HERE"/$pat)
        if [ ${#hit[@]} -gt 0 ]; then
            for h in "${hit[@]}"; do echo "   O  ${h#$HERE/}"; done
        else
            echo "   -  $pat  (없음)"
        fi
    done
    shopt -u nullglob
    ;;

check)
    # 순서 주의: **산출물을 먼저 만들고 그 다음에 검사**한다. 반대로 하면
    # check_shapes 가 "manifest.json 이 없다"로 exit 1 해서, 정작 manifest 를
    # 만드는 build_and_run.sh 까지 도달하지 못한다 - 새 트리가 게이트만으로는
    # 자력 부팅이 안 됐고, 128 판 때는 손으로 gen_manifest 를 돌려서 넘어갔다.
    bash "$HERE/verif_host/build_and_run.sh" | tail -3 || exit 1
    echo
    python3 "$HERE/python/check_shapes.py" || exit 1
    echo
    # 실측이 있으면 실측을 찍는다. cycle_model.py 는 ROI=64 가 박혀 있고
    # cyc/pos 를 YOLO 512폭 레이어에서 가져온 **예측기**라, 게이트가 매번
    # 597 ROI/s 를 헤드라인으로 찍었다 - 이 트리 실측은 61 ROI/s 다(10배 낙관).
    # 예측이 필요하면 python3 python/cycle_model.py 로 직접 부를 것.
    python3 "$HERE/python/roi_budget.py"
    ;;

csynth)
    engine_dir "${2:-}" || exit 2; d="$ENG_DIR"
    require_idle light; dry_stop "vitis_hls csim+csynth ($d)"
    echo "== csim + csynth: $d  (로그: $LOGS/${2}_csynth.log)"
    run_hls "$d" "${2}_csynth.log"; rc=$?
    echo "== exit=$rc"
    grep -aE "SUITE PASS|SUITE FAILED|ERROR" "$LOGS/${2}_csynth.log" | tail -5
    exit $rc
    ;;

package)
    engine_dir "${2:-}" || exit 2; d="$ENG_DIR"
    require_idle light; dry_stop "vitis_hls csynth+export_design ($d)"
    echo "== package: $d  (로그: $LOGS/${2}_package.log)"
    run_hls "$d" "${2}_package.log" "RUN_HLS_PACKAGE=1"; rc=$?
    echo "== exit=$rc"; exit $rc
    ;;

cosim)
    engine_dir "${2:-}" || exit 2; d="$ENG_DIR"
    if [ "$d" = conv0_engine ]; then
        # conv0 은 형상이 하나뿐이라 인덱스가 없다. 다만 D3 진단(2026-08-19)
        # 용으로 `HxW` 를 넘기면 그 형상으로 잰다 - 로그 이름에 형상이 들어가
        # 정본 측정(conv0_cosim_x.log)을 덮지 않는다.
        op=""
        if [ -n "${3:-}" ]; then
            if ! [[ "$3" =~ ^[0-9]+x[0-9]+$ ]]; then
                echo "== conv0 의 3번째 인자는 HxW 형식이어야 한다 (받은 값: '$3')" >&2; exit 2
            fi
            DIAG_H="${3%x*}"; DIAG_W="${3#*x}"
        fi
    else
        op="${3:-}"
        # 유효 인덱스는 **TB 의 CLS_CONVS[] 개수**에서 센다. 손으로 적으면
        # 구성을 추가할 때마다 여기만 안 따라온다(2026-08-18 에 실제로 그랬다).
        nconf=$(grep -cE '^\s*\{\s*"' "$HERE/$d/HW/conv_engine_tb.cpp")
        if ! [[ "$op" =~ ^[0-9]+$ ]] || [ "$op" -ge "$nconf" ]; then
            echo "== op 인덱스는 0..$((nconf-1)) (받은 값: '$op')" >&2; exit 2
        fi
    fi
    require_idle heavy; dry_stop "vitis_hls cosim ($d, op $op)"
    echo "== ⚠️ cosim 단독 실행. 다른 Xilinx 툴을 띄우지 말 것."
    echo "== cosim: $d op '${op}'  (로그: $LOGS/${2}_cosim_${op:-x}.log)"
    if [ -n "$op" ]; then
        run_hls "$d" "${2}_cosim_$op.log" "RUN_HLS_COSIM=1" "RUN_HLS_COSIM_OP=$op"; rc=$?
    elif [ -n "${DIAG_H:-}" ]; then
        # SKIP_CSIM=1 은 스로틀 빌드 전용이다 (csim 이 반드시 FAIL 해서 cosim 까지
        # 못 가는 경우). 채택 판정에 쓰면 안 된다 - run_hls.tcl 의 주석 참조.
        # 로그 **이름이 모드를 따라온다.** 2026-08-19 에 스로틀 실행이
        # `conv0_cosim_diag_98x98.log` 로 남았는데, 이름은 정본 형상인데 내용은
        # 데이터가 틀린 측정이었다 - 배너를 안에 찍어도 파일 이름을 먼저 보는
        # 사람은 속는다. 이 저장소에서 가장 비싼 오류 유형이라 이름으로 막는다.
        extra=(); sfx=""
        if [ -n "${SKIP_CSIM:-}" ]; then
            extra+=("RUN_HLS_SKIP_CSIM=1" "CONV0_DIAG_NOCHECK=1"); sfx="_throttle"
        fi
        run_hls "$d" "${2}_cosim_diag_${DIAG_H}x${DIAG_W}${sfx}.log" "RUN_HLS_COSIM=1" \
                "CONV0_DIAG_H=$DIAG_H" "CONV0_DIAG_W=$DIAG_W" "${extra[@]}"; rc=$?
    else
        run_hls "$d" "${2}_cosim_x.log" "RUN_HLS_COSIM=1"; rc=$?
    fi
    echo "== exit=$rc"
    grep -aE "COSIM CONFIG (PASS|FAILED)" "$LOGS/${2}_cosim_${op:-x}.log" | tail -2
    exit $rc
    ;;

bd|build)
    require_idle heavy
    mode="$1"
    [ "$mode" = bd ] && pre="set BD_ONLY=1& " || pre=""
    # ⚠️ tcl 이름을 하드코딩하지 말 것 - 트리를 복사하면 안 따라온다(WIN_SUB 와
    # 같은 부류). 글롭으로 찾고, 0개나 2개면 세는 즉시 멈춘다: "어느 것을
    # 빌드하는지 모르는 채로 40분을 돌리는 것"이 가장 비싼 실패다.
    shopt -s nullglob
    tcls=("$HERE"/system/build_*_classifier.tcl)
    shopt -u nullglob
    if [ ${#tcls[@]} -ne 1 ]; then
        echo "== 중단: system/ 의 build_*_classifier.tcl 이 ${#tcls[@]}개다 (1개여야 함)" >&2
        printf '   %s\n' "${tcls[@]}" >&2
        exit 2
    fi
    BUILD_TCL="$(basename "${tcls[0]}")"
    dry_stop "vivado -mode batch -source $BUILD_TCL (${mode})"
    echo "== vivado ${mode}  ($BUILD_TCL, 로그: $LOGS/vivado_${mode}.log)"
    cmd.exe /c "call $VIVADO_BAT >nul 2>&1 & ${pre}cd /d $WIN_TREE\\system & vivado -mode batch -source $BUILD_TCL" \
        > "$LOGS/vivado_${mode}.log" 2>&1
    rc=$?
    echo "== exit=$rc"
    # Vitis/Vivado 로그에는 NUL 바이트가 섞여 grep 이 -a 없으면 rc=1 을 준다.
    grep -aE "^== (FATAL|BD_ONLY OK|XSA|.*구현 완료)" "$LOGS/vivado_${mode}.log" | tail -5
    exit $rc
    ;;

*)
    sed -n '2,20p' "$0"; exit 2 ;;
esac
