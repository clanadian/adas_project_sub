// ==============================================================
// Vitis HLS - High-Level Synthesis from C, C++ and OpenCL v2024.2 (64-bit)
// Tool Version Limit: 2024.11
// Copyright 1986-2022 Xilinx, Inc. All Rights Reserved.
// Copyright 2022-2024 Advanced Micro Devices, Inc. All Rights Reserved.
// 
// ==============================================================
/***************************** Include Files *********************************/
#include "xclassifier_top.h"

/************************** Function Implementation *************************/
#ifndef __linux__
int XClassifier_top_CfgInitialize(XClassifier_top *InstancePtr, XClassifier_top_Config *ConfigPtr) {
    Xil_AssertNonvoid(InstancePtr != NULL);
    Xil_AssertNonvoid(ConfigPtr != NULL);

    InstancePtr->Control_BaseAddress = ConfigPtr->Control_BaseAddress;
    InstancePtr->Ctrl_BaseAddress = ConfigPtr->Ctrl_BaseAddress;
    InstancePtr->IsReady = XIL_COMPONENT_IS_READY;

    return XST_SUCCESS;
}
#endif

void XClassifier_top_Start(XClassifier_top *InstancePtr) {
    u32 Data;

    Xil_AssertVoid(InstancePtr != NULL);
    Xil_AssertVoid(InstancePtr->IsReady == XIL_COMPONENT_IS_READY);

    Data = XClassifier_top_ReadReg(InstancePtr->Ctrl_BaseAddress, XCLASSIFIER_TOP_CTRL_ADDR_AP_CTRL) & 0x80;
    XClassifier_top_WriteReg(InstancePtr->Ctrl_BaseAddress, XCLASSIFIER_TOP_CTRL_ADDR_AP_CTRL, Data | 0x01);
}

u32 XClassifier_top_IsDone(XClassifier_top *InstancePtr) {
    u32 Data;

    Xil_AssertNonvoid(InstancePtr != NULL);
    Xil_AssertNonvoid(InstancePtr->IsReady == XIL_COMPONENT_IS_READY);

    Data = XClassifier_top_ReadReg(InstancePtr->Ctrl_BaseAddress, XCLASSIFIER_TOP_CTRL_ADDR_AP_CTRL);
    return (Data >> 1) & 0x1;
}

u32 XClassifier_top_IsIdle(XClassifier_top *InstancePtr) {
    u32 Data;

    Xil_AssertNonvoid(InstancePtr != NULL);
    Xil_AssertNonvoid(InstancePtr->IsReady == XIL_COMPONENT_IS_READY);

    Data = XClassifier_top_ReadReg(InstancePtr->Ctrl_BaseAddress, XCLASSIFIER_TOP_CTRL_ADDR_AP_CTRL);
    return (Data >> 2) & 0x1;
}

u32 XClassifier_top_IsReady(XClassifier_top *InstancePtr) {
    u32 Data;

    Xil_AssertNonvoid(InstancePtr != NULL);
    Xil_AssertNonvoid(InstancePtr->IsReady == XIL_COMPONENT_IS_READY);

    Data = XClassifier_top_ReadReg(InstancePtr->Ctrl_BaseAddress, XCLASSIFIER_TOP_CTRL_ADDR_AP_CTRL);
    // check ap_start to see if the pcore is ready for next input
    return !(Data & 0x1);
}

void XClassifier_top_EnableAutoRestart(XClassifier_top *InstancePtr) {
    Xil_AssertVoid(InstancePtr != NULL);
    Xil_AssertVoid(InstancePtr->IsReady == XIL_COMPONENT_IS_READY);

    XClassifier_top_WriteReg(InstancePtr->Ctrl_BaseAddress, XCLASSIFIER_TOP_CTRL_ADDR_AP_CTRL, 0x80);
}

void XClassifier_top_DisableAutoRestart(XClassifier_top *InstancePtr) {
    Xil_AssertVoid(InstancePtr != NULL);
    Xil_AssertVoid(InstancePtr->IsReady == XIL_COMPONENT_IS_READY);

    XClassifier_top_WriteReg(InstancePtr->Ctrl_BaseAddress, XCLASSIFIER_TOP_CTRL_ADDR_AP_CTRL, 0);
}

void XClassifier_top_Set_ifmap_padded(XClassifier_top *InstancePtr, u64 Data) {
    Xil_AssertVoid(InstancePtr != NULL);
    Xil_AssertVoid(InstancePtr->IsReady == XIL_COMPONENT_IS_READY);

    XClassifier_top_WriteReg(InstancePtr->Control_BaseAddress, XCLASSIFIER_TOP_CONTROL_ADDR_IFMAP_PADDED_DATA, (u32)(Data));
    XClassifier_top_WriteReg(InstancePtr->Control_BaseAddress, XCLASSIFIER_TOP_CONTROL_ADDR_IFMAP_PADDED_DATA + 4, (u32)(Data >> 32));
}

u64 XClassifier_top_Get_ifmap_padded(XClassifier_top *InstancePtr) {
    u64 Data;

    Xil_AssertNonvoid(InstancePtr != NULL);
    Xil_AssertNonvoid(InstancePtr->IsReady == XIL_COMPONENT_IS_READY);

    Data = XClassifier_top_ReadReg(InstancePtr->Control_BaseAddress, XCLASSIFIER_TOP_CONTROL_ADDR_IFMAP_PADDED_DATA);
    Data += (u64)XClassifier_top_ReadReg(InstancePtr->Control_BaseAddress, XCLASSIFIER_TOP_CONTROL_ADDR_IFMAP_PADDED_DATA + 4) << 32;
    return Data;
}

void XClassifier_top_Set_w_conv0(XClassifier_top *InstancePtr, u64 Data) {
    Xil_AssertVoid(InstancePtr != NULL);
    Xil_AssertVoid(InstancePtr->IsReady == XIL_COMPONENT_IS_READY);

    XClassifier_top_WriteReg(InstancePtr->Control_BaseAddress, XCLASSIFIER_TOP_CONTROL_ADDR_W_CONV0_DATA, (u32)(Data));
    XClassifier_top_WriteReg(InstancePtr->Control_BaseAddress, XCLASSIFIER_TOP_CONTROL_ADDR_W_CONV0_DATA + 4, (u32)(Data >> 32));
}

u64 XClassifier_top_Get_w_conv0(XClassifier_top *InstancePtr) {
    u64 Data;

    Xil_AssertNonvoid(InstancePtr != NULL);
    Xil_AssertNonvoid(InstancePtr->IsReady == XIL_COMPONENT_IS_READY);

    Data = XClassifier_top_ReadReg(InstancePtr->Control_BaseAddress, XCLASSIFIER_TOP_CONTROL_ADDR_W_CONV0_DATA);
    Data += (u64)XClassifier_top_ReadReg(InstancePtr->Control_BaseAddress, XCLASSIFIER_TOP_CONTROL_ADDR_W_CONV0_DATA + 4) << 32;
    return Data;
}

void XClassifier_top_Set_w_conv1(XClassifier_top *InstancePtr, u64 Data) {
    Xil_AssertVoid(InstancePtr != NULL);
    Xil_AssertVoid(InstancePtr->IsReady == XIL_COMPONENT_IS_READY);

    XClassifier_top_WriteReg(InstancePtr->Control_BaseAddress, XCLASSIFIER_TOP_CONTROL_ADDR_W_CONV1_DATA, (u32)(Data));
    XClassifier_top_WriteReg(InstancePtr->Control_BaseAddress, XCLASSIFIER_TOP_CONTROL_ADDR_W_CONV1_DATA + 4, (u32)(Data >> 32));
}

u64 XClassifier_top_Get_w_conv1(XClassifier_top *InstancePtr) {
    u64 Data;

    Xil_AssertNonvoid(InstancePtr != NULL);
    Xil_AssertNonvoid(InstancePtr->IsReady == XIL_COMPONENT_IS_READY);

    Data = XClassifier_top_ReadReg(InstancePtr->Control_BaseAddress, XCLASSIFIER_TOP_CONTROL_ADDR_W_CONV1_DATA);
    Data += (u64)XClassifier_top_ReadReg(InstancePtr->Control_BaseAddress, XCLASSIFIER_TOP_CONTROL_ADDR_W_CONV1_DATA + 4) << 32;
    return Data;
}

void XClassifier_top_Set_w_conv2(XClassifier_top *InstancePtr, u64 Data) {
    Xil_AssertVoid(InstancePtr != NULL);
    Xil_AssertVoid(InstancePtr->IsReady == XIL_COMPONENT_IS_READY);

    XClassifier_top_WriteReg(InstancePtr->Control_BaseAddress, XCLASSIFIER_TOP_CONTROL_ADDR_W_CONV2_DATA, (u32)(Data));
    XClassifier_top_WriteReg(InstancePtr->Control_BaseAddress, XCLASSIFIER_TOP_CONTROL_ADDR_W_CONV2_DATA + 4, (u32)(Data >> 32));
}

u64 XClassifier_top_Get_w_conv2(XClassifier_top *InstancePtr) {
    u64 Data;

    Xil_AssertNonvoid(InstancePtr != NULL);
    Xil_AssertNonvoid(InstancePtr->IsReady == XIL_COMPONENT_IS_READY);

    Data = XClassifier_top_ReadReg(InstancePtr->Control_BaseAddress, XCLASSIFIER_TOP_CONTROL_ADDR_W_CONV2_DATA);
    Data += (u64)XClassifier_top_ReadReg(InstancePtr->Control_BaseAddress, XCLASSIFIER_TOP_CONTROL_ADDR_W_CONV2_DATA + 4) << 32;
    return Data;
}

void XClassifier_top_Set_out_r(XClassifier_top *InstancePtr, u64 Data) {
    Xil_AssertVoid(InstancePtr != NULL);
    Xil_AssertVoid(InstancePtr->IsReady == XIL_COMPONENT_IS_READY);

    XClassifier_top_WriteReg(InstancePtr->Control_BaseAddress, XCLASSIFIER_TOP_CONTROL_ADDR_OUT_R_DATA, (u32)(Data));
    XClassifier_top_WriteReg(InstancePtr->Control_BaseAddress, XCLASSIFIER_TOP_CONTROL_ADDR_OUT_R_DATA + 4, (u32)(Data >> 32));
}

u64 XClassifier_top_Get_out_r(XClassifier_top *InstancePtr) {
    u64 Data;

    Xil_AssertNonvoid(InstancePtr != NULL);
    Xil_AssertNonvoid(InstancePtr->IsReady == XIL_COMPONENT_IS_READY);

    Data = XClassifier_top_ReadReg(InstancePtr->Control_BaseAddress, XCLASSIFIER_TOP_CONTROL_ADDR_OUT_R_DATA);
    Data += (u64)XClassifier_top_ReadReg(InstancePtr->Control_BaseAddress, XCLASSIFIER_TOP_CONTROL_ADDR_OUT_R_DATA + 4) << 32;
    return Data;
}

void XClassifier_top_Set_rq_conv0(XClassifier_top *InstancePtr, u64 Data) {
    Xil_AssertVoid(InstancePtr != NULL);
    Xil_AssertVoid(InstancePtr->IsReady == XIL_COMPONENT_IS_READY);

    XClassifier_top_WriteReg(InstancePtr->Ctrl_BaseAddress, XCLASSIFIER_TOP_CTRL_ADDR_RQ_CONV0_DATA, (u32)(Data));
    XClassifier_top_WriteReg(InstancePtr->Ctrl_BaseAddress, XCLASSIFIER_TOP_CTRL_ADDR_RQ_CONV0_DATA + 4, (u32)(Data >> 32));
}

u64 XClassifier_top_Get_rq_conv0(XClassifier_top *InstancePtr) {
    u64 Data;

    Xil_AssertNonvoid(InstancePtr != NULL);
    Xil_AssertNonvoid(InstancePtr->IsReady == XIL_COMPONENT_IS_READY);

    Data = XClassifier_top_ReadReg(InstancePtr->Ctrl_BaseAddress, XCLASSIFIER_TOP_CTRL_ADDR_RQ_CONV0_DATA);
    Data += (u64)XClassifier_top_ReadReg(InstancePtr->Ctrl_BaseAddress, XCLASSIFIER_TOP_CTRL_ADDR_RQ_CONV0_DATA + 4) << 32;
    return Data;
}

void XClassifier_top_Set_rq_conv1(XClassifier_top *InstancePtr, u64 Data) {
    Xil_AssertVoid(InstancePtr != NULL);
    Xil_AssertVoid(InstancePtr->IsReady == XIL_COMPONENT_IS_READY);

    XClassifier_top_WriteReg(InstancePtr->Ctrl_BaseAddress, XCLASSIFIER_TOP_CTRL_ADDR_RQ_CONV1_DATA, (u32)(Data));
    XClassifier_top_WriteReg(InstancePtr->Ctrl_BaseAddress, XCLASSIFIER_TOP_CTRL_ADDR_RQ_CONV1_DATA + 4, (u32)(Data >> 32));
}

u64 XClassifier_top_Get_rq_conv1(XClassifier_top *InstancePtr) {
    u64 Data;

    Xil_AssertNonvoid(InstancePtr != NULL);
    Xil_AssertNonvoid(InstancePtr->IsReady == XIL_COMPONENT_IS_READY);

    Data = XClassifier_top_ReadReg(InstancePtr->Ctrl_BaseAddress, XCLASSIFIER_TOP_CTRL_ADDR_RQ_CONV1_DATA);
    Data += (u64)XClassifier_top_ReadReg(InstancePtr->Ctrl_BaseAddress, XCLASSIFIER_TOP_CTRL_ADDR_RQ_CONV1_DATA + 4) << 32;
    return Data;
}

void XClassifier_top_Set_rq_conv2(XClassifier_top *InstancePtr, u64 Data) {
    Xil_AssertVoid(InstancePtr != NULL);
    Xil_AssertVoid(InstancePtr->IsReady == XIL_COMPONENT_IS_READY);

    XClassifier_top_WriteReg(InstancePtr->Ctrl_BaseAddress, XCLASSIFIER_TOP_CTRL_ADDR_RQ_CONV2_DATA, (u32)(Data));
    XClassifier_top_WriteReg(InstancePtr->Ctrl_BaseAddress, XCLASSIFIER_TOP_CTRL_ADDR_RQ_CONV2_DATA + 4, (u32)(Data >> 32));
}

u64 XClassifier_top_Get_rq_conv2(XClassifier_top *InstancePtr) {
    u64 Data;

    Xil_AssertNonvoid(InstancePtr != NULL);
    Xil_AssertNonvoid(InstancePtr->IsReady == XIL_COMPONENT_IS_READY);

    Data = XClassifier_top_ReadReg(InstancePtr->Ctrl_BaseAddress, XCLASSIFIER_TOP_CTRL_ADDR_RQ_CONV2_DATA);
    Data += (u64)XClassifier_top_ReadReg(InstancePtr->Ctrl_BaseAddress, XCLASSIFIER_TOP_CTRL_ADDR_RQ_CONV2_DATA + 4) << 32;
    return Data;
}

u32 XClassifier_top_Get_b_conv0_BaseAddress(XClassifier_top *InstancePtr) {
    Xil_AssertNonvoid(InstancePtr != NULL);
    Xil_AssertNonvoid(InstancePtr->IsReady == XIL_COMPONENT_IS_READY);

    return (InstancePtr->Ctrl_BaseAddress + XCLASSIFIER_TOP_CTRL_ADDR_B_CONV0_BASE);
}

u32 XClassifier_top_Get_b_conv0_HighAddress(XClassifier_top *InstancePtr) {
    Xil_AssertNonvoid(InstancePtr != NULL);
    Xil_AssertNonvoid(InstancePtr->IsReady == XIL_COMPONENT_IS_READY);

    return (InstancePtr->Ctrl_BaseAddress + XCLASSIFIER_TOP_CTRL_ADDR_B_CONV0_HIGH);
}

u32 XClassifier_top_Get_b_conv0_TotalBytes(XClassifier_top *InstancePtr) {
    Xil_AssertNonvoid(InstancePtr != NULL);
    Xil_AssertNonvoid(InstancePtr->IsReady == XIL_COMPONENT_IS_READY);

    return (XCLASSIFIER_TOP_CTRL_ADDR_B_CONV0_HIGH - XCLASSIFIER_TOP_CTRL_ADDR_B_CONV0_BASE + 1);
}

u32 XClassifier_top_Get_b_conv0_BitWidth(XClassifier_top *InstancePtr) {
    Xil_AssertNonvoid(InstancePtr != NULL);
    Xil_AssertNonvoid(InstancePtr->IsReady == XIL_COMPONENT_IS_READY);

    return XCLASSIFIER_TOP_CTRL_WIDTH_B_CONV0;
}

u32 XClassifier_top_Get_b_conv0_Depth(XClassifier_top *InstancePtr) {
    Xil_AssertNonvoid(InstancePtr != NULL);
    Xil_AssertNonvoid(InstancePtr->IsReady == XIL_COMPONENT_IS_READY);

    return XCLASSIFIER_TOP_CTRL_DEPTH_B_CONV0;
}

u32 XClassifier_top_Write_b_conv0_Words(XClassifier_top *InstancePtr, int offset, word_type *data, int length) {
    Xil_AssertNonvoid(InstancePtr != NULL);
    Xil_AssertNonvoid(InstancePtr -> IsReady == XIL_COMPONENT_IS_READY);

    int i;

    if ((offset + length)*4 > (XCLASSIFIER_TOP_CTRL_ADDR_B_CONV0_HIGH - XCLASSIFIER_TOP_CTRL_ADDR_B_CONV0_BASE + 1))
        return 0;

    for (i = 0; i < length; i++) {
        *(int *)(InstancePtr->Ctrl_BaseAddress + XCLASSIFIER_TOP_CTRL_ADDR_B_CONV0_BASE + (offset + i)*4) = *(data + i);
    }
    return length;
}

u32 XClassifier_top_Read_b_conv0_Words(XClassifier_top *InstancePtr, int offset, word_type *data, int length) {
    Xil_AssertNonvoid(InstancePtr != NULL);
    Xil_AssertNonvoid(InstancePtr -> IsReady == XIL_COMPONENT_IS_READY);

    int i;

    if ((offset + length)*4 > (XCLASSIFIER_TOP_CTRL_ADDR_B_CONV0_HIGH - XCLASSIFIER_TOP_CTRL_ADDR_B_CONV0_BASE + 1))
        return 0;

    for (i = 0; i < length; i++) {
        *(data + i) = *(int *)(InstancePtr->Ctrl_BaseAddress + XCLASSIFIER_TOP_CTRL_ADDR_B_CONV0_BASE + (offset + i)*4);
    }
    return length;
}

u32 XClassifier_top_Write_b_conv0_Bytes(XClassifier_top *InstancePtr, int offset, char *data, int length) {
    Xil_AssertNonvoid(InstancePtr != NULL);
    Xil_AssertNonvoid(InstancePtr -> IsReady == XIL_COMPONENT_IS_READY);

    int i;

    if ((offset + length) > (XCLASSIFIER_TOP_CTRL_ADDR_B_CONV0_HIGH - XCLASSIFIER_TOP_CTRL_ADDR_B_CONV0_BASE + 1))
        return 0;

    for (i = 0; i < length; i++) {
        *(char *)(InstancePtr->Ctrl_BaseAddress + XCLASSIFIER_TOP_CTRL_ADDR_B_CONV0_BASE + offset + i) = *(data + i);
    }
    return length;
}

u32 XClassifier_top_Read_b_conv0_Bytes(XClassifier_top *InstancePtr, int offset, char *data, int length) {
    Xil_AssertNonvoid(InstancePtr != NULL);
    Xil_AssertNonvoid(InstancePtr -> IsReady == XIL_COMPONENT_IS_READY);

    int i;

    if ((offset + length) > (XCLASSIFIER_TOP_CTRL_ADDR_B_CONV0_HIGH - XCLASSIFIER_TOP_CTRL_ADDR_B_CONV0_BASE + 1))
        return 0;

    for (i = 0; i < length; i++) {
        *(data + i) = *(char *)(InstancePtr->Ctrl_BaseAddress + XCLASSIFIER_TOP_CTRL_ADDR_B_CONV0_BASE + offset + i);
    }
    return length;
}

u32 XClassifier_top_Get_b_conv1_BaseAddress(XClassifier_top *InstancePtr) {
    Xil_AssertNonvoid(InstancePtr != NULL);
    Xil_AssertNonvoid(InstancePtr->IsReady == XIL_COMPONENT_IS_READY);

    return (InstancePtr->Ctrl_BaseAddress + XCLASSIFIER_TOP_CTRL_ADDR_B_CONV1_BASE);
}

u32 XClassifier_top_Get_b_conv1_HighAddress(XClassifier_top *InstancePtr) {
    Xil_AssertNonvoid(InstancePtr != NULL);
    Xil_AssertNonvoid(InstancePtr->IsReady == XIL_COMPONENT_IS_READY);

    return (InstancePtr->Ctrl_BaseAddress + XCLASSIFIER_TOP_CTRL_ADDR_B_CONV1_HIGH);
}

u32 XClassifier_top_Get_b_conv1_TotalBytes(XClassifier_top *InstancePtr) {
    Xil_AssertNonvoid(InstancePtr != NULL);
    Xil_AssertNonvoid(InstancePtr->IsReady == XIL_COMPONENT_IS_READY);

    return (XCLASSIFIER_TOP_CTRL_ADDR_B_CONV1_HIGH - XCLASSIFIER_TOP_CTRL_ADDR_B_CONV1_BASE + 1);
}

u32 XClassifier_top_Get_b_conv1_BitWidth(XClassifier_top *InstancePtr) {
    Xil_AssertNonvoid(InstancePtr != NULL);
    Xil_AssertNonvoid(InstancePtr->IsReady == XIL_COMPONENT_IS_READY);

    return XCLASSIFIER_TOP_CTRL_WIDTH_B_CONV1;
}

u32 XClassifier_top_Get_b_conv1_Depth(XClassifier_top *InstancePtr) {
    Xil_AssertNonvoid(InstancePtr != NULL);
    Xil_AssertNonvoid(InstancePtr->IsReady == XIL_COMPONENT_IS_READY);

    return XCLASSIFIER_TOP_CTRL_DEPTH_B_CONV1;
}

u32 XClassifier_top_Write_b_conv1_Words(XClassifier_top *InstancePtr, int offset, word_type *data, int length) {
    Xil_AssertNonvoid(InstancePtr != NULL);
    Xil_AssertNonvoid(InstancePtr -> IsReady == XIL_COMPONENT_IS_READY);

    int i;

    if ((offset + length)*4 > (XCLASSIFIER_TOP_CTRL_ADDR_B_CONV1_HIGH - XCLASSIFIER_TOP_CTRL_ADDR_B_CONV1_BASE + 1))
        return 0;

    for (i = 0; i < length; i++) {
        *(int *)(InstancePtr->Ctrl_BaseAddress + XCLASSIFIER_TOP_CTRL_ADDR_B_CONV1_BASE + (offset + i)*4) = *(data + i);
    }
    return length;
}

u32 XClassifier_top_Read_b_conv1_Words(XClassifier_top *InstancePtr, int offset, word_type *data, int length) {
    Xil_AssertNonvoid(InstancePtr != NULL);
    Xil_AssertNonvoid(InstancePtr -> IsReady == XIL_COMPONENT_IS_READY);

    int i;

    if ((offset + length)*4 > (XCLASSIFIER_TOP_CTRL_ADDR_B_CONV1_HIGH - XCLASSIFIER_TOP_CTRL_ADDR_B_CONV1_BASE + 1))
        return 0;

    for (i = 0; i < length; i++) {
        *(data + i) = *(int *)(InstancePtr->Ctrl_BaseAddress + XCLASSIFIER_TOP_CTRL_ADDR_B_CONV1_BASE + (offset + i)*4);
    }
    return length;
}

u32 XClassifier_top_Write_b_conv1_Bytes(XClassifier_top *InstancePtr, int offset, char *data, int length) {
    Xil_AssertNonvoid(InstancePtr != NULL);
    Xil_AssertNonvoid(InstancePtr -> IsReady == XIL_COMPONENT_IS_READY);

    int i;

    if ((offset + length) > (XCLASSIFIER_TOP_CTRL_ADDR_B_CONV1_HIGH - XCLASSIFIER_TOP_CTRL_ADDR_B_CONV1_BASE + 1))
        return 0;

    for (i = 0; i < length; i++) {
        *(char *)(InstancePtr->Ctrl_BaseAddress + XCLASSIFIER_TOP_CTRL_ADDR_B_CONV1_BASE + offset + i) = *(data + i);
    }
    return length;
}

u32 XClassifier_top_Read_b_conv1_Bytes(XClassifier_top *InstancePtr, int offset, char *data, int length) {
    Xil_AssertNonvoid(InstancePtr != NULL);
    Xil_AssertNonvoid(InstancePtr -> IsReady == XIL_COMPONENT_IS_READY);

    int i;

    if ((offset + length) > (XCLASSIFIER_TOP_CTRL_ADDR_B_CONV1_HIGH - XCLASSIFIER_TOP_CTRL_ADDR_B_CONV1_BASE + 1))
        return 0;

    for (i = 0; i < length; i++) {
        *(data + i) = *(char *)(InstancePtr->Ctrl_BaseAddress + XCLASSIFIER_TOP_CTRL_ADDR_B_CONV1_BASE + offset + i);
    }
    return length;
}

u32 XClassifier_top_Get_b_conv2_BaseAddress(XClassifier_top *InstancePtr) {
    Xil_AssertNonvoid(InstancePtr != NULL);
    Xil_AssertNonvoid(InstancePtr->IsReady == XIL_COMPONENT_IS_READY);

    return (InstancePtr->Ctrl_BaseAddress + XCLASSIFIER_TOP_CTRL_ADDR_B_CONV2_BASE);
}

u32 XClassifier_top_Get_b_conv2_HighAddress(XClassifier_top *InstancePtr) {
    Xil_AssertNonvoid(InstancePtr != NULL);
    Xil_AssertNonvoid(InstancePtr->IsReady == XIL_COMPONENT_IS_READY);

    return (InstancePtr->Ctrl_BaseAddress + XCLASSIFIER_TOP_CTRL_ADDR_B_CONV2_HIGH);
}

u32 XClassifier_top_Get_b_conv2_TotalBytes(XClassifier_top *InstancePtr) {
    Xil_AssertNonvoid(InstancePtr != NULL);
    Xil_AssertNonvoid(InstancePtr->IsReady == XIL_COMPONENT_IS_READY);

    return (XCLASSIFIER_TOP_CTRL_ADDR_B_CONV2_HIGH - XCLASSIFIER_TOP_CTRL_ADDR_B_CONV2_BASE + 1);
}

u32 XClassifier_top_Get_b_conv2_BitWidth(XClassifier_top *InstancePtr) {
    Xil_AssertNonvoid(InstancePtr != NULL);
    Xil_AssertNonvoid(InstancePtr->IsReady == XIL_COMPONENT_IS_READY);

    return XCLASSIFIER_TOP_CTRL_WIDTH_B_CONV2;
}

u32 XClassifier_top_Get_b_conv2_Depth(XClassifier_top *InstancePtr) {
    Xil_AssertNonvoid(InstancePtr != NULL);
    Xil_AssertNonvoid(InstancePtr->IsReady == XIL_COMPONENT_IS_READY);

    return XCLASSIFIER_TOP_CTRL_DEPTH_B_CONV2;
}

u32 XClassifier_top_Write_b_conv2_Words(XClassifier_top *InstancePtr, int offset, word_type *data, int length) {
    Xil_AssertNonvoid(InstancePtr != NULL);
    Xil_AssertNonvoid(InstancePtr -> IsReady == XIL_COMPONENT_IS_READY);

    int i;

    if ((offset + length)*4 > (XCLASSIFIER_TOP_CTRL_ADDR_B_CONV2_HIGH - XCLASSIFIER_TOP_CTRL_ADDR_B_CONV2_BASE + 1))
        return 0;

    for (i = 0; i < length; i++) {
        *(int *)(InstancePtr->Ctrl_BaseAddress + XCLASSIFIER_TOP_CTRL_ADDR_B_CONV2_BASE + (offset + i)*4) = *(data + i);
    }
    return length;
}

u32 XClassifier_top_Read_b_conv2_Words(XClassifier_top *InstancePtr, int offset, word_type *data, int length) {
    Xil_AssertNonvoid(InstancePtr != NULL);
    Xil_AssertNonvoid(InstancePtr -> IsReady == XIL_COMPONENT_IS_READY);

    int i;

    if ((offset + length)*4 > (XCLASSIFIER_TOP_CTRL_ADDR_B_CONV2_HIGH - XCLASSIFIER_TOP_CTRL_ADDR_B_CONV2_BASE + 1))
        return 0;

    for (i = 0; i < length; i++) {
        *(data + i) = *(int *)(InstancePtr->Ctrl_BaseAddress + XCLASSIFIER_TOP_CTRL_ADDR_B_CONV2_BASE + (offset + i)*4);
    }
    return length;
}

u32 XClassifier_top_Write_b_conv2_Bytes(XClassifier_top *InstancePtr, int offset, char *data, int length) {
    Xil_AssertNonvoid(InstancePtr != NULL);
    Xil_AssertNonvoid(InstancePtr -> IsReady == XIL_COMPONENT_IS_READY);

    int i;

    if ((offset + length) > (XCLASSIFIER_TOP_CTRL_ADDR_B_CONV2_HIGH - XCLASSIFIER_TOP_CTRL_ADDR_B_CONV2_BASE + 1))
        return 0;

    for (i = 0; i < length; i++) {
        *(char *)(InstancePtr->Ctrl_BaseAddress + XCLASSIFIER_TOP_CTRL_ADDR_B_CONV2_BASE + offset + i) = *(data + i);
    }
    return length;
}

u32 XClassifier_top_Read_b_conv2_Bytes(XClassifier_top *InstancePtr, int offset, char *data, int length) {
    Xil_AssertNonvoid(InstancePtr != NULL);
    Xil_AssertNonvoid(InstancePtr -> IsReady == XIL_COMPONENT_IS_READY);

    int i;

    if ((offset + length) > (XCLASSIFIER_TOP_CTRL_ADDR_B_CONV2_HIGH - XCLASSIFIER_TOP_CTRL_ADDR_B_CONV2_BASE + 1))
        return 0;

    for (i = 0; i < length; i++) {
        *(data + i) = *(char *)(InstancePtr->Ctrl_BaseAddress + XCLASSIFIER_TOP_CTRL_ADDR_B_CONV2_BASE + offset + i);
    }
    return length;
}

void XClassifier_top_InterruptGlobalEnable(XClassifier_top *InstancePtr) {
    Xil_AssertVoid(InstancePtr != NULL);
    Xil_AssertVoid(InstancePtr->IsReady == XIL_COMPONENT_IS_READY);

    XClassifier_top_WriteReg(InstancePtr->Ctrl_BaseAddress, XCLASSIFIER_TOP_CTRL_ADDR_GIE, 1);
}

void XClassifier_top_InterruptGlobalDisable(XClassifier_top *InstancePtr) {
    Xil_AssertVoid(InstancePtr != NULL);
    Xil_AssertVoid(InstancePtr->IsReady == XIL_COMPONENT_IS_READY);

    XClassifier_top_WriteReg(InstancePtr->Ctrl_BaseAddress, XCLASSIFIER_TOP_CTRL_ADDR_GIE, 0);
}

void XClassifier_top_InterruptEnable(XClassifier_top *InstancePtr, u32 Mask) {
    u32 Register;

    Xil_AssertVoid(InstancePtr != NULL);
    Xil_AssertVoid(InstancePtr->IsReady == XIL_COMPONENT_IS_READY);

    Register =  XClassifier_top_ReadReg(InstancePtr->Ctrl_BaseAddress, XCLASSIFIER_TOP_CTRL_ADDR_IER);
    XClassifier_top_WriteReg(InstancePtr->Ctrl_BaseAddress, XCLASSIFIER_TOP_CTRL_ADDR_IER, Register | Mask);
}

void XClassifier_top_InterruptDisable(XClassifier_top *InstancePtr, u32 Mask) {
    u32 Register;

    Xil_AssertVoid(InstancePtr != NULL);
    Xil_AssertVoid(InstancePtr->IsReady == XIL_COMPONENT_IS_READY);

    Register =  XClassifier_top_ReadReg(InstancePtr->Ctrl_BaseAddress, XCLASSIFIER_TOP_CTRL_ADDR_IER);
    XClassifier_top_WriteReg(InstancePtr->Ctrl_BaseAddress, XCLASSIFIER_TOP_CTRL_ADDR_IER, Register & (~Mask));
}

void XClassifier_top_InterruptClear(XClassifier_top *InstancePtr, u32 Mask) {
    Xil_AssertVoid(InstancePtr != NULL);
    Xil_AssertVoid(InstancePtr->IsReady == XIL_COMPONENT_IS_READY);

    XClassifier_top_WriteReg(InstancePtr->Ctrl_BaseAddress, XCLASSIFIER_TOP_CTRL_ADDR_ISR, Mask);
}

u32 XClassifier_top_InterruptGetEnabled(XClassifier_top *InstancePtr) {
    Xil_AssertNonvoid(InstancePtr != NULL);
    Xil_AssertNonvoid(InstancePtr->IsReady == XIL_COMPONENT_IS_READY);

    return XClassifier_top_ReadReg(InstancePtr->Ctrl_BaseAddress, XCLASSIFIER_TOP_CTRL_ADDR_IER);
}

u32 XClassifier_top_InterruptGetStatus(XClassifier_top *InstancePtr) {
    Xil_AssertNonvoid(InstancePtr != NULL);
    Xil_AssertNonvoid(InstancePtr->IsReady == XIL_COMPONENT_IS_READY);

    return XClassifier_top_ReadReg(InstancePtr->Ctrl_BaseAddress, XCLASSIFIER_TOP_CTRL_ADDR_ISR);
}

