;# ===========================================================================
;# run_hls.tcl - ARTY CLASSIFIER conv_engine **TR=8 포크** (2026-08-18)
;#
;# Forked from hls/zybo_conv_engine_tr16/run_hls.tcl. Same part, same clock,
;# same tool quirks; the delta is the testbench (no 64 MB YOLO data header)
;# and the cosim target selector (a classifier op index, not a REAL_LAYERS
;# index).
;#
;# Tool quirks carried over verbatim from the fork source - do not "clean up":
;#   - export_design needs csynth_design to have run in THIS SAME vitis_hls
;#     invocation. So does cosim_design. A separate `vitis_hls -f` run that
;#     only calls cosim_design fails immediately: the "synthesis succeeded"
;#     check is session-local, not on-disk.
;#   - vitis_hls's own CLI rejects extra argv tokens, which is why every knob
;#     below arrives through an ENVIRONMENT VARIABLE. cosim_design's -argv is
;#     a different thing: a real passthrough to the compiled testbench's argv.
;#
;# Usage:
;#   vitis_hls -f run_hls.tcl                        # csim + csynth
;#   RUN_HLS_COSIM=1 RUN_HLS_COSIM_OP=0 vitis_hls -f run_hls.tcl
;#   RUN_HLS_PACKAGE=1 vitis_hls -f run_hls.tcl      # + export IP
;#
;# ⚠️ NEVER run two cosims at once, and never during a Vivado implementation.
;#    One xsimk peaks at 11.2 GB on this design's parent and the host has 16.
;#    (feedback: HLS cosim은 절대 병렬 금지)
;# ===========================================================================

set PART      "xc7z020clg400-1"   ;# Arty Z7-20. Identical die/package/speed
                                  ;# grade to Zybo Z7-20, which is why every
                                  ;# measured number from the Zybo campaign
                                  ;# transfers. Board files are irrelevant to
                                  ;# HLS - only Vivado's PS7 preset cares.
set CLOCK_NS  10.0                ;# 100 MHz.

set RUN_COSIM   [info exists ::env(RUN_HLS_COSIM)]
set RUN_PACKAGE [info exists ::env(RUN_HLS_PACKAGE)]
;# Which classifier conv to cosim: index into CLS_CONVS[] in the testbench
;# 인덱스->형상은 TB 의 CLS_CONVS[] 가 정본이고 check_shapes.py §3 이 순서까지
;# 대조한다. 여기에 형상을 적어두면 트리를 복사했을 때 그것만 안 따라온다.
set COSIM_OP [expr {[info exists ::env(RUN_HLS_COSIM_OP)] ? $::env(RUN_HLS_COSIM_OP) : 0}]

set SCRIPT_DIR [file dirname [file normalize [info script]]]
set HW_DIR     "$SCRIPT_DIR/HW"

open_project -reset conv_engine_prj
add_files      "$HW_DIR/conv_engine.cpp"
add_files -tb  "$HW_DIR/conv_engine_tb.cpp"
set_top conv_engine

open_solution -reset "solution1"
set_part $PART
create_clock -period $CLOCK_NS -name default

;# 2.7 ns = the tool's own 27% default at CLOCK_NS=10.0, stated explicitly
;# rather than overridden. The parent's 2.0 ns override was bought to fix one
;# measured 200 MHz routing failure on KV260; there is no equivalent
;# measurement here and raising uncertainty costs LUT.
;#
;# CHECK AFTER THE RUN: if csynth's Estimated lands at exactly
;# 10.00 - 2.70 = 7.30 ns, the scheduler spent the entire budget and 7.30 is a
;# ceiling, not a margin - raise this to 3.5-4.0 and re-run.
set_clock_uncertainty 2.7

puts "\n==> C Simulation - expect the three classifier conv shapes plus two\
extras to each print PASS, then \"CLASSIFIER CONV SUITE PASS\"."
csim_design

puts "\n==> C Synthesis - report lands in\
conv_engine_prj/solution1/syn/report/conv_engine_csynth.rpt.\
\n\
THIS IS THE AREA GATE. The classifier build has only TWO engines (this one\
and maxpool), not the YOLO build's four, and MAX_IMG_W/MAX_IN_CH/MAX_OUT_CH\
are cut from 512/128/1024 to 64/64/64. Both should show up as BRAM relief.\
\n\
READ THE LUT NUMBER CAREFULLY. On KV260 this design's csynth LUT\
overestimated real impl by 1.4-1.5x. That ratio is a KV260 measurement and\
has NOT been re-established for 7-series - do not divide by 1.45 and call the\
result a fit. Percentages are against XC7Z020 and are directly meaningful.\
\n\
If area comes back with real headroom, the experiment to run next is PE_OC\
16 -> 32 (conv_engine.h line 251): the classifier's out_ch tops out at 64, so\
PE_OC=32 halves the oc_tile passes. It is a MEASUREMENT, not a given - PE_OC\
16->24 blew the LUT budget once already on KV260."
csynth_design

if {$RUN_COSIM} {
    ;# -trace_level none: xsim is single-threaded and waveform capture on this
    ;# design dominates runtime. Set RUN_HLS_COSIM_TRACE_ALL=1 to override
    ;# when a cycle count needs explaining rather than just measuring.
    set trace_opt [expr {[info exists ::env(RUN_HLS_COSIM_TRACE_ALL)] ? "all" : "none"}]
    puts "\n==> C/RTL Co-simulation - classifier conv op $COSIM_OP,\
trace_level $trace_opt. THIS is what replaces the cycle PREDICTION in\
python/cycle_model.py with a measurement. Expect \"COSIM CONFIG PASS\"."
    cosim_design -trace_level $trace_opt -argv "--cosim-only $COSIM_OP"
}

if {$RUN_PACKAGE} {
    puts "\n==> Package IP - export_design must follow csynth_design in this\
same invocation (see header)."
    export_design -rtl verilog -format ip_catalog
}

puts "\n==> DONE. Judge by the PASS markers above and by the exit code, not\
by the absence of errors in the log."

;# ⚠️ close_project + exit 은 필수다. 없으면 vitis_hls 가 스크립트를 다 돌고도
;# **대화형 프롬프트로 떨어져 죽지 않는다**(로그 끝에 `vitis_hls>` 가 반복해서
;# 찍힌다). 리포트는 이미 다 나와 있어서 "성공했는데 왜 안 끝나지"로 보이고,
;# 그 사이 다음 csynth 는 가드에 막혀 큐가 통째로 선다. 2026-08-18 에 10분을
;# 이렇게 날렸다.
close_project
exit
