// ==============================================================
// Vitis HLS - High-Level Synthesis from C, C++ and OpenCL v2024.2 (64-bit)
// Tool Version Limit: 2024.11
// Copyright 1986-2022 Xilinx, Inc. All Rights Reserved.
// Copyright 2022-2024 Advanced Micro Devices, Inc. All Rights Reserved.
// 
// ==============================================================
/***************************** Include Files *********************************/
#include "xconv_engine.h"

/************************** Function Implementation *************************/
#ifndef __linux__
int XConv_engine_CfgInitialize(XConv_engine *InstancePtr, XConv_engine_Config *ConfigPtr) {
    Xil_AssertNonvoid(InstancePtr != NULL);
    Xil_AssertNonvoid(ConfigPtr != NULL);

    InstancePtr->Ctrl_BaseAddress = ConfigPtr->Ctrl_BaseAddress;
    InstancePtr->IsReady = XIL_COMPONENT_IS_READY;

    return XST_SUCCESS;
}
#endif

void XConv_engine_Start(XConv_engine *InstancePtr) {
    u32 Data;

    Xil_AssertVoid(InstancePtr != NULL);
    Xil_AssertVoid(InstancePtr->IsReady == XIL_COMPONENT_IS_READY);

    Data = XConv_engine_ReadReg(InstancePtr->Ctrl_BaseAddress, XCONV_ENGINE_CTRL_ADDR_AP_CTRL) & 0x80;
    XConv_engine_WriteReg(InstancePtr->Ctrl_BaseAddress, XCONV_ENGINE_CTRL_ADDR_AP_CTRL, Data | 0x01);
}

u32 XConv_engine_IsDone(XConv_engine *InstancePtr) {
    u32 Data;

    Xil_AssertNonvoid(InstancePtr != NULL);
    Xil_AssertNonvoid(InstancePtr->IsReady == XIL_COMPONENT_IS_READY);

    Data = XConv_engine_ReadReg(InstancePtr->Ctrl_BaseAddress, XCONV_ENGINE_CTRL_ADDR_AP_CTRL);
    return (Data >> 1) & 0x1;
}

u32 XConv_engine_IsIdle(XConv_engine *InstancePtr) {
    u32 Data;

    Xil_AssertNonvoid(InstancePtr != NULL);
    Xil_AssertNonvoid(InstancePtr->IsReady == XIL_COMPONENT_IS_READY);

    Data = XConv_engine_ReadReg(InstancePtr->Ctrl_BaseAddress, XCONV_ENGINE_CTRL_ADDR_AP_CTRL);
    return (Data >> 2) & 0x1;
}

u32 XConv_engine_IsReady(XConv_engine *InstancePtr) {
    u32 Data;

    Xil_AssertNonvoid(InstancePtr != NULL);
    Xil_AssertNonvoid(InstancePtr->IsReady == XIL_COMPONENT_IS_READY);

    Data = XConv_engine_ReadReg(InstancePtr->Ctrl_BaseAddress, XCONV_ENGINE_CTRL_ADDR_AP_CTRL);
    // check ap_start to see if the pcore is ready for next input
    return !(Data & 0x1);
}

void XConv_engine_EnableAutoRestart(XConv_engine *InstancePtr) {
    Xil_AssertVoid(InstancePtr != NULL);
    Xil_AssertVoid(InstancePtr->IsReady == XIL_COMPONENT_IS_READY);

    XConv_engine_WriteReg(InstancePtr->Ctrl_BaseAddress, XCONV_ENGINE_CTRL_ADDR_AP_CTRL, 0x80);
}

void XConv_engine_DisableAutoRestart(XConv_engine *InstancePtr) {
    Xil_AssertVoid(InstancePtr != NULL);
    Xil_AssertVoid(InstancePtr->IsReady == XIL_COMPONENT_IS_READY);

    XConv_engine_WriteReg(InstancePtr->Ctrl_BaseAddress, XCONV_ENGINE_CTRL_ADDR_AP_CTRL, 0);
}

void XConv_engine_Set_ifmap(XConv_engine *InstancePtr, u64 Data) {
    Xil_AssertVoid(InstancePtr != NULL);
    Xil_AssertVoid(InstancePtr->IsReady == XIL_COMPONENT_IS_READY);

    XConv_engine_WriteReg(InstancePtr->Ctrl_BaseAddress, XCONV_ENGINE_CTRL_ADDR_IFMAP_DATA, (u32)(Data));
    XConv_engine_WriteReg(InstancePtr->Ctrl_BaseAddress, XCONV_ENGINE_CTRL_ADDR_IFMAP_DATA + 4, (u32)(Data >> 32));
}

u64 XConv_engine_Get_ifmap(XConv_engine *InstancePtr) {
    u64 Data;

    Xil_AssertNonvoid(InstancePtr != NULL);
    Xil_AssertNonvoid(InstancePtr->IsReady == XIL_COMPONENT_IS_READY);

    Data = XConv_engine_ReadReg(InstancePtr->Ctrl_BaseAddress, XCONV_ENGINE_CTRL_ADDR_IFMAP_DATA);
    Data += (u64)XConv_engine_ReadReg(InstancePtr->Ctrl_BaseAddress, XCONV_ENGINE_CTRL_ADDR_IFMAP_DATA + 4) << 32;
    return Data;
}

void XConv_engine_Set_weights(XConv_engine *InstancePtr, u64 Data) {
    Xil_AssertVoid(InstancePtr != NULL);
    Xil_AssertVoid(InstancePtr->IsReady == XIL_COMPONENT_IS_READY);

    XConv_engine_WriteReg(InstancePtr->Ctrl_BaseAddress, XCONV_ENGINE_CTRL_ADDR_WEIGHTS_DATA, (u32)(Data));
    XConv_engine_WriteReg(InstancePtr->Ctrl_BaseAddress, XCONV_ENGINE_CTRL_ADDR_WEIGHTS_DATA + 4, (u32)(Data >> 32));
}

u64 XConv_engine_Get_weights(XConv_engine *InstancePtr) {
    u64 Data;

    Xil_AssertNonvoid(InstancePtr != NULL);
    Xil_AssertNonvoid(InstancePtr->IsReady == XIL_COMPONENT_IS_READY);

    Data = XConv_engine_ReadReg(InstancePtr->Ctrl_BaseAddress, XCONV_ENGINE_CTRL_ADDR_WEIGHTS_DATA);
    Data += (u64)XConv_engine_ReadReg(InstancePtr->Ctrl_BaseAddress, XCONV_ENGINE_CTRL_ADDR_WEIGHTS_DATA + 4) << 32;
    return Data;
}

void XConv_engine_Set_weights_hi(XConv_engine *InstancePtr, u64 Data) {
    Xil_AssertVoid(InstancePtr != NULL);
    Xil_AssertVoid(InstancePtr->IsReady == XIL_COMPONENT_IS_READY);

    XConv_engine_WriteReg(InstancePtr->Ctrl_BaseAddress, XCONV_ENGINE_CTRL_ADDR_WEIGHTS_HI_DATA, (u32)(Data));
    XConv_engine_WriteReg(InstancePtr->Ctrl_BaseAddress, XCONV_ENGINE_CTRL_ADDR_WEIGHTS_HI_DATA + 4, (u32)(Data >> 32));
}

u64 XConv_engine_Get_weights_hi(XConv_engine *InstancePtr) {
    u64 Data;

    Xil_AssertNonvoid(InstancePtr != NULL);
    Xil_AssertNonvoid(InstancePtr->IsReady == XIL_COMPONENT_IS_READY);

    Data = XConv_engine_ReadReg(InstancePtr->Ctrl_BaseAddress, XCONV_ENGINE_CTRL_ADDR_WEIGHTS_HI_DATA);
    Data += (u64)XConv_engine_ReadReg(InstancePtr->Ctrl_BaseAddress, XCONV_ENGINE_CTRL_ADDR_WEIGHTS_HI_DATA + 4) << 32;
    return Data;
}

void XConv_engine_Set_bias(XConv_engine *InstancePtr, u64 Data) {
    Xil_AssertVoid(InstancePtr != NULL);
    Xil_AssertVoid(InstancePtr->IsReady == XIL_COMPONENT_IS_READY);

    XConv_engine_WriteReg(InstancePtr->Ctrl_BaseAddress, XCONV_ENGINE_CTRL_ADDR_BIAS_DATA, (u32)(Data));
    XConv_engine_WriteReg(InstancePtr->Ctrl_BaseAddress, XCONV_ENGINE_CTRL_ADDR_BIAS_DATA + 4, (u32)(Data >> 32));
}

u64 XConv_engine_Get_bias(XConv_engine *InstancePtr) {
    u64 Data;

    Xil_AssertNonvoid(InstancePtr != NULL);
    Xil_AssertNonvoid(InstancePtr->IsReady == XIL_COMPONENT_IS_READY);

    Data = XConv_engine_ReadReg(InstancePtr->Ctrl_BaseAddress, XCONV_ENGINE_CTRL_ADDR_BIAS_DATA);
    Data += (u64)XConv_engine_ReadReg(InstancePtr->Ctrl_BaseAddress, XCONV_ENGINE_CTRL_ADDR_BIAS_DATA + 4) << 32;
    return Data;
}

void XConv_engine_Set_ofmap(XConv_engine *InstancePtr, u64 Data) {
    Xil_AssertVoid(InstancePtr != NULL);
    Xil_AssertVoid(InstancePtr->IsReady == XIL_COMPONENT_IS_READY);

    XConv_engine_WriteReg(InstancePtr->Ctrl_BaseAddress, XCONV_ENGINE_CTRL_ADDR_OFMAP_DATA, (u32)(Data));
    XConv_engine_WriteReg(InstancePtr->Ctrl_BaseAddress, XCONV_ENGINE_CTRL_ADDR_OFMAP_DATA + 4, (u32)(Data >> 32));
}

u64 XConv_engine_Get_ofmap(XConv_engine *InstancePtr) {
    u64 Data;

    Xil_AssertNonvoid(InstancePtr != NULL);
    Xil_AssertNonvoid(InstancePtr->IsReady == XIL_COMPONENT_IS_READY);

    Data = XConv_engine_ReadReg(InstancePtr->Ctrl_BaseAddress, XCONV_ENGINE_CTRL_ADDR_OFMAP_DATA);
    Data += (u64)XConv_engine_ReadReg(InstancePtr->Ctrl_BaseAddress, XCONV_ENGINE_CTRL_ADDR_OFMAP_DATA + 4) << 32;
    return Data;
}

void XConv_engine_Set_img_h(XConv_engine *InstancePtr, u32 Data) {
    Xil_AssertVoid(InstancePtr != NULL);
    Xil_AssertVoid(InstancePtr->IsReady == XIL_COMPONENT_IS_READY);

    XConv_engine_WriteReg(InstancePtr->Ctrl_BaseAddress, XCONV_ENGINE_CTRL_ADDR_IMG_H_DATA, Data);
}

u32 XConv_engine_Get_img_h(XConv_engine *InstancePtr) {
    u32 Data;

    Xil_AssertNonvoid(InstancePtr != NULL);
    Xil_AssertNonvoid(InstancePtr->IsReady == XIL_COMPONENT_IS_READY);

    Data = XConv_engine_ReadReg(InstancePtr->Ctrl_BaseAddress, XCONV_ENGINE_CTRL_ADDR_IMG_H_DATA);
    return Data;
}

void XConv_engine_Set_img_w(XConv_engine *InstancePtr, u32 Data) {
    Xil_AssertVoid(InstancePtr != NULL);
    Xil_AssertVoid(InstancePtr->IsReady == XIL_COMPONENT_IS_READY);

    XConv_engine_WriteReg(InstancePtr->Ctrl_BaseAddress, XCONV_ENGINE_CTRL_ADDR_IMG_W_DATA, Data);
}

u32 XConv_engine_Get_img_w(XConv_engine *InstancePtr) {
    u32 Data;

    Xil_AssertNonvoid(InstancePtr != NULL);
    Xil_AssertNonvoid(InstancePtr->IsReady == XIL_COMPONENT_IS_READY);

    Data = XConv_engine_ReadReg(InstancePtr->Ctrl_BaseAddress, XCONV_ENGINE_CTRL_ADDR_IMG_W_DATA);
    return Data;
}

void XConv_engine_Set_in_ch(XConv_engine *InstancePtr, u32 Data) {
    Xil_AssertVoid(InstancePtr != NULL);
    Xil_AssertVoid(InstancePtr->IsReady == XIL_COMPONENT_IS_READY);

    XConv_engine_WriteReg(InstancePtr->Ctrl_BaseAddress, XCONV_ENGINE_CTRL_ADDR_IN_CH_DATA, Data);
}

u32 XConv_engine_Get_in_ch(XConv_engine *InstancePtr) {
    u32 Data;

    Xil_AssertNonvoid(InstancePtr != NULL);
    Xil_AssertNonvoid(InstancePtr->IsReady == XIL_COMPONENT_IS_READY);

    Data = XConv_engine_ReadReg(InstancePtr->Ctrl_BaseAddress, XCONV_ENGINE_CTRL_ADDR_IN_CH_DATA);
    return Data;
}

void XConv_engine_Set_out_ch(XConv_engine *InstancePtr, u32 Data) {
    Xil_AssertVoid(InstancePtr != NULL);
    Xil_AssertVoid(InstancePtr->IsReady == XIL_COMPONENT_IS_READY);

    XConv_engine_WriteReg(InstancePtr->Ctrl_BaseAddress, XCONV_ENGINE_CTRL_ADDR_OUT_CH_DATA, Data);
}

u32 XConv_engine_Get_out_ch(XConv_engine *InstancePtr) {
    u32 Data;

    Xil_AssertNonvoid(InstancePtr != NULL);
    Xil_AssertNonvoid(InstancePtr->IsReady == XIL_COMPONENT_IS_READY);

    Data = XConv_engine_ReadReg(InstancePtr->Ctrl_BaseAddress, XCONV_ENGINE_CTRL_ADDR_OUT_CH_DATA);
    return Data;
}

void XConv_engine_Set_k(XConv_engine *InstancePtr, u32 Data) {
    Xil_AssertVoid(InstancePtr != NULL);
    Xil_AssertVoid(InstancePtr->IsReady == XIL_COMPONENT_IS_READY);

    XConv_engine_WriteReg(InstancePtr->Ctrl_BaseAddress, XCONV_ENGINE_CTRL_ADDR_K_DATA, Data);
}

u32 XConv_engine_Get_k(XConv_engine *InstancePtr) {
    u32 Data;

    Xil_AssertNonvoid(InstancePtr != NULL);
    Xil_AssertNonvoid(InstancePtr->IsReady == XIL_COMPONENT_IS_READY);

    Data = XConv_engine_ReadReg(InstancePtr->Ctrl_BaseAddress, XCONV_ENGINE_CTRL_ADDR_K_DATA);
    return Data;
}

void XConv_engine_Set_stride(XConv_engine *InstancePtr, u32 Data) {
    Xil_AssertVoid(InstancePtr != NULL);
    Xil_AssertVoid(InstancePtr->IsReady == XIL_COMPONENT_IS_READY);

    XConv_engine_WriteReg(InstancePtr->Ctrl_BaseAddress, XCONV_ENGINE_CTRL_ADDR_STRIDE_DATA, Data);
}

u32 XConv_engine_Get_stride(XConv_engine *InstancePtr) {
    u32 Data;

    Xil_AssertNonvoid(InstancePtr != NULL);
    Xil_AssertNonvoid(InstancePtr->IsReady == XIL_COMPONENT_IS_READY);

    Data = XConv_engine_ReadReg(InstancePtr->Ctrl_BaseAddress, XCONV_ENGINE_CTRL_ADDR_STRIDE_DATA);
    return Data;
}

void XConv_engine_Set_pad(XConv_engine *InstancePtr, u32 Data) {
    Xil_AssertVoid(InstancePtr != NULL);
    Xil_AssertVoid(InstancePtr->IsReady == XIL_COMPONENT_IS_READY);

    XConv_engine_WriteReg(InstancePtr->Ctrl_BaseAddress, XCONV_ENGINE_CTRL_ADDR_PAD_DATA, Data);
}

u32 XConv_engine_Get_pad(XConv_engine *InstancePtr) {
    u32 Data;

    Xil_AssertNonvoid(InstancePtr != NULL);
    Xil_AssertNonvoid(InstancePtr->IsReady == XIL_COMPONENT_IS_READY);

    Data = XConv_engine_ReadReg(InstancePtr->Ctrl_BaseAddress, XCONV_ENGINE_CTRL_ADDR_PAD_DATA);
    return Data;
}

void XConv_engine_Set_requant_multiplier(XConv_engine *InstancePtr, u32 Data) {
    Xil_AssertVoid(InstancePtr != NULL);
    Xil_AssertVoid(InstancePtr->IsReady == XIL_COMPONENT_IS_READY);

    XConv_engine_WriteReg(InstancePtr->Ctrl_BaseAddress, XCONV_ENGINE_CTRL_ADDR_REQUANT_MULTIPLIER_DATA, Data);
}

u32 XConv_engine_Get_requant_multiplier(XConv_engine *InstancePtr) {
    u32 Data;

    Xil_AssertNonvoid(InstancePtr != NULL);
    Xil_AssertNonvoid(InstancePtr->IsReady == XIL_COMPONENT_IS_READY);

    Data = XConv_engine_ReadReg(InstancePtr->Ctrl_BaseAddress, XCONV_ENGINE_CTRL_ADDR_REQUANT_MULTIPLIER_DATA);
    return Data;
}

void XConv_engine_Set_requant_shift(XConv_engine *InstancePtr, u32 Data) {
    Xil_AssertVoid(InstancePtr != NULL);
    Xil_AssertVoid(InstancePtr->IsReady == XIL_COMPONENT_IS_READY);

    XConv_engine_WriteReg(InstancePtr->Ctrl_BaseAddress, XCONV_ENGINE_CTRL_ADDR_REQUANT_SHIFT_DATA, Data);
}

u32 XConv_engine_Get_requant_shift(XConv_engine *InstancePtr) {
    u32 Data;

    Xil_AssertNonvoid(InstancePtr != NULL);
    Xil_AssertNonvoid(InstancePtr->IsReady == XIL_COMPONENT_IS_READY);

    Data = XConv_engine_ReadReg(InstancePtr->Ctrl_BaseAddress, XCONV_ENGINE_CTRL_ADDR_REQUANT_SHIFT_DATA);
    return Data;
}

void XConv_engine_Set_leaky_relu_enable(XConv_engine *InstancePtr, u32 Data) {
    Xil_AssertVoid(InstancePtr != NULL);
    Xil_AssertVoid(InstancePtr->IsReady == XIL_COMPONENT_IS_READY);

    XConv_engine_WriteReg(InstancePtr->Ctrl_BaseAddress, XCONV_ENGINE_CTRL_ADDR_LEAKY_RELU_ENABLE_DATA, Data);
}

u32 XConv_engine_Get_leaky_relu_enable(XConv_engine *InstancePtr) {
    u32 Data;

    Xil_AssertNonvoid(InstancePtr != NULL);
    Xil_AssertNonvoid(InstancePtr->IsReady == XIL_COMPONENT_IS_READY);

    Data = XConv_engine_ReadReg(InstancePtr->Ctrl_BaseAddress, XCONV_ENGINE_CTRL_ADDR_LEAKY_RELU_ENABLE_DATA);
    return Data;
}

void XConv_engine_InterruptGlobalEnable(XConv_engine *InstancePtr) {
    Xil_AssertVoid(InstancePtr != NULL);
    Xil_AssertVoid(InstancePtr->IsReady == XIL_COMPONENT_IS_READY);

    XConv_engine_WriteReg(InstancePtr->Ctrl_BaseAddress, XCONV_ENGINE_CTRL_ADDR_GIE, 1);
}

void XConv_engine_InterruptGlobalDisable(XConv_engine *InstancePtr) {
    Xil_AssertVoid(InstancePtr != NULL);
    Xil_AssertVoid(InstancePtr->IsReady == XIL_COMPONENT_IS_READY);

    XConv_engine_WriteReg(InstancePtr->Ctrl_BaseAddress, XCONV_ENGINE_CTRL_ADDR_GIE, 0);
}

void XConv_engine_InterruptEnable(XConv_engine *InstancePtr, u32 Mask) {
    u32 Register;

    Xil_AssertVoid(InstancePtr != NULL);
    Xil_AssertVoid(InstancePtr->IsReady == XIL_COMPONENT_IS_READY);

    Register =  XConv_engine_ReadReg(InstancePtr->Ctrl_BaseAddress, XCONV_ENGINE_CTRL_ADDR_IER);
    XConv_engine_WriteReg(InstancePtr->Ctrl_BaseAddress, XCONV_ENGINE_CTRL_ADDR_IER, Register | Mask);
}

void XConv_engine_InterruptDisable(XConv_engine *InstancePtr, u32 Mask) {
    u32 Register;

    Xil_AssertVoid(InstancePtr != NULL);
    Xil_AssertVoid(InstancePtr->IsReady == XIL_COMPONENT_IS_READY);

    Register =  XConv_engine_ReadReg(InstancePtr->Ctrl_BaseAddress, XCONV_ENGINE_CTRL_ADDR_IER);
    XConv_engine_WriteReg(InstancePtr->Ctrl_BaseAddress, XCONV_ENGINE_CTRL_ADDR_IER, Register & (~Mask));
}

void XConv_engine_InterruptClear(XConv_engine *InstancePtr, u32 Mask) {
    Xil_AssertVoid(InstancePtr != NULL);
    Xil_AssertVoid(InstancePtr->IsReady == XIL_COMPONENT_IS_READY);

    XConv_engine_WriteReg(InstancePtr->Ctrl_BaseAddress, XCONV_ENGINE_CTRL_ADDR_ISR, Mask);
}

u32 XConv_engine_InterruptGetEnabled(XConv_engine *InstancePtr) {
    Xil_AssertNonvoid(InstancePtr != NULL);
    Xil_AssertNonvoid(InstancePtr->IsReady == XIL_COMPONENT_IS_READY);

    return XConv_engine_ReadReg(InstancePtr->Ctrl_BaseAddress, XCONV_ENGINE_CTRL_ADDR_IER);
}

u32 XConv_engine_InterruptGetStatus(XConv_engine *InstancePtr) {
    Xil_AssertNonvoid(InstancePtr != NULL);
    Xil_AssertNonvoid(InstancePtr->IsReady == XIL_COMPONENT_IS_READY);

    return XConv_engine_ReadReg(InstancePtr->Ctrl_BaseAddress, XCONV_ENGINE_CTRL_ADDR_ISR);
}

