;# ===========================================================================
;# run_hls.tcl - ARTY CLASSIFIER conv0_engine (2026-08-18)
;#
;# 원본: hls/zybo_conv0_engine/run_hls.tcl. 델타는 테스트벤치(64 MB 골든 헤더를
;# 안 씀)와 MAX_IMG_W(514 -> 66) 뿐이고 엔진 소스는 무변경입니다.
;#
;# 이 판이 답할 질문은 **DSP 하나**입니다.
;#   conv_engine(PE_OC=16) csynth DSP = 156
;#   conv0_engine          csynth DSP = 93  (KV260/Zybo 실측)
;#   합 249 > 220 (XC7Z020)  -> 그대로는 두 엔진이 못 들어갑니다.
;# MAX_IMG_W 축소는 행 버퍼만 건드리고 108-MAC 언롤
;# (K*K*IN_CH*PACK4_LANES = 3*3*3*4) 은 그대로이므로 **DSP 는 안 줄 것으로
;# 예상**하지만, 예상이지 측정이 아닙니다. 이 리포트가 정합니다.
;#
;# 도구 관련 함정은 ../conv_engine/run_hls.tcl 머리 참조 (export_design /
;# cosim_design 은 세션 로컬, 환경변수로만 인자 전달).
;#
;# 사용법:
;#   vitis_hls -f run_hls.tcl                    # csim + csynth
;#   RUN_HLS_COSIM=1 vitis_hls -f run_hls.tcl    # + cosim (ROI 64x64)
;#   RUN_HLS_PACKAGE=1 vitis_hls -f run_hls.tcl  # + export IP
;# ===========================================================================

set PART      "xc7z020clg400-1"   ;# Arty Z7-20 == Zybo Z7-20 die.
set CLOCK_NS  10.0                ;# 100 MHz.

set RUN_COSIM   [info exists ::env(RUN_HLS_COSIM)]
set RUN_PACKAGE [info exists ::env(RUN_HLS_PACKAGE)]

set SCRIPT_DIR [file dirname [file normalize [info script]]]
set HW_DIR     "$SCRIPT_DIR/HW"

open_project -reset conv0_engine_prj
add_files      "$HW_DIR/conv0_engine.cpp"
add_files -tb  "$HW_DIR/conv0_engine_tb.cpp"
set_top conv0_engine

open_solution -reset "solution1"
set_part $PART
create_clock -period $CLOCK_NS -name default
set_clock_uncertainty 2.7

;# RUN_HLS_SKIP_CSIM: **진단 전용.** 스로틀 빌드(의도적으로 데이터를 틀리게
;# 만들어 어떤 루프가 얼마를 먹는지 분리하는 빌드)는 csim 이 반드시 FAIL 하고,
;# csim 이 FAIL 하면 cosim 까지 못 가서 사이클을 못 읽는다. 그때만 켠다.
;# 기본 경로는 그대로 csim 을 돈다 - 이 저장소 규칙은 "채택 판정은 cosim 이고
;# 그 앞에 csim 비트일치가 반드시 선다" 이고, 이 스위치는 그 규칙의 예외가
;# 아니라 **측정 도구**다. 이 스위치를 켜고 나온 결과로 채택 판정을 하지 말 것.
if {[info exists ::env(RUN_HLS_SKIP_CSIM)]} {
    puts "\n==> ⚠️ C Simulation SKIPPED (RUN_HLS_SKIP_CSIM). 진단 전용 실행이다."
    puts "==>    이 실행의 결과물로 채택 판정을 하면 안 된다."
} else {
puts "\n==> C Simulation - 5개 형상이 각각 PASS 하고 마지막에\
\"CLASSIFIER CONV0 SUITE PASS\" 가 나와야 합니다."
csim_design
}

puts "\n==> C Synthesis - 리포트:\
conv0_engine_prj/solution1/syn/report/conv0_engine_csynth.rpt\
\n\
**DSP 를 먼저 볼 것.** 93 근처면 예상대로이고, conv_engine 과 합쳐 220 을\
넘으므로 conv_engine 의 PE_OC 를 낮춰야 합니다. 93 보다 크게 낮으면 예상이\
틀린 것이고 그건 좋은 소식입니다 - 어느 쪽이든 추정하지 말고 이 숫자를 쓸 것.\
\n\
BRAM 은 MAX_IMG_W 514 -> 66 만큼 크게 줄어야 정상입니다. 안 줄었으면 행\
버퍼가 이 상수에서 크기를 안 받고 있다는 뜻이니 conv0_engine.cpp 를 먼저\
볼 것."
csynth_design

if {$RUN_COSIM} {
    set trace_opt [expr {[info exists ::env(RUN_HLS_COSIM_TRACE_ALL)] ? "all" : "none"}]
    puts "\n==> C/RTL Co-simulation - ROI 64x64 (pre-padded 66x66), 위치 4,096개.\
공유 conv_engine 의 93.3 cyc/pos 와 직접 비교할 수 있는 숫자가 나옵니다\
(KV260 세대 conv0 실측은 13.1)."
    cosim_design -trace_level $trace_opt -argv "--cosim-only"
}

if {$RUN_PACKAGE} {
    puts "\n==> Package IP - export_design 은 같은 invocation 안에서\
csynth_design 뒤에 와야 합니다."
    export_design -rtl verilog -format ip_catalog
}

puts "\n==> DONE. PASS 표시와 종료 코드로 판정할 것."

;# ⚠️ close_project + exit 은 필수다. 없으면 vitis_hls 가 스크립트를 다 돌고도
;# **대화형 프롬프트로 떨어져 죽지 않는다**(로그 끝에 `vitis_hls>` 가 반복해서
;# 찍힌다). 리포트는 이미 다 나와 있어서 "성공했는데 왜 안 끝나지"로 보이고,
;# 그 사이 다음 csynth 는 가드에 막혀 큐가 통째로 선다. 2026-08-18 에 10분을
;# 이렇게 날렸다.
close_project
exit
