# Arty Z7-20 ROI Classifier PL

`hls/` contains the source of truth for the 96x96 ROI classifier accelerator.

- Target: `xc7z020-clg400-1`
- Tool version: Vitis HLS/Vivado 2024.2
- Target clock: 100 MHz
- Logical ROI: 96x96x3
- PL input: pre-padded 98x98x3 signed INT8 NHWC
- PL output: 12x12x64 signed INT8 NHWC

## Layout

```text
hls/
  HW/
    classifier_engine.cpp
    classifier_engine.h
    classifier_engine_tb.cpp
  scripts/
    run_hls.tcl
    run_csynth_100mhz.tcl
reports/
  classifier_top_csynth.rpt
  classifier_top_cosim.rpt
  timing_summary_routed.rpt
  utilization_routed.rpt
```

Generated HLS/Vivado projects, caches, bitstreams and hardware handoff files are
not stored here. Distribute `.bit`, `.hwh` and `.xsa` as build artifacts.

The cross-component interface is defined in
`docs/contracts/ROI_CLASSIFIER_CONTRACT.md`.
