# Arty Z7-20 96x96 classifier - 100 MHz timing-focused build
# Full flow: C simulation -> synthesis -> C/RTL co-simulation -> Vivado IP export.

open_project -reset classifier_prj
add_files HW/classifier_engine.cpp
add_files HW/classifier_engine.h
add_files -tb HW/classifier_engine_tb.cpp
set_top classifier_top

open_solution -reset solution1
set_part {xc7z020clg400-1}
create_clock -period 10.0 -name default
# 100 MHz remains the exported clock.  Increase scheduling margin beyond the
# Vitis default (2.70 ns at 10 ns) so downstream Vivado place/route has more room.
set_clock_uncertainty 3.5

puts {============================================================}
puts {[1/4] C simulation: deterministic software-golden comparison}
puts {============================================================}
csim_design

puts {============================================================}
puts {[2/4] C synthesis: CHECK DSP/BRAM/TIMING BEFORE COSIM}
puts {============================================================}
csynth_design

puts {============================================================}
puts {[3/4] C/RTL co-simulation (Verilog / XSIM)}
puts {============================================================}
cosim_design -rtl verilog

puts {============================================================}
puts {[4/4] Export Vivado IP Catalog}
puts {============================================================}
export_design -format ip_catalog

puts {============================================================}
puts {96x96 100MHz TIMING FLOW COMPLETED}
puts {csynth: classifier_prj/solution1/syn/report/classifier_top_csynth.rpt}
puts {cosim  : classifier_prj/solution1/sim/report/classifier_top_cosim.rpt}
puts {IP     : classifier_prj/solution1/impl/}
puts {============================================================}
exit
