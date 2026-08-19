// ==============================================================
// Vitis HLS - High-Level Synthesis from C, C++ and OpenCL v2024.2 (64-bit)
// Tool Version Limit: 2024.11
// Copyright 1986-2022 Xilinx, Inc. All Rights Reserved.
// Copyright 2022-2024 Advanced Micro Devices, Inc. All Rights Reserved.
// 
// ==============================================================
// CTRL
// 0x00 : Control signals
//        bit 0  - ap_start (Read/Write/COH)
//        bit 1  - ap_done (Read/COR)
//        bit 2  - ap_idle (Read)
//        bit 3  - ap_ready (Read/COR)
//        bit 7  - auto_restart (Read/Write)
//        bit 9  - interrupt (Read)
//        others - reserved
// 0x04 : Global Interrupt Enable Register
//        bit 0  - Global Interrupt Enable (Read/Write)
//        others - reserved
// 0x08 : IP Interrupt Enable Register (Read/Write)
//        bit 0 - enable ap_done interrupt (Read/Write)
//        bit 1 - enable ap_ready interrupt (Read/Write)
//        others - reserved
// 0x0c : IP Interrupt Status Register (Read/TOW)
//        bit 0 - ap_done (Read/TOW)
//        bit 1 - ap_ready (Read/TOW)
//        others - reserved
// 0x10 : Data signal of ifmap
//        bit 31~0 - ifmap[31:0] (Read/Write)
// 0x14 : Data signal of ifmap
//        bit 31~0 - ifmap[63:32] (Read/Write)
// 0x18 : reserved
// 0x1c : Data signal of weights
//        bit 31~0 - weights[31:0] (Read/Write)
// 0x20 : Data signal of weights
//        bit 31~0 - weights[63:32] (Read/Write)
// 0x24 : reserved
// 0x28 : Data signal of bias
//        bit 31~0 - bias[31:0] (Read/Write)
// 0x2c : Data signal of bias
//        bit 31~0 - bias[63:32] (Read/Write)
// 0x30 : reserved
// 0x34 : Data signal of ofmap
//        bit 31~0 - ofmap[31:0] (Read/Write)
// 0x38 : Data signal of ofmap
//        bit 31~0 - ofmap[63:32] (Read/Write)
// 0x3c : reserved
// 0x40 : Data signal of img_h
//        bit 15~0 - img_h[15:0] (Read/Write)
//        others   - reserved
// 0x44 : reserved
// 0x48 : Data signal of img_w
//        bit 15~0 - img_w[15:0] (Read/Write)
//        others   - reserved
// 0x4c : reserved
// 0x50 : Data signal of requant_multiplier
//        bit 31~0 - requant_multiplier[31:0] (Read/Write)
// 0x54 : reserved
// 0x58 : Data signal of requant_shift
//        bit 7~0 - requant_shift[7:0] (Read/Write)
//        others  - reserved
// 0x5c : reserved
// 0x60 : Data signal of leaky_relu_enable
//        bit 7~0 - leaky_relu_enable[7:0] (Read/Write)
//        others  - reserved
// 0x64 : reserved
// (SC = Self Clear, COR = Clear on Read, TOW = Toggle on Write, COH = Clear on Handshake)

#define XCONV0_ENGINE_CTRL_ADDR_AP_CTRL                 0x00
#define XCONV0_ENGINE_CTRL_ADDR_GIE                     0x04
#define XCONV0_ENGINE_CTRL_ADDR_IER                     0x08
#define XCONV0_ENGINE_CTRL_ADDR_ISR                     0x0c
#define XCONV0_ENGINE_CTRL_ADDR_IFMAP_DATA              0x10
#define XCONV0_ENGINE_CTRL_BITS_IFMAP_DATA              64
#define XCONV0_ENGINE_CTRL_ADDR_WEIGHTS_DATA            0x1c
#define XCONV0_ENGINE_CTRL_BITS_WEIGHTS_DATA            64
#define XCONV0_ENGINE_CTRL_ADDR_BIAS_DATA               0x28
#define XCONV0_ENGINE_CTRL_BITS_BIAS_DATA               64
#define XCONV0_ENGINE_CTRL_ADDR_OFMAP_DATA              0x34
#define XCONV0_ENGINE_CTRL_BITS_OFMAP_DATA              64
#define XCONV0_ENGINE_CTRL_ADDR_IMG_H_DATA              0x40
#define XCONV0_ENGINE_CTRL_BITS_IMG_H_DATA              16
#define XCONV0_ENGINE_CTRL_ADDR_IMG_W_DATA              0x48
#define XCONV0_ENGINE_CTRL_BITS_IMG_W_DATA              16
#define XCONV0_ENGINE_CTRL_ADDR_REQUANT_MULTIPLIER_DATA 0x50
#define XCONV0_ENGINE_CTRL_BITS_REQUANT_MULTIPLIER_DATA 32
#define XCONV0_ENGINE_CTRL_ADDR_REQUANT_SHIFT_DATA      0x58
#define XCONV0_ENGINE_CTRL_BITS_REQUANT_SHIFT_DATA      8
#define XCONV0_ENGINE_CTRL_ADDR_LEAKY_RELU_ENABLE_DATA  0x60
#define XCONV0_ENGINE_CTRL_BITS_LEAKY_RELU_ENABLE_DATA  8

