# Arty Z7-20 ROI Classifier PL — DB

DB 담당자가 만든 PL 가속기다. EB 쪽은 `../pl_eb/` 를 본다.

현재 소스와 리포트는 `z7_classifier_96_hls_100mhz_dsppack.zip`의
2026-08-19 최종 DSP-pack 버전을 기준으로 한다.

`hls/` contains the source of truth for the 96x96 ROI classifier accelerator.

- Target: `xc7z020-clg400-1`
- Tool version: Vitis HLS/Vivado 2024.2
- Target clock: 100 MHz
- Logical ROI: 96x96x3
- PL input: pre-padded 98x98x3 signed INT8 NHWC
- PL output: 12x12x64 signed INT8 NHWC
- HLS cosim: 932,760 cycles
- Routed timing: WNS +0.114 ns at 100 MHz
- Routed resources: 14,380 LUT, 121 BRAM tiles, 199 DSP

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
`../../docs/contracts/ROI_CLASSIFIER_CONTRACT.md`.
