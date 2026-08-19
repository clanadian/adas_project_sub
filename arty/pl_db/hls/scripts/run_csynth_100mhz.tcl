# Fast 100 MHz timing/resource check.
open_project -reset classifier_prj
add_files HW/classifier_engine.cpp
add_files HW/classifier_engine.h
add_files -tb HW/classifier_engine_tb.cpp
set_top classifier_top

open_solution -reset solution1
set_part {xc7z020clg400-1}
create_clock -period 10.0 -name default
set_clock_uncertainty 3.5

csim_design
csynth_design

puts {============================================================}
puts {100MHz CSYNTH CHECK COMPLETED}
puts {report: classifier_prj/solution1/syn/report/classifier_top_csynth.rpt}
puts {Check Estimated clock, DSP, BRAM_18K, LUT and latency.}
puts {============================================================}
exit
