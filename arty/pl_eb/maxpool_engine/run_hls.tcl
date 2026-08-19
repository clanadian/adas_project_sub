;# ===========================================================================
;# run_hls.tcl - ARTY CLASSIFIER maxpool_engine (2026-08-18)
;#
;# Forked from hls/zybo_pool_upsample_route/run_hls.tcl. That project built
;# THREE tops (maxpool / upsample / route_concat) out of one project with
;# three solutions; the classifier uses none of the other two, so this is a
;# single-top project and the solution loop is gone.
;#
;# Same tool quirks as the conv fork - see ../conv_engine/run_hls.tcl header
;# (export_design/cosim_design are session-local; knobs arrive via env vars
;# because vitis_hls's CLI rejects extra argv tokens).
;#
;# Usage:
;#   vitis_hls -f run_hls.tcl                        # csim + csynth
;#   RUN_HLS_COSIM=1 RUN_HLS_COSIM_OP=0 vitis_hls -f run_hls.tcl
;#   RUN_HLS_PACKAGE=1 vitis_hls -f run_hls.tcl      # + export IP
;#
;# ⚠️ NEVER run two cosims at once, and never during a Vivado implementation.
;# ===========================================================================

set PART      "xc7z020clg400-1"   ;# Arty Z7-20 == Zybo Z7-20 die.
set CLOCK_NS  10.0                ;# 100 MHz.

set RUN_COSIM   [info exists ::env(RUN_HLS_COSIM)]
set RUN_PACKAGE [info exists ::env(RUN_HLS_PACKAGE)]
;# Index into CLS_POOLS[] in the testbench. 형상은 그 배열이 정본이고
;# check_shapes.py §3 이 헤더와 순서까지 대조한다 - 여기에 적으면 트리를
;# 복사했을 때 이것만 안 따라온다.
set COSIM_OP [expr {[info exists ::env(RUN_HLS_COSIM_OP)] ? $::env(RUN_HLS_COSIM_OP) : 0}]

set SCRIPT_DIR [file dirname [file normalize [info script]]]
set HW_DIR     "$SCRIPT_DIR/HW"

open_project -reset maxpool_engine_prj
add_files      "$HW_DIR/maxpool_engine.cpp"
add_files -tb  "$HW_DIR/maxpool_engine_tb.cpp"
set_top maxpool_engine

open_solution -reset "solution1"
set_part $PART
create_clock -period $CLOCK_NS -name default
set_clock_uncertainty 2.7

puts "\n==> C Simulation - expect the three classifier pool shapes plus the\
odd-dimension extra to print PASS, then \"CLASSIFIER MAXPOOL SUITE PASS\"."
csim_design

puts "\n==> C Synthesis - report lands in\
maxpool_engine_prj/solution1/syn/report/maxpool_engine_csynth.rpt.\
\n\
Watch BRAM specifically. MAX_ROW_WORDS dropped 2048 -> 512 and MAX_IMG_W /\
MAX_CH dropped 512 -> 64, so the row-buffer path should cost roughly a\
quarter of what it did in the YOLO build. If BRAM did NOT move, the row\
buffers are not being sized from these constants and the change bought\
nothing - check maxpool_engine.cpp before assuming a win."
csynth_design

if {$RUN_COSIM} {
    set trace_opt [expr {[info exists ::env(RUN_HLS_COSIM_TRACE_ALL)] ? "all" : "none"}]
    puts "\n==> C/RTL Co-simulation - classifier pool op $COSIM_OP,\
trace_level $trace_opt. The cycle model currently carries a flat 3,000-cycle\
ALLOWANCE for each pool; this is what replaces it with a number."
    cosim_design -trace_level $trace_opt -argv "--cosim-only $COSIM_OP"
}

if {$RUN_PACKAGE} {
    puts "\n==> Package IP - export_design must follow csynth_design in this\
same invocation."
    export_design -rtl verilog -format ip_catalog
}

puts "\n==> DONE. Judge by the PASS markers above and by the exit code."

;# ⚠️ close_project + exit 은 필수다. 없으면 vitis_hls 가 스크립트를 다 돌고도
;# **대화형 프롬프트로 떨어져 죽지 않는다**(로그 끝에 `vitis_hls>` 가 반복해서
;# 찍힌다). 리포트는 이미 다 나와 있어서 "성공했는데 왜 안 끝나지"로 보이고,
;# 그 사이 다음 csynth 는 가드에 막혀 큐가 통째로 선다. 2026-08-18 에 10분을
;# 이렇게 날렸다.
close_project
exit
