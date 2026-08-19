# ===========================================================================
# build_arty96_classifier.tcl - ROI 분류기 **96x96**, Arty Z7-20 (2026-08-18)
#
# hls/arty_classifier/ (64x64) 와 **같은 설계**이고 형상 상수만 다르다:
#   conv 64->96 · conv0 66->98 · maxpool 64->96 (MAX_ROW_WORDS 512->768)
#
# 2026-08-19 정정: 이 트리는 128 판을 복사해 만들었고, 위 문단이 **128 판의
# 서사를 그대로 이고 있었다** - "MAX_IMG_W 만 두 배", "위치 수가 4배라
# 사이클도 대략 4배", "**BRAM 이 이 판의 위험 자원**". 96 판에서는 셋 다
# 틀리다: 폭은 1.5배, 위치는 2.25배(사이클 실측 2.15배), 그리고 **BRAM 은
# 35.71% 로 한가하고 위험 자원은 Slice(98.32%)** 다.
# 숫자만 갱신하고 산문을 안 고치면 이렇게 남는다 - 값이 맞아서 안 보인다.
# ===========================================================================
# 원본: hls/zybo_system/build_arty_system_v1_boot.tcl (YOLOv3-tiny-ADAS 4엔진).
# 델타는 **엔진 목록과 IP 저장소 두 곳뿐**이고 PS7 프리셋·SmartConnect·주소
# 할당·타이밍 전략·XSA 인자는 전부 그대로다.
#
#   엔진 4개 -> 3개 : conv_engine(TR=8) + conv0_engine + maxpool_engine.
#                     upsample_engine 과 route_concat_engine 은 분류기에
#                     해당 op 가 없어서 뺐다. 대신 conv0_engine 이 들어왔다 -
#                     첫 conv 가 ROI 사이클의 59% 인데 공유 엔진으로는 못 줄여서
#                     (in_ch=3 이라 TR 레인이 놀고 out_ch=16 이라 oc_tile 1개),
#                     전용 엔진이 93.3 -> 13.1 cyc/pos 를 낸다.
#   m_axi 9개 -> 7개 : conv 3(RD_BUS/RD_BUS2/WR_BUS) + conv0 2 + maxpool 2.
#
# 이 빌드가 실행하는 망은 HW/classifier_net.h 에 정의돼 있다. 엔진 소스는
# YOLO 판과 같고 MAX_* 상수만 분류기 크기(64/64/64)로 줄었다.
#
# ---------------------------------------------------------------------------
# 이 빌드로 답하려는 질문 2개
# ---------------------------------------------------------------------------
#  1) 면적. 엔진 2개 + 축소된 MAX_* 가 얼마나 돌려주나. v7_boot 실측이
#     LUT 42,554(80.0%) / Slice 12,940(97.29%) / DSP 184(83.6%) / BRAM 108
#     이었다. 여기서 크게 내려오면 **PE_OC 16 -> 32** 실험이 열린다
#     (분류기는 out_ch 가 64 에서 끝나므로 oc_tile 통과가 절반이 된다).
#     열리는지 아닌지는 이 리포트가 정한다 - 추정하지 말 것.
#  2) 타이밍. Slice 압박이 풀리면 100 MHz 마진이 늘어난다. 늘어난 만큼을
#     클럭 상향에 쓸지는 **다음 판**의 문제다. 이 판은 100 MHz 로 닫는다.
#
# ---------------------------------------------------------------------------
# 캐리어 보드 주의 (원본 파일 머리에서 가져옴, 여전히 유효)
# ---------------------------------------------------------------------------
#   - DDR 버스 폭 16 bit (Zybo 32 bit) -> 외부 DRAM 대역폭 절반. **실보드 위험**
#   - DDR 512 MB, 콘솔은 UART0 MIO 14..15 (Zybo 는 UART1 MIO 48..49)
#   - 프리셋이 HP0 을 끄므로 **source 순서가 필수** (프리셋 파일 주석 참조)
#
# ⚠️ Vivado 를 2개 동시에 띄우지 말 것. 실행 전에 tasklist 로 확인하고,
#    UNKNOWN 이면 BUSY 로 취급할 것.
#
# 사용법:
#   BD_ONLY=1 vivado -mode batch -source build_arty96_classifier.tcl  # 약 3분
#   vivado -mode batch -source build_arty96_classifier.tcl            # 전체
#   (저장소 밖에서는 `bash run.sh bd` / `bash run.sh build` 가 가드까지 건다)
# ===========================================================================
set SCRIPT_DIR [file normalize [file dirname [info script]]]
set HLS_DIR    [file normalize $SCRIPT_DIR/..]
# ⚠️ 프로젝트/BD 이름은 **짧아야 한다.** Windows 경로 한계 260 바이트다.
#
# Vivado 가 HLS IP 를 풀면 다음 같은 경로가 나온다:
#   <repo>/hls/arty_96_classifier/system/<PROJ>/<PROJ>.gen/sources_1/bd/<BD>/
#     ip/<BD>_conv_engine_0_0/hdl/verilog/
#     conv_engine_conv_engine_Pipeline_FUSED_SHIFT_STEP_p_ZZL16scan_and_computePK7ap_uintILi32Ebkb.dat
#
# 끝의 .dat 이름만 96 바이트다(HLS 가 C++ 맹글링을 파일명에 쓴다). PROJ 는
# **두 번**, BD 는 **세 번** 들어간다.
#
# 2026-08-18: PROJ=arty_cls_sys1 / BD=arty_cls_system 으로 272 바이트가 되어
# `ERROR: [Common 17-680] Path length exceeds 260-Byte maximum` 로 죽었다.
# 참고로 YOLO 판(hls/zybo_system/arty_sys1_boot, BD zybo_system)은 255 로
# **여유가 5 바이트뿐**이었다 - 우연히 통과한 것이지 안전했던 게 아니다.
#
# 현재: PROJ=s1 / BD=cls -> 226 바이트 (여유 34).
# 이름을 늘리기 전에 python/check_shapes.py 의 경로 길이 검사를 볼 것.
set PROJ_NAME  s1
;# BD 만 만들고 멈추는 모드. 503개 프리셋 파라미터를 Vivado 가 받아들이는지
;# 2시간짜리 impl 앞에서 3분에 확인한다.
;#
;# 환경변수 **또는** 센티넬 파일로 켠다. 둘 다 받는 이유: WSL 의 export 는
;# cmd.exe 로 그냥 넘어가지 않는다(WSLENV 에 없으면 사라진다). 이 저장소는
;# 이미 그 함정에 빠져 `set VAR=6&` 로 값에 & 를 붙이는 우회를 쓰고 있는데,
;# 그러면 런처마다 호출 형태가 달라진다. 파일은 어느 런처에서도 똑같이 보인다.
set BD_ONLY [expr {[info exists ::env(BD_ONLY)]
                   || [file exists $SCRIPT_DIR/BD_ONLY]}]
if {$BD_ONLY} {
    puts "== BD_ONLY 모드 (BD 생성 + PS7 프리셋 검증까지만, 합성/구현 없음)"
}
set BD_NAME    cls
set PART       xc7z020clg400-1
# 100 MHz 는 **제약**이고, 실제 목표는 83 MHz 다 (HLS 추정 Fmax x 이 프로젝트의
# 실측 HLS->routed 비율 0.60). 일부러 목표보다 빡빡하게 걸어서 되돌려줄 마진을
# 만든다. WNS 가 음수로 나오면 그때 83 MHz 로 낮춰 다시 돌린다.
set CLK_MHZ    100

# ---- IP 저장소 2개 -------------------------------------------------------
# $HLS_DIR 은 이 스크립트의 부모다 - 하드코딩이 아니라 [info script] 에서
# 유도하므로 트리를 복사해도 따라온다. 이 트리에서는 hls/arty_96_classifier/.
# 두 엔진 모두 RUN_HLS_PACKAGE=1 로 export_design 을 먼저 돌려야 생긴다.
set IP_REPOS [list \
    "$HLS_DIR/conv_engine_tr8/conv_engine_prj/solution1/impl/ip" \
    "$HLS_DIR/conv0_engine/conv0_engine_prj/solution1/impl/ip" \
    "$HLS_DIR/maxpool_engine/maxpool_engine_prj/solution1/impl/ip" \
]

# ⚠️ 공유 conv 는 **conv_engine_tr8** 이지 conv_engine 이 아니다.
#    TR=16 판은 conv0_engine 과 합치면 DSP 256 > 220 으로 못 들어간다.
#    csynth 실측: conv(TR=16) 156 / conv(TR=8) 92 / conv0 90 / maxpool 10.
#    채택 구성 = 92 + 90 + 10 = 192.
#    두 포크의 IP 이름이 **둘 다 conv_engine** 이므로, 저장소 경로를 잘못
#    가리키면 이름 충돌 없이 조용히 틀린 IP 가 잡힌다. 경로를 확인할 것.

# 게이트: 저장소가 하나라도 없으면 여기서 죽는다. 없는 채로 진행하면 위 함정 1)
# 그대로 40분 뒤에 엉뚱한 에러로 죽는다.
foreach r $IP_REPOS {
    if {![file isdirectory $r]} {
        puts "== FATAL: IP 저장소 없음: $r"
        puts "==        먼저 패키징할 것: RUN_HLS_PACKAGE=1 vitis_hls -f <engine>/run_hls.tcl"
        exit 1
    }
}
puts "== IP 저장소 [llength $IP_REPOS]개 확인 OK"

create_project $PROJ_NAME $SCRIPT_DIR/$PROJ_NAME -part $PART -force
set_property ip_repo_paths $IP_REPOS [current_project]
update_ip_catalog -rebuild

create_bd_design $BD_NAME
current_bd_design $BD_NAME

# ---- Zynq-7000 PS ---------------------------------------------------------
set ps [create_bd_cell -type ip -vlnv [get_ipdefs -filter {NAME == "processing_system7"}] ps7_0]
apply_bd_automation -rule xilinx.com:bd_rule:processing_system7 \
    -config {make_external "FIXED_IO, DDR" apply_board_preset "0" Master "Disable" Slave "Disable"} \
    [get_bd_cells ps7_0]

# ---- 실제 Zybo Z7-20 PS7 설정 (v7_boot 의 유일한 변경점) ------------------
# 순서가 중요하다: 프리셋이 먼저, 설계 고유 설정(아래 M_AXI_GP0/HP0/FCLK0)이
# 나중이어야 설계 값이 이긴다. 파일 머리에 근거가 있다.
source $SCRIPT_DIR/arty_ps7_preset_z7_20.tcl

# HP0 하나만 쓴다. 마스터가 11개인데 HP 포트는 4개뿐이지만, 포트를 늘릴 이유가
# 없다: op 가 엄격히 순차 실행이라 여러 마스터가 동시에 HP 를 물지 않는다
# (2026-08-08 에 "HP 경합" 전제가 거짓임을 확인하고 P6 을 폐기한 그 근거).
# 엔진을 동시 실행하도록 SW 를 바꾸면 그때 다시 볼 것.
set_property -dict [list \
    CONFIG.PCW_USE_M_AXI_GP0            {1} \
    CONFIG.PCW_USE_S_AXI_HP0            {1} \
    CONFIG.PCW_S_AXI_HP0_DATA_WIDTH     {64} \
    CONFIG.PCW_FPGA0_PERIPHERAL_FREQMHZ $CLK_MHZ \
] [get_bd_cells ps7_0]

# ---- HLS IP 2개 -----------------------------------------------------------
# {BD 인스턴스명  IP 이름  m_axi 번들 목록}
# upsample_engine / route_concat_engine 은 분류기에 해당 op 가 없어서 없다.
set ENGINES {
    {conv_engine_0        conv_engine        {RD_BUS RD_BUS2 WR_BUS}}
    {conv0_engine_0       conv0_engine       {RD_BUS WR_BUS}}
    {maxpool_engine_0     maxpool_engine     {RD_BUS WR_BUS}}
}

set masters {}
foreach e $ENGINES {
    lassign $e inst name bundles
    set vlnv [get_ipdefs -filter "NAME == \"$name\" && VLNV =~ \"xilinx.com:hls:*\""]
    if {$vlnv eq ""} {
        puts "== FATAL: IP 카탈로그에서 $name 을 못 찾았다. 패키징이 안 됐거나 저장소 경로가 틀렸다."
        exit 1
    }
    create_bd_cell -type ip -vlnv $vlnv $inst
    foreach b $bundles { lappend masters "$inst/m_axi_$b" }
    puts "== 배치: $inst ($name), m_axi [llength $bundles]개"
}
puts "== m_axi 마스터 총 [llength $masters]개"

# ---- m_axi 11개 -> SmartConnect -> HP0 ------------------------------------
# HP0 는 64-bit, 엔진 포트는 전부 32-bit 다. SmartConnect 가 폭 변환을 한다.
set sc [create_bd_cell -type ip -vlnv [get_ipdefs -filter {NAME == "smartconnect"}] smartconnect_hp0]
set_property -dict [list CONFIG.NUM_SI [llength $masters] CONFIG.NUM_MI {1}] [get_bd_cells $sc]

set i 0
foreach m $masters {
    connect_bd_intf_net [get_bd_intf_pins $m] \
        [get_bd_intf_pins [format "%s/S%02d_AXI" $sc $i]]
    incr i
}
connect_bd_intf_net [get_bd_intf_pins $sc/M00_AXI] [get_bd_intf_pins ps7_0/S_AXI_HP0]

# ---- s_axilite 5개 <- M_AXI_GP0 (automation 이 인터커넥트를 만들어 준다) ---
foreach e $ENGINES {
    lassign $e inst name bundles
    apply_bd_automation -rule xilinx.com:bd_rule:axi4 \
        -config {Master "/ps7_0/M_AXI_GP0" Clk "Auto"} \
        [get_bd_intf_pins $inst/s_axi_CTRL]
}

# ---- 클럭/리셋: automation 이 만든 FCLK_CLK0 망에 나머지를 붙인다 ---------
# 함정 4): 이미 물린 핀을 다시 연결하면 에러다. 반드시 확인 후 연결.
set clk_net [get_bd_nets -of_objects [get_bd_pins ps7_0/FCLK_CLK0]]
set clk_src [get_bd_pins -of_objects $clk_net -filter {DIR == O}]
set rst_cell [get_bd_cells -quiet -filter {VLNV =~ "*proc_sys_reset*"}]
set rst_src  [get_bd_pins -quiet $rst_cell/peripheral_aresetn]

proc wire_if_free {pin src} {
    if {[llength [get_bd_pins -quiet $pin]] == 0} { return }
    if {[llength [get_bd_nets -quiet -of_objects [get_bd_pins $pin]]] == 0} {
        connect_bd_net $src [get_bd_pins $pin]
    }
}

foreach e $ENGINES {
    lassign $e inst name bundles
    wire_if_free "$inst/ap_clk"   $clk_src
    wire_if_free "$inst/ap_rst_n" $rst_src
}
wire_if_free "$sc/aclk"           $clk_src
wire_if_free "$sc/aresetn"        $rst_src
wire_if_free "ps7_0/S_AXI_HP0_ACLK" $clk_src

# ---- 인터럽트: 안 붙인다. SW 가 ap_done 을 폴링한다 (모든 세대 동일) ------

assign_bd_address
validate_bd_design
save_bd_design

# ---- BD_ONLY: 여기서 멈춘다 (약 3분) --------------------------------------
# PS7 이 실제로 무엇으로 설정됐는지 찍는다. "프리셋을 source 했다"가 아니라
# "BD 안의 PS7 이 이 값을 들고 있다"를 확인하는 것이 목적이다.
if {$BD_ONLY} {
    puts "\n== BD_ONLY: PS7 최종 설정 확인"
    foreach p {PCW_UIPARAM_DDR_PARTNO PCW_UIPARAM_DDR_MEMORY_TYPE \
               PCW_UIPARAM_DDR_DRAM_WIDTH PCW_UIPARAM_DDR_DEVICE_CAPACITY \
               PCW_UIPARAM_DDR_ROW_ADDR_COUNT PCW_UART1_PERIPHERAL_ENABLE \
               PCW_UART1_UART1_IO PCW_SD0_PERIPHERAL_ENABLE \
               PCW_QSPI_PERIPHERAL_ENABLE PCW_ENET0_PERIPHERAL_ENABLE \
               PCW_USE_M_AXI_GP0 PCW_USE_S_AXI_HP0 PCW_S_AXI_HP0_DATA_WIDTH \
               PCW_FPGA0_PERIPHERAL_FREQMHZ PCW_CLK0_FREQ \
               PCW_CRYSTAL_PERIPHERAL_FREQMHZ PCW_APU_PERIPHERAL_FREQMHZ \
               PCW_UIPARAM_DDR_FREQ_MHZ} {
        puts [format "==   %-34s %s" $p \
              [get_property CONFIG.$p [get_bd_cells ps7_0]]]
    }
    ;# FCLK0 이 100 이 아니면 타이밍 제약이 바뀐 것이고, v5_tr16 의 WNS
    ;# +0.007 ns 와는 비교 자체가 성립하지 않는다. 여기서 죽는 게 맞다.
    set f [get_property CONFIG.PCW_FPGA0_PERIPHERAL_FREQMHZ [get_bd_cells ps7_0]]
    if {$f != $CLK_MHZ} {
        puts "== FATAL: FCLK0 = $f MHz, 기대값 $CLK_MHZ MHz."
        puts "==        프리셋이 클럭을 덮었다. 타이밍 비교가 무의미해진다."
        exit 1
    }
    ;# EMIO 로 새면 PL 핀이 생겨 넷리스트가 달라진다 - 그러면 "PL 무변경"이
    ;# 거짓이 된다. 최상위 포트 목록으로 직접 확인한다.
    set emio [get_bd_intf_ports -quiet -filter {NAME =~ "*EMIO*" || NAME =~ "*emio*"}]
    if {[llength $emio] > 0} {
        puts "== FATAL: EMIO 포트가 생겼다: $emio"
        exit 1
    }
    puts "== BD_ONLY OK: FCLK0 $f MHz, EMIO 포트 0개. 합성/구현은 돌리지 않았다."
    exit 0
}

# ---- 래퍼 + 합성/구현 -----------------------------------------------------
# 함정 2): BD 최상위 HDL 은 synth/ 에 나온다.
set bd_file [get_files $BD_NAME.bd]
generate_target all $bd_file
make_wrapper -files $bd_file -top -import
set_property top ${BD_NAME}_wrapper [current_fileset]
update_compile_order -fileset sources_1

launch_runs synth_1 -jobs 4
wait_on_run synth_1
if {[get_property PROGRESS [get_runs synth_1]] != "100%"} {
    puts "== FATAL: 합성 실패"
    exit 1
}

# 합성 직후 면적을 먼저 찍는다. impl 이 실패해도 LUT/DSP/BRAM 판정은 남는다 -
# 이 빌드의 1차 목적이 투영 검증이기 때문이다.
open_run synth_1 -name synth_1
report_utilization -file $SCRIPT_DIR/utilization_synth_arty96_v1.rpt
puts "== 합성 후 면적 리포트: $SCRIPT_DIR/utilization_synth_arty96_v1.rpt"

;# 기본 전략은 이 설계에서 이미 실패했다(v5_tr16 1차: WNS −0.156 ns, 23
;# endpoints). 같은 실패를 다시 사지 않는다 - retime_v5_tr16.tcl 이 +0.163 을
;# 회수한 그 전략으로 처음부터 간다.
set_property strategy Performance_ExploreWithRemap [get_runs impl_1]
puts "== impl 전략: Performance_ExploreWithRemap (기본 전략은 이 설계에서 실패 실적)"

launch_runs impl_1 -to_step write_bitstream -jobs 4
wait_on_run impl_1
if {[get_property PROGRESS [get_runs impl_1]] != "100%"} {
    puts "== 경고: impl 미완료. 합성 리포트는 유효하다."
    exit 1
}

open_run impl_1
report_utilization      -file $SCRIPT_DIR/utilization_impl_arty96_v1.rpt
report_timing_summary   -file $SCRIPT_DIR/timing_impl_arty96_v1.rpt
puts "== arty96_v1 구현 완료. 리포트: utilization_impl_arty96_v1.rpt / timing_impl_arty96_v1.rpt"
;# ⚠️ 이 줄이 찍는 값은 WNS 가 아니다. `-delay_type min_max` 가 min 경로를
;# 돌려줄 수 있어서 실제로는 **WHS** 가 나온다 - v4 에서 이 줄은 0.014 를
;# 찍었는데 진짜 WNS 는 0.045 였다 (v4 기준). 인용할 값은 timing 리포트의
;# Design Timing Summary 다.
puts "== (아래는 WHS 일 수 있음 - WNS 는 timing_impl_arty96_v1.rpt 를 볼 것):\
[get_property SLACK [get_timing_paths -delay_type min_max]]"

;# ---- XSA: 이 빌드의 존재 이유다 ------------------------------------------
;# v5_tr16 의 XSA 는 부팅 불가였다(파일 머리 참조). 이 XSA 가 그걸 대체한다.
;# retime_v5_tr16.tcl 의 write_hw_platform 과 같은 인자를 쓴다 - -fixed 는
;# 고정 플랫폼(PL 재구성 없음)이고 -include_bit 는 비트스트림을 XSA 에 넣어
;# 따로 들고 다니지 않게 한다.
write_hw_platform -fixed -include_bit -force \
    -file $SCRIPT_DIR/arty96_classifier_v1.xsa
puts "== XSA: $SCRIPT_DIR/arty96_classifier_v1.xsa"
puts "==      이 XSA 는 Arty Z7-20 의 PS7 설정(DDR3 16-bit 512MB, UART0"
puts "==      MIO 14..15, SD0, ENET0)을 들고 있다. PS 담당은 이걸로 플랫폼을"
puts "==      만들 것. 레지스터 맵은 KR260/Zybo 판과 동일하고 베이스 주소만"
puts "==      확인하면 된다."
exit
