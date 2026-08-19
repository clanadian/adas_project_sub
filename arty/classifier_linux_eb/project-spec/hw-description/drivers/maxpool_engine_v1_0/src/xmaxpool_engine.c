// ==============================================================
// Vitis HLS - High-Level Synthesis from C, C++ and OpenCL v2024.2 (64-bit)
// Tool Version Limit: 2024.11
// Copyright 1986-2022 Xilinx, Inc. All Rights Reserved.
// Copyright 2022-2024 Advanced Micro Devices, Inc. All Rights Reserved.
// 
// ==============================================================
/***************************** Include Files *********************************/
#include "xmaxpool_engine.h"

/************************** Function Implementation *************************/
#ifndef __linux__
int XMaxpool_engine_CfgInitialize(XMaxpool_engine *InstancePtr, XMaxpool_engine_Config *ConfigPtr) {
    Xil_AssertNonvoid(InstancePtr != NULL);
    Xil_AssertNonvoid(ConfigPtr != NULL);

    InstancePtr->Ctrl_BaseAddress = ConfigPtr->Ctrl_BaseAddress;
    InstancePtr->IsReady = XIL_COMPONENT_IS_READY;

    return XST_SUCCESS;
}
#endif

void XMaxpool_engine_Start(XMaxpool_engine *InstancePtr) {
    u32 Data;

    Xil_AssertVoid(InstancePtr != NULL);
    Xil_AssertVoid(InstancePtr->IsReady == XIL_COMPONENT_IS_READY);

    Data = XMaxpool_engine_ReadReg(InstancePtr->Ctrl_BaseAddress, XMAXPOOL_ENGINE_CTRL_ADDR_AP_CTRL) & 0x80;
    XMaxpool_engine_WriteReg(InstancePtr->Ctrl_BaseAddress, XMAXPOOL_ENGINE_CTRL_ADDR_AP_CTRL, Data | 0x01);
}

u32 XMaxpool_engine_IsDone(XMaxpool_engine *InstancePtr) {
    u32 Data;

    Xil_AssertNonvoid(InstancePtr != NULL);
    Xil_AssertNonvoid(InstancePtr->IsReady == XIL_COMPONENT_IS_READY);

    Data = XMaxpool_engine_ReadReg(InstancePtr->Ctrl_BaseAddress, XMAXPOOL_ENGINE_CTRL_ADDR_AP_CTRL);
    return (Data >> 1) & 0x1;
}

u32 XMaxpool_engine_IsIdle(XMaxpool_engine *InstancePtr) {
    u32 Data;

    Xil_AssertNonvoid(InstancePtr != NULL);
    Xil_AssertNonvoid(InstancePtr->IsReady == XIL_COMPONENT_IS_READY);

    Data = XMaxpool_engine_ReadReg(InstancePtr->Ctrl_BaseAddress, XMAXPOOL_ENGINE_CTRL_ADDR_AP_CTRL);
    return (Data >> 2) & 0x1;
}

u32 XMaxpool_engine_IsReady(XMaxpool_engine *InstancePtr) {
    u32 Data;

    Xil_AssertNonvoid(InstancePtr != NULL);
    Xil_AssertNonvoid(InstancePtr->IsReady == XIL_COMPONENT_IS_READY);

    Data = XMaxpool_engine_ReadReg(InstancePtr->Ctrl_BaseAddress, XMAXPOOL_ENGINE_CTRL_ADDR_AP_CTRL);
    // check ap_start to see if the pcore is ready for next input
    return !(Data & 0x1);
}

void XMaxpool_engine_EnableAutoRestart(XMaxpool_engine *InstancePtr) {
    Xil_AssertVoid(InstancePtr != NULL);
    Xil_AssertVoid(InstancePtr->IsReady == XIL_COMPONENT_IS_READY);

    XMaxpool_engine_WriteReg(InstancePtr->Ctrl_BaseAddress, XMAXPOOL_ENGINE_CTRL_ADDR_AP_CTRL, 0x80);
}

void XMaxpool_engine_DisableAutoRestart(XMaxpool_engine *InstancePtr) {
    Xil_AssertVoid(InstancePtr != NULL);
    Xil_AssertVoid(InstancePtr->IsReady == XIL_COMPONENT_IS_READY);

    XMaxpool_engine_WriteReg(InstancePtr->Ctrl_BaseAddress, XMAXPOOL_ENGINE_CTRL_ADDR_AP_CTRL, 0);
}

void XMaxpool_engine_Set_ifmap(XMaxpool_engine *InstancePtr, u64 Data) {
    Xil_AssertVoid(InstancePtr != NULL);
    Xil_AssertVoid(InstancePtr->IsReady == XIL_COMPONENT_IS_READY);

    XMaxpool_engine_WriteReg(InstancePtr->Ctrl_BaseAddress, XMAXPOOL_ENGINE_CTRL_ADDR_IFMAP_DATA, (u32)(Data));
    XMaxpool_engine_WriteReg(InstancePtr->Ctrl_BaseAddress, XMAXPOOL_ENGINE_CTRL_ADDR_IFMAP_DATA + 4, (u32)(Data >> 32));
}

u64 XMaxpool_engine_Get_ifmap(XMaxpool_engine *InstancePtr) {
    u64 Data;

    Xil_AssertNonvoid(InstancePtr != NULL);
    Xil_AssertNonvoid(InstancePtr->IsReady == XIL_COMPONENT_IS_READY);

    Data = XMaxpool_engine_ReadReg(InstancePtr->Ctrl_BaseAddress, XMAXPOOL_ENGINE_CTRL_ADDR_IFMAP_DATA);
    Data += (u64)XMaxpool_engine_ReadReg(InstancePtr->Ctrl_BaseAddress, XMAXPOOL_ENGINE_CTRL_ADDR_IFMAP_DATA + 4) << 32;
    return Data;
}

void XMaxpool_engine_Set_ofmap(XMaxpool_engine *InstancePtr, u64 Data) {
    Xil_AssertVoid(InstancePtr != NULL);
    Xil_AssertVoid(InstancePtr->IsReady == XIL_COMPONENT_IS_READY);

    XMaxpool_engine_WriteReg(InstancePtr->Ctrl_BaseAddress, XMAXPOOL_ENGINE_CTRL_ADDR_OFMAP_DATA, (u32)(Data));
    XMaxpool_engine_WriteReg(InstancePtr->Ctrl_BaseAddress, XMAXPOOL_ENGINE_CTRL_ADDR_OFMAP_DATA + 4, (u32)(Data >> 32));
}

u64 XMaxpool_engine_Get_ofmap(XMaxpool_engine *InstancePtr) {
    u64 Data;

    Xil_AssertNonvoid(InstancePtr != NULL);
    Xil_AssertNonvoid(InstancePtr->IsReady == XIL_COMPONENT_IS_READY);

    Data = XMaxpool_engine_ReadReg(InstancePtr->Ctrl_BaseAddress, XMAXPOOL_ENGINE_CTRL_ADDR_OFMAP_DATA);
    Data += (u64)XMaxpool_engine_ReadReg(InstancePtr->Ctrl_BaseAddress, XMAXPOOL_ENGINE_CTRL_ADDR_OFMAP_DATA + 4) << 32;
    return Data;
}

void XMaxpool_engine_Set_img_h(XMaxpool_engine *InstancePtr, u32 Data) {
    Xil_AssertVoid(InstancePtr != NULL);
    Xil_AssertVoid(InstancePtr->IsReady == XIL_COMPONENT_IS_READY);

    XMaxpool_engine_WriteReg(InstancePtr->Ctrl_BaseAddress, XMAXPOOL_ENGINE_CTRL_ADDR_IMG_H_DATA, Data);
}

u32 XMaxpool_engine_Get_img_h(XMaxpool_engine *InstancePtr) {
    u32 Data;

    Xil_AssertNonvoid(InstancePtr != NULL);
    Xil_AssertNonvoid(InstancePtr->IsReady == XIL_COMPONENT_IS_READY);

    Data = XMaxpool_engine_ReadReg(InstancePtr->Ctrl_BaseAddress, XMAXPOOL_ENGINE_CTRL_ADDR_IMG_H_DATA);
    return Data;
}

void XMaxpool_engine_Set_img_w(XMaxpool_engine *InstancePtr, u32 Data) {
    Xil_AssertVoid(InstancePtr != NULL);
    Xil_AssertVoid(InstancePtr->IsReady == XIL_COMPONENT_IS_READY);

    XMaxpool_engine_WriteReg(InstancePtr->Ctrl_BaseAddress, XMAXPOOL_ENGINE_CTRL_ADDR_IMG_W_DATA, Data);
}

u32 XMaxpool_engine_Get_img_w(XMaxpool_engine *InstancePtr) {
    u32 Data;

    Xil_AssertNonvoid(InstancePtr != NULL);
    Xil_AssertNonvoid(InstancePtr->IsReady == XIL_COMPONENT_IS_READY);

    Data = XMaxpool_engine_ReadReg(InstancePtr->Ctrl_BaseAddress, XMAXPOOL_ENGINE_CTRL_ADDR_IMG_W_DATA);
    return Data;
}

void XMaxpool_engine_Set_ch(XMaxpool_engine *InstancePtr, u32 Data) {
    Xil_AssertVoid(InstancePtr != NULL);
    Xil_AssertVoid(InstancePtr->IsReady == XIL_COMPONENT_IS_READY);

    XMaxpool_engine_WriteReg(InstancePtr->Ctrl_BaseAddress, XMAXPOOL_ENGINE_CTRL_ADDR_CH_DATA, Data);
}

u32 XMaxpool_engine_Get_ch(XMaxpool_engine *InstancePtr) {
    u32 Data;

    Xil_AssertNonvoid(InstancePtr != NULL);
    Xil_AssertNonvoid(InstancePtr->IsReady == XIL_COMPONENT_IS_READY);

    Data = XMaxpool_engine_ReadReg(InstancePtr->Ctrl_BaseAddress, XMAXPOOL_ENGINE_CTRL_ADDR_CH_DATA);
    return Data;
}

void XMaxpool_engine_Set_stride(XMaxpool_engine *InstancePtr, u32 Data) {
    Xil_AssertVoid(InstancePtr != NULL);
    Xil_AssertVoid(InstancePtr->IsReady == XIL_COMPONENT_IS_READY);

    XMaxpool_engine_WriteReg(InstancePtr->Ctrl_BaseAddress, XMAXPOOL_ENGINE_CTRL_ADDR_STRIDE_DATA, Data);
}

u32 XMaxpool_engine_Get_stride(XMaxpool_engine *InstancePtr) {
    u32 Data;

    Xil_AssertNonvoid(InstancePtr != NULL);
    Xil_AssertNonvoid(InstancePtr->IsReady == XIL_COMPONENT_IS_READY);

    Data = XMaxpool_engine_ReadReg(InstancePtr->Ctrl_BaseAddress, XMAXPOOL_ENGINE_CTRL_ADDR_STRIDE_DATA);
    return Data;
}

void XMaxpool_engine_Set_pad_right(XMaxpool_engine *InstancePtr, u32 Data) {
    Xil_AssertVoid(InstancePtr != NULL);
    Xil_AssertVoid(InstancePtr->IsReady == XIL_COMPONENT_IS_READY);

    XMaxpool_engine_WriteReg(InstancePtr->Ctrl_BaseAddress, XMAXPOOL_ENGINE_CTRL_ADDR_PAD_RIGHT_DATA, Data);
}

u32 XMaxpool_engine_Get_pad_right(XMaxpool_engine *InstancePtr) {
    u32 Data;

    Xil_AssertNonvoid(InstancePtr != NULL);
    Xil_AssertNonvoid(InstancePtr->IsReady == XIL_COMPONENT_IS_READY);

    Data = XMaxpool_engine_ReadReg(InstancePtr->Ctrl_BaseAddress, XMAXPOOL_ENGINE_CTRL_ADDR_PAD_RIGHT_DATA);
    return Data;
}

void XMaxpool_engine_Set_pad_bottom(XMaxpool_engine *InstancePtr, u32 Data) {
    Xil_AssertVoid(InstancePtr != NULL);
    Xil_AssertVoid(InstancePtr->IsReady == XIL_COMPONENT_IS_READY);

    XMaxpool_engine_WriteReg(InstancePtr->Ctrl_BaseAddress, XMAXPOOL_ENGINE_CTRL_ADDR_PAD_BOTTOM_DATA, Data);
}

u32 XMaxpool_engine_Get_pad_bottom(XMaxpool_engine *InstancePtr) {
    u32 Data;

    Xil_AssertNonvoid(InstancePtr != NULL);
    Xil_AssertNonvoid(InstancePtr->IsReady == XIL_COMPONENT_IS_READY);

    Data = XMaxpool_engine_ReadReg(InstancePtr->Ctrl_BaseAddress, XMAXPOOL_ENGINE_CTRL_ADDR_PAD_BOTTOM_DATA);
    return Data;
}

void XMaxpool_engine_InterruptGlobalEnable(XMaxpool_engine *InstancePtr) {
    Xil_AssertVoid(InstancePtr != NULL);
    Xil_AssertVoid(InstancePtr->IsReady == XIL_COMPONENT_IS_READY);

    XMaxpool_engine_WriteReg(InstancePtr->Ctrl_BaseAddress, XMAXPOOL_ENGINE_CTRL_ADDR_GIE, 1);
}

void XMaxpool_engine_InterruptGlobalDisable(XMaxpool_engine *InstancePtr) {
    Xil_AssertVoid(InstancePtr != NULL);
    Xil_AssertVoid(InstancePtr->IsReady == XIL_COMPONENT_IS_READY);

    XMaxpool_engine_WriteReg(InstancePtr->Ctrl_BaseAddress, XMAXPOOL_ENGINE_CTRL_ADDR_GIE, 0);
}

void XMaxpool_engine_InterruptEnable(XMaxpool_engine *InstancePtr, u32 Mask) {
    u32 Register;

    Xil_AssertVoid(InstancePtr != NULL);
    Xil_AssertVoid(InstancePtr->IsReady == XIL_COMPONENT_IS_READY);

    Register =  XMaxpool_engine_ReadReg(InstancePtr->Ctrl_BaseAddress, XMAXPOOL_ENGINE_CTRL_ADDR_IER);
    XMaxpool_engine_WriteReg(InstancePtr->Ctrl_BaseAddress, XMAXPOOL_ENGINE_CTRL_ADDR_IER, Register | Mask);
}

void XMaxpool_engine_InterruptDisable(XMaxpool_engine *InstancePtr, u32 Mask) {
    u32 Register;

    Xil_AssertVoid(InstancePtr != NULL);
    Xil_AssertVoid(InstancePtr->IsReady == XIL_COMPONENT_IS_READY);

    Register =  XMaxpool_engine_ReadReg(InstancePtr->Ctrl_BaseAddress, XMAXPOOL_ENGINE_CTRL_ADDR_IER);
    XMaxpool_engine_WriteReg(InstancePtr->Ctrl_BaseAddress, XMAXPOOL_ENGINE_CTRL_ADDR_IER, Register & (~Mask));
}

void XMaxpool_engine_InterruptClear(XMaxpool_engine *InstancePtr, u32 Mask) {
    Xil_AssertVoid(InstancePtr != NULL);
    Xil_AssertVoid(InstancePtr->IsReady == XIL_COMPONENT_IS_READY);

    XMaxpool_engine_WriteReg(InstancePtr->Ctrl_BaseAddress, XMAXPOOL_ENGINE_CTRL_ADDR_ISR, Mask);
}

u32 XMaxpool_engine_InterruptGetEnabled(XMaxpool_engine *InstancePtr) {
    Xil_AssertNonvoid(InstancePtr != NULL);
    Xil_AssertNonvoid(InstancePtr->IsReady == XIL_COMPONENT_IS_READY);

    return XMaxpool_engine_ReadReg(InstancePtr->Ctrl_BaseAddress, XMAXPOOL_ENGINE_CTRL_ADDR_IER);
}

u32 XMaxpool_engine_InterruptGetStatus(XMaxpool_engine *InstancePtr) {
    Xil_AssertNonvoid(InstancePtr != NULL);
    Xil_AssertNonvoid(InstancePtr->IsReady == XIL_COMPONENT_IS_READY);

    return XMaxpool_engine_ReadReg(InstancePtr->Ctrl_BaseAddress, XMAXPOOL_ENGINE_CTRL_ADDR_ISR);
}

