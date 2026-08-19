// ==============================================================
// Vitis HLS - High-Level Synthesis from C, C++ and OpenCL v2024.2 (64-bit)
// Tool Version Limit: 2024.11
// Copyright 1986-2022 Xilinx, Inc. All Rights Reserved.
// Copyright 2022-2024 Advanced Micro Devices, Inc. All Rights Reserved.
// 
// ==============================================================
// control
// 0x00 : reserved
// 0x04 : reserved
// 0x08 : reserved
// 0x0c : reserved
// 0x10 : Data signal of ifmap_padded
//        bit 31~0 - ifmap_padded[31:0] (Read/Write)
// 0x14 : Data signal of ifmap_padded
//        bit 31~0 - ifmap_padded[63:32] (Read/Write)
// 0x18 : reserved
// 0x1c : Data signal of w_conv0
//        bit 31~0 - w_conv0[31:0] (Read/Write)
// 0x20 : Data signal of w_conv0
//        bit 31~0 - w_conv0[63:32] (Read/Write)
// 0x24 : reserved
// 0x28 : Data signal of w_conv1
//        bit 31~0 - w_conv1[31:0] (Read/Write)
// 0x2c : Data signal of w_conv1
//        bit 31~0 - w_conv1[63:32] (Read/Write)
// 0x30 : reserved
// 0x34 : Data signal of w_conv2
//        bit 31~0 - w_conv2[31:0] (Read/Write)
// 0x38 : Data signal of w_conv2
//        bit 31~0 - w_conv2[63:32] (Read/Write)
// 0x3c : reserved
// 0x40 : Data signal of out_r
//        bit 31~0 - out_r[31:0] (Read/Write)
// 0x44 : Data signal of out_r
//        bit 31~0 - out_r[63:32] (Read/Write)
// 0x48 : reserved
// (SC = Self Clear, COR = Clear on Read, TOW = Toggle on Write, COH = Clear on Handshake)

#define XCLASSIFIER_TOP_CONTROL_ADDR_IFMAP_PADDED_DATA 0x10
#define XCLASSIFIER_TOP_CONTROL_BITS_IFMAP_PADDED_DATA 64
#define XCLASSIFIER_TOP_CONTROL_ADDR_W_CONV0_DATA      0x1c
#define XCLASSIFIER_TOP_CONTROL_BITS_W_CONV0_DATA      64
#define XCLASSIFIER_TOP_CONTROL_ADDR_W_CONV1_DATA      0x28
#define XCLASSIFIER_TOP_CONTROL_BITS_W_CONV1_DATA      64
#define XCLASSIFIER_TOP_CONTROL_ADDR_W_CONV2_DATA      0x34
#define XCLASSIFIER_TOP_CONTROL_BITS_W_CONV2_DATA      64
#define XCLASSIFIER_TOP_CONTROL_ADDR_OUT_R_DATA        0x40
#define XCLASSIFIER_TOP_CONTROL_BITS_OUT_R_DATA        64

// CTRL
// 0x000 : Control signals
//         bit 0  - ap_start (Read/Write/COH)
//         bit 1  - ap_done (Read/COR)
//         bit 2  - ap_idle (Read)
//         bit 3  - ap_ready (Read/COR)
//         bit 7  - auto_restart (Read/Write)
//         bit 9  - interrupt (Read)
//         others - reserved
// 0x004 : Global Interrupt Enable Register
//         bit 0  - Global Interrupt Enable (Read/Write)
//         others - reserved
// 0x008 : IP Interrupt Enable Register (Read/Write)
//         bit 0 - enable ap_done interrupt (Read/Write)
//         bit 1 - enable ap_ready interrupt (Read/Write)
//         others - reserved
// 0x00c : IP Interrupt Status Register (Read/TOW)
//         bit 0 - ap_done (Read/TOW)
//         bit 1 - ap_ready (Read/TOW)
//         others - reserved
// 0x010 : Data signal of rq_conv0
//         bit 31~0 - rq_conv0[31:0] (Read/Write)
// 0x014 : Data signal of rq_conv0
//         bit 31~0 - rq_conv0[63:32] (Read/Write)
// 0x018 : reserved
// 0x01c : Data signal of rq_conv1
//         bit 31~0 - rq_conv1[31:0] (Read/Write)
// 0x020 : Data signal of rq_conv1
//         bit 31~0 - rq_conv1[63:32] (Read/Write)
// 0x024 : reserved
// 0x028 : Data signal of rq_conv2
//         bit 31~0 - rq_conv2[31:0] (Read/Write)
// 0x02c : Data signal of rq_conv2
//         bit 31~0 - rq_conv2[63:32] (Read/Write)
// 0x030 : reserved
// 0x040 ~
// 0x07f : Memory 'b_conv0' (16 * 32b)
//         Word n : bit [31:0] - b_conv0[n]
// 0x080 ~
// 0x0ff : Memory 'b_conv1' (32 * 32b)
//         Word n : bit [31:0] - b_conv1[n]
// 0x100 ~
// 0x1ff : Memory 'b_conv2' (64 * 32b)
//         Word n : bit [31:0] - b_conv2[n]
// (SC = Self Clear, COR = Clear on Read, TOW = Toggle on Write, COH = Clear on Handshake)

#define XCLASSIFIER_TOP_CTRL_ADDR_AP_CTRL       0x000
#define XCLASSIFIER_TOP_CTRL_ADDR_GIE           0x004
#define XCLASSIFIER_TOP_CTRL_ADDR_IER           0x008
#define XCLASSIFIER_TOP_CTRL_ADDR_ISR           0x00c
#define XCLASSIFIER_TOP_CTRL_ADDR_RQ_CONV0_DATA 0x010
#define XCLASSIFIER_TOP_CTRL_BITS_RQ_CONV0_DATA 64
#define XCLASSIFIER_TOP_CTRL_ADDR_RQ_CONV1_DATA 0x01c
#define XCLASSIFIER_TOP_CTRL_BITS_RQ_CONV1_DATA 64
#define XCLASSIFIER_TOP_CTRL_ADDR_RQ_CONV2_DATA 0x028
#define XCLASSIFIER_TOP_CTRL_BITS_RQ_CONV2_DATA 64
#define XCLASSIFIER_TOP_CTRL_ADDR_B_CONV0_BASE  0x040
#define XCLASSIFIER_TOP_CTRL_ADDR_B_CONV0_HIGH  0x07f
#define XCLASSIFIER_TOP_CTRL_WIDTH_B_CONV0      32
#define XCLASSIFIER_TOP_CTRL_DEPTH_B_CONV0      16
#define XCLASSIFIER_TOP_CTRL_ADDR_B_CONV1_BASE  0x080
#define XCLASSIFIER_TOP_CTRL_ADDR_B_CONV1_HIGH  0x0ff
#define XCLASSIFIER_TOP_CTRL_WIDTH_B_CONV1      32
#define XCLASSIFIER_TOP_CTRL_DEPTH_B_CONV1      32
#define XCLASSIFIER_TOP_CTRL_ADDR_B_CONV2_BASE  0x100
#define XCLASSIFIER_TOP_CTRL_ADDR_B_CONV2_HIGH  0x1ff
#define XCLASSIFIER_TOP_CTRL_WIDTH_B_CONV2      32
#define XCLASSIFIER_TOP_CTRL_DEPTH_B_CONV2      64

