// ==============================================================
// Vitis HLS - High-Level Synthesis from C, C++ and OpenCL v2024.2 (64-bit)
// Tool Version Limit: 2024.11
// Copyright 1986-2022 Xilinx, Inc. All Rights Reserved.
// Copyright 2022-2024 Advanced Micro Devices, Inc. All Rights Reserved.
// 
// ==============================================================
/***************************** Include Files *********************************/
#include "xconv0_engine.h"

/************************** Function Implementation *************************/
#ifndef __linux__
int XConv0_engine_CfgInitialize(XConv0_engine *InstancePtr, XConv0_engine_Config *ConfigPtr) {
    Xil_AssertNonvoid(InstancePtr != NULL);
    Xil_AssertNonvoid(ConfigPtr != NULL);

    InstancePtr->Ctrl_BaseAddress = ConfigPtr->Ctrl_BaseAddress;
    InstancePtr->IsReady = XIL_COMPONENT_IS_READY;

    return XST_SUCCESS;
}
#endif

void XConv0_engine_Start(XConv0_engine *InstancePtr) {
    u32 Data;

    Xil_AssertVoid(InstancePtr != NULL);
    Xil_AssertVoid(InstancePtr->IsReady == XIL_COMPONENT_IS_READY);

    Data = XConv0_engine_ReadReg(InstancePtr->Ctrl_BaseAddress, XCONV0_ENGINE_CTRL_ADDR_AP_CTRL) & 0x80;
    XConv0_engine_WriteReg(InstancePtr->Ctrl_BaseAddress, XCONV0_ENGINE_CTRL_ADDR_AP_CTRL, Data | 0x01);
}

u32 XConv0_engine_IsDone(XConv0_engine *InstancePtr) {
    u32 Data;

    Xil_AssertNonvoid(InstancePtr != NULL);
    Xil_AssertNonvoid(InstancePtr->IsReady == XIL_COMPONENT_IS_READY);

    Data = XConv0_engine_ReadReg(InstancePtr->Ctrl_BaseAddress, XCONV0_ENGINE_CTRL_ADDR_AP_CTRL);
    return (Data >> 1) & 0x1;
}

u32 XConv0_engine_IsIdle(XConv0_engine *InstancePtr) {
    u32 Data;

    Xil_AssertNonvoid(InstancePtr != NULL);
    Xil_AssertNonvoid(InstancePtr->IsReady == XIL_COMPONENT_IS_READY);

    Data = XConv0_engine_ReadReg(InstancePtr->Ctrl_BaseAddress, XCONV0_ENGINE_CTRL_ADDR_AP_CTRL);
    return (Data >> 2) & 0x1;
}

u32 XConv0_engine_IsReady(XConv0_engine *InstancePtr) {
    u32 Data;

    Xil_AssertNonvoid(InstancePtr != NULL);
    Xil_AssertNonvoid(InstancePtr->IsReady == XIL_COMPONENT_IS_READY);

    Data = XConv0_engine_ReadReg(InstancePtr->Ctrl_BaseAddress, XCONV0_ENGINE_CTRL_ADDR_AP_CTRL);
    // check ap_start to see if the pcore is ready for next input
    return !(Data & 0x1);
}

void XConv0_engine_EnableAutoRestart(XConv0_engine *InstancePtr) {
    Xil_AssertVoid(InstancePtr != NULL);
    Xil_AssertVoid(InstancePtr->IsReady == XIL_COMPONENT_IS_READY);

    XConv0_engine_WriteReg(InstancePtr->Ctrl_BaseAddress, XCONV0_ENGINE_CTRL_ADDR_AP_CTRL, 0x80);
}

void XConv0_engine_DisableAutoRestart(XConv0_engine *InstancePtr) {
    Xil_AssertVoid(InstancePtr != NULL);
    Xil_AssertVoid(InstancePtr->IsReady == XIL_COMPONENT_IS_READY);

    XConv0_engine_WriteReg(InstancePtr->Ctrl_BaseAddress, XCONV0_ENGINE_CTRL_ADDR_AP_CTRL, 0);
}

void XConv0_engine_Set_ifmap(XConv0_engine *InstancePtr, u64 Data) {
    Xil_AssertVoid(InstancePtr != NULL);
    Xil_AssertVoid(InstancePtr->IsReady == XIL_COMPONENT_IS_READY);

    XConv0_engine_WriteReg(InstancePtr->Ctrl_BaseAddress, XCONV0_ENGINE_CTRL_ADDR_IFMAP_DATA, (u32)(Data));
    XConv0_engine_WriteReg(InstancePtr->Ctrl_BaseAddress, XCONV0_ENGINE_CTRL_ADDR_IFMAP_DATA + 4, (u32)(Data >> 32));
}

u64 XConv0_engine_Get_ifmap(XConv0_engine *InstancePtr) {
    u64 Data;

    Xil_AssertNonvoid(InstancePtr != NULL);
    Xil_AssertNonvoid(InstancePtr->IsReady == XIL_COMPONENT_IS_READY);

    Data = XConv0_engine_ReadReg(InstancePtr->Ctrl_BaseAddress, XCONV0_ENGINE_CTRL_ADDR_IFMAP_DATA);
    Data += (u64)XConv0_engine_ReadReg(InstancePtr->Ctrl_BaseAddress, XCONV0_ENGINE_CTRL_ADDR_IFMAP_DATA + 4) << 32;
    return Data;
}

void XConv0_engine_Set_weights(XConv0_engine *InstancePtr, u64 Data) {
    Xil_AssertVoid(InstancePtr != NULL);
    Xil_AssertVoid(InstancePtr->IsReady == XIL_COMPONENT_IS_READY);

    XConv0_engine_WriteReg(InstancePtr->Ctrl_BaseAddress, XCONV0_ENGINE_CTRL_ADDR_WEIGHTS_DATA, (u32)(Data));
    XConv0_engine_WriteReg(InstancePtr->Ctrl_BaseAddress, XCONV0_ENGINE_CTRL_ADDR_WEIGHTS_DATA + 4, (u32)(Data >> 32));
}

u64 XConv0_engine_Get_weights(XConv0_engine *InstancePtr) {
    u64 Data;

    Xil_AssertNonvoid(InstancePtr != NULL);
    Xil_AssertNonvoid(InstancePtr->IsReady == XIL_COMPONENT_IS_READY);

    Data = XConv0_engine_ReadReg(InstancePtr->Ctrl_BaseAddress, XCONV0_ENGINE_CTRL_ADDR_WEIGHTS_DATA);
    Data += (u64)XConv0_engine_ReadReg(InstancePtr->Ctrl_BaseAddress, XCONV0_ENGINE_CTRL_ADDR_WEIGHTS_DATA + 4) << 32;
    return Data;
}

void XConv0_engine_Set_bias(XConv0_engine *InstancePtr, u64 Data) {
    Xil_AssertVoid(InstancePtr != NULL);
    Xil_AssertVoid(InstancePtr->IsReady == XIL_COMPONENT_IS_READY);

    XConv0_engine_WriteReg(InstancePtr->Ctrl_BaseAddress, XCONV0_ENGINE_CTRL_ADDR_BIAS_DATA, (u32)(Data));
    XConv0_engine_WriteReg(InstancePtr->Ctrl_BaseAddress, XCONV0_ENGINE_CTRL_ADDR_BIAS_DATA + 4, (u32)(Data >> 32));
}

u64 XConv0_engine_Get_bias(XConv0_engine *InstancePtr) {
    u64 Data;

    Xil_AssertNonvoid(InstancePtr != NULL);
    Xil_AssertNonvoid(InstancePtr->IsReady == XIL_COMPONENT_IS_READY);

    Data = XConv0_engine_ReadReg(InstancePtr->Ctrl_BaseAddress, XCONV0_ENGINE_CTRL_ADDR_BIAS_DATA);
    Data += (u64)XConv0_engine_ReadReg(InstancePtr->Ctrl_BaseAddress, XCONV0_ENGINE_CTRL_ADDR_BIAS_DATA + 4) << 32;
    return Data;
}

void XConv0_engine_Set_ofmap(XConv0_engine *InstancePtr, u64 Data) {
    Xil_AssertVoid(InstancePtr != NULL);
    Xil_AssertVoid(InstancePtr->IsReady == XIL_COMPONENT_IS_READY);

    XConv0_engine_WriteReg(InstancePtr->Ctrl_BaseAddress, XCONV0_ENGINE_CTRL_ADDR_OFMAP_DATA, (u32)(Data));
    XConv0_engine_WriteReg(InstancePtr->Ctrl_BaseAddress, XCONV0_ENGINE_CTRL_ADDR_OFMAP_DATA + 4, (u32)(Data >> 32));
}

u64 XConv0_engine_Get_ofmap(XConv0_engine *InstancePtr) {
    u64 Data;

    Xil_AssertNonvoid(InstancePtr != NULL);
    Xil_AssertNonvoid(InstancePtr->IsReady == XIL_COMPONENT_IS_READY);

    Data = XConv0_engine_ReadReg(InstancePtr->Ctrl_BaseAddress, XCONV0_ENGINE_CTRL_ADDR_OFMAP_DATA);
    Data += (u64)XConv0_engine_ReadReg(InstancePtr->Ctrl_BaseAddress, XCONV0_ENGINE_CTRL_ADDR_OFMAP_DATA + 4) << 32;
    return Data;
}

void XConv0_engine_Set_img_h(XConv0_engine *InstancePtr, u32 Data) {
    Xil_AssertVoid(InstancePtr != NULL);
    Xil_AssertVoid(InstancePtr->IsReady == XIL_COMPONENT_IS_READY);

    XConv0_engine_WriteReg(InstancePtr->Ctrl_BaseAddress, XCONV0_ENGINE_CTRL_ADDR_IMG_H_DATA, Data);
}

u32 XConv0_engine_Get_img_h(XConv0_engine *InstancePtr) {
    u32 Data;

    Xil_AssertNonvoid(InstancePtr != NULL);
    Xil_AssertNonvoid(InstancePtr->IsReady == XIL_COMPONENT_IS_READY);

    Data = XConv0_engine_ReadReg(InstancePtr->Ctrl_BaseAddress, XCONV0_ENGINE_CTRL_ADDR_IMG_H_DATA);
    return Data;
}

void XConv0_engine_Set_img_w(XConv0_engine *InstancePtr, u32 Data) {
    Xil_AssertVoid(InstancePtr != NULL);
    Xil_AssertVoid(InstancePtr->IsReady == XIL_COMPONENT_IS_READY);

    XConv0_engine_WriteReg(InstancePtr->Ctrl_BaseAddress, XCONV0_ENGINE_CTRL_ADDR_IMG_W_DATA, Data);
}

u32 XConv0_engine_Get_img_w(XConv0_engine *InstancePtr) {
    u32 Data;

    Xil_AssertNonvoid(InstancePtr != NULL);
    Xil_AssertNonvoid(InstancePtr->IsReady == XIL_COMPONENT_IS_READY);

    Data = XConv0_engine_ReadReg(InstancePtr->Ctrl_BaseAddress, XCONV0_ENGINE_CTRL_ADDR_IMG_W_DATA);
    return Data;
}

void XConv0_engine_Set_requant_multiplier(XConv0_engine *InstancePtr, u32 Data) {
    Xil_AssertVoid(InstancePtr != NULL);
    Xil_AssertVoid(InstancePtr->IsReady == XIL_COMPONENT_IS_READY);

    XConv0_engine_WriteReg(InstancePtr->Ctrl_BaseAddress, XCONV0_ENGINE_CTRL_ADDR_REQUANT_MULTIPLIER_DATA, Data);
}

u32 XConv0_engine_Get_requant_multiplier(XConv0_engine *InstancePtr) {
    u32 Data;

    Xil_AssertNonvoid(InstancePtr != NULL);
    Xil_AssertNonvoid(InstancePtr->IsReady == XIL_COMPONENT_IS_READY);

    Data = XConv0_engine_ReadReg(InstancePtr->Ctrl_BaseAddress, XCONV0_ENGINE_CTRL_ADDR_REQUANT_MULTIPLIER_DATA);
    return Data;
}

void XConv0_engine_Set_requant_shift(XConv0_engine *InstancePtr, u32 Data) {
    Xil_AssertVoid(InstancePtr != NULL);
    Xil_AssertVoid(InstancePtr->IsReady == XIL_COMPONENT_IS_READY);

    XConv0_engine_WriteReg(InstancePtr->Ctrl_BaseAddress, XCONV0_ENGINE_CTRL_ADDR_REQUANT_SHIFT_DATA, Data);
}

u32 XConv0_engine_Get_requant_shift(XConv0_engine *InstancePtr) {
    u32 Data;

    Xil_AssertNonvoid(InstancePtr != NULL);
    Xil_AssertNonvoid(InstancePtr->IsReady == XIL_COMPONENT_IS_READY);

    Data = XConv0_engine_ReadReg(InstancePtr->Ctrl_BaseAddress, XCONV0_ENGINE_CTRL_ADDR_REQUANT_SHIFT_DATA);
    return Data;
}

void XConv0_engine_Set_leaky_relu_enable(XConv0_engine *InstancePtr, u32 Data) {
    Xil_AssertVoid(InstancePtr != NULL);
    Xil_AssertVoid(InstancePtr->IsReady == XIL_COMPONENT_IS_READY);

    XConv0_engine_WriteReg(InstancePtr->Ctrl_BaseAddress, XCONV0_ENGINE_CTRL_ADDR_LEAKY_RELU_ENABLE_DATA, Data);
}

u32 XConv0_engine_Get_leaky_relu_enable(XConv0_engine *InstancePtr) {
    u32 Data;

    Xil_AssertNonvoid(InstancePtr != NULL);
    Xil_AssertNonvoid(InstancePtr->IsReady == XIL_COMPONENT_IS_READY);

    Data = XConv0_engine_ReadReg(InstancePtr->Ctrl_BaseAddress, XCONV0_ENGINE_CTRL_ADDR_LEAKY_RELU_ENABLE_DATA);
    return Data;
}

void XConv0_engine_InterruptGlobalEnable(XConv0_engine *InstancePtr) {
    Xil_AssertVoid(InstancePtr != NULL);
    Xil_AssertVoid(InstancePtr->IsReady == XIL_COMPONENT_IS_READY);

    XConv0_engine_WriteReg(InstancePtr->Ctrl_BaseAddress, XCONV0_ENGINE_CTRL_ADDR_GIE, 1);
}

void XConv0_engine_InterruptGlobalDisable(XConv0_engine *InstancePtr) {
    Xil_AssertVoid(InstancePtr != NULL);
    Xil_AssertVoid(InstancePtr->IsReady == XIL_COMPONENT_IS_READY);

    XConv0_engine_WriteReg(InstancePtr->Ctrl_BaseAddress, XCONV0_ENGINE_CTRL_ADDR_GIE, 0);
}

void XConv0_engine_InterruptEnable(XConv0_engine *InstancePtr, u32 Mask) {
    u32 Register;

    Xil_AssertVoid(InstancePtr != NULL);
    Xil_AssertVoid(InstancePtr->IsReady == XIL_COMPONENT_IS_READY);

    Register =  XConv0_engine_ReadReg(InstancePtr->Ctrl_BaseAddress, XCONV0_ENGINE_CTRL_ADDR_IER);
    XConv0_engine_WriteReg(InstancePtr->Ctrl_BaseAddress, XCONV0_ENGINE_CTRL_ADDR_IER, Register | Mask);
}

void XConv0_engine_InterruptDisable(XConv0_engine *InstancePtr, u32 Mask) {
    u32 Register;

    Xil_AssertVoid(InstancePtr != NULL);
    Xil_AssertVoid(InstancePtr->IsReady == XIL_COMPONENT_IS_READY);

    Register =  XConv0_engine_ReadReg(InstancePtr->Ctrl_BaseAddress, XCONV0_ENGINE_CTRL_ADDR_IER);
    XConv0_engine_WriteReg(InstancePtr->Ctrl_BaseAddress, XCONV0_ENGINE_CTRL_ADDR_IER, Register & (~Mask));
}

void XConv0_engine_InterruptClear(XConv0_engine *InstancePtr, u32 Mask) {
    Xil_AssertVoid(InstancePtr != NULL);
    Xil_AssertVoid(InstancePtr->IsReady == XIL_COMPONENT_IS_READY);

    XConv0_engine_WriteReg(InstancePtr->Ctrl_BaseAddress, XCONV0_ENGINE_CTRL_ADDR_ISR, Mask);
}

u32 XConv0_engine_InterruptGetEnabled(XConv0_engine *InstancePtr) {
    Xil_AssertNonvoid(InstancePtr != NULL);
    Xil_AssertNonvoid(InstancePtr->IsReady == XIL_COMPONENT_IS_READY);

    return XConv0_engine_ReadReg(InstancePtr->Ctrl_BaseAddress, XCONV0_ENGINE_CTRL_ADDR_IER);
}

u32 XConv0_engine_InterruptGetStatus(XConv0_engine *InstancePtr) {
    Xil_AssertNonvoid(InstancePtr != NULL);
    Xil_AssertNonvoid(InstancePtr->IsReady == XIL_COMPONENT_IS_READY);

    return XConv0_engine_ReadReg(InstancePtr->Ctrl_BaseAddress, XCONV0_ENGINE_CTRL_ADDR_ISR);
}

