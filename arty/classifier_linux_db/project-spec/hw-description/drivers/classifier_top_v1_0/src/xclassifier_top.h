// ==============================================================
// Vitis HLS - High-Level Synthesis from C, C++ and OpenCL v2024.2 (64-bit)
// Tool Version Limit: 2024.11
// Copyright 1986-2022 Xilinx, Inc. All Rights Reserved.
// Copyright 2022-2024 Advanced Micro Devices, Inc. All Rights Reserved.
// 
// ==============================================================
#ifndef XCLASSIFIER_TOP_H
#define XCLASSIFIER_TOP_H

#ifdef __cplusplus
extern "C" {
#endif

/***************************** Include Files *********************************/
#ifndef __linux__
#include "xil_types.h"
#include "xil_assert.h"
#include "xstatus.h"
#include "xil_io.h"
#else
#include <stdint.h>
#include <assert.h>
#include <dirent.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <unistd.h>
#include <stddef.h>
#endif
#include "xclassifier_top_hw.h"

/**************************** Type Definitions ******************************/
#ifdef __linux__
typedef uint8_t u8;
typedef uint16_t u16;
typedef uint32_t u32;
typedef uint64_t u64;
#else
typedef struct {
#ifdef SDT
    char *Name;
#else
    u16 DeviceId;
#endif
    u64 Control_BaseAddress;
    u64 Ctrl_BaseAddress;
} XClassifier_top_Config;
#endif

typedef struct {
    u64 Control_BaseAddress;
    u64 Ctrl_BaseAddress;
    u32 IsReady;
} XClassifier_top;

typedef u32 word_type;

/***************** Macros (Inline Functions) Definitions *********************/
#ifndef __linux__
#define XClassifier_top_WriteReg(BaseAddress, RegOffset, Data) \
    Xil_Out32((BaseAddress) + (RegOffset), (u32)(Data))
#define XClassifier_top_ReadReg(BaseAddress, RegOffset) \
    Xil_In32((BaseAddress) + (RegOffset))
#else
#define XClassifier_top_WriteReg(BaseAddress, RegOffset, Data) \
    *(volatile u32*)((BaseAddress) + (RegOffset)) = (u32)(Data)
#define XClassifier_top_ReadReg(BaseAddress, RegOffset) \
    *(volatile u32*)((BaseAddress) + (RegOffset))

#define Xil_AssertVoid(expr)    assert(expr)
#define Xil_AssertNonvoid(expr) assert(expr)

#define XST_SUCCESS             0
#define XST_DEVICE_NOT_FOUND    2
#define XST_OPEN_DEVICE_FAILED  3
#define XIL_COMPONENT_IS_READY  1
#endif

/************************** Function Prototypes *****************************/
#ifndef __linux__
#ifdef SDT
int XClassifier_top_Initialize(XClassifier_top *InstancePtr, UINTPTR BaseAddress);
XClassifier_top_Config* XClassifier_top_LookupConfig(UINTPTR BaseAddress);
#else
int XClassifier_top_Initialize(XClassifier_top *InstancePtr, u16 DeviceId);
XClassifier_top_Config* XClassifier_top_LookupConfig(u16 DeviceId);
#endif
int XClassifier_top_CfgInitialize(XClassifier_top *InstancePtr, XClassifier_top_Config *ConfigPtr);
#else
int XClassifier_top_Initialize(XClassifier_top *InstancePtr, const char* InstanceName);
int XClassifier_top_Release(XClassifier_top *InstancePtr);
#endif

void XClassifier_top_Start(XClassifier_top *InstancePtr);
u32 XClassifier_top_IsDone(XClassifier_top *InstancePtr);
u32 XClassifier_top_IsIdle(XClassifier_top *InstancePtr);
u32 XClassifier_top_IsReady(XClassifier_top *InstancePtr);
void XClassifier_top_EnableAutoRestart(XClassifier_top *InstancePtr);
void XClassifier_top_DisableAutoRestart(XClassifier_top *InstancePtr);

void XClassifier_top_Set_ifmap_padded(XClassifier_top *InstancePtr, u64 Data);
u64 XClassifier_top_Get_ifmap_padded(XClassifier_top *InstancePtr);
void XClassifier_top_Set_w_conv0(XClassifier_top *InstancePtr, u64 Data);
u64 XClassifier_top_Get_w_conv0(XClassifier_top *InstancePtr);
void XClassifier_top_Set_w_conv1(XClassifier_top *InstancePtr, u64 Data);
u64 XClassifier_top_Get_w_conv1(XClassifier_top *InstancePtr);
void XClassifier_top_Set_w_conv2(XClassifier_top *InstancePtr, u64 Data);
u64 XClassifier_top_Get_w_conv2(XClassifier_top *InstancePtr);
void XClassifier_top_Set_out_r(XClassifier_top *InstancePtr, u64 Data);
u64 XClassifier_top_Get_out_r(XClassifier_top *InstancePtr);
void XClassifier_top_Set_rq_conv0(XClassifier_top *InstancePtr, u64 Data);
u64 XClassifier_top_Get_rq_conv0(XClassifier_top *InstancePtr);
void XClassifier_top_Set_rq_conv1(XClassifier_top *InstancePtr, u64 Data);
u64 XClassifier_top_Get_rq_conv1(XClassifier_top *InstancePtr);
void XClassifier_top_Set_rq_conv2(XClassifier_top *InstancePtr, u64 Data);
u64 XClassifier_top_Get_rq_conv2(XClassifier_top *InstancePtr);
u32 XClassifier_top_Get_b_conv0_BaseAddress(XClassifier_top *InstancePtr);
u32 XClassifier_top_Get_b_conv0_HighAddress(XClassifier_top *InstancePtr);
u32 XClassifier_top_Get_b_conv0_TotalBytes(XClassifier_top *InstancePtr);
u32 XClassifier_top_Get_b_conv0_BitWidth(XClassifier_top *InstancePtr);
u32 XClassifier_top_Get_b_conv0_Depth(XClassifier_top *InstancePtr);
u32 XClassifier_top_Write_b_conv0_Words(XClassifier_top *InstancePtr, int offset, word_type *data, int length);
u32 XClassifier_top_Read_b_conv0_Words(XClassifier_top *InstancePtr, int offset, word_type *data, int length);
u32 XClassifier_top_Write_b_conv0_Bytes(XClassifier_top *InstancePtr, int offset, char *data, int length);
u32 XClassifier_top_Read_b_conv0_Bytes(XClassifier_top *InstancePtr, int offset, char *data, int length);
u32 XClassifier_top_Get_b_conv1_BaseAddress(XClassifier_top *InstancePtr);
u32 XClassifier_top_Get_b_conv1_HighAddress(XClassifier_top *InstancePtr);
u32 XClassifier_top_Get_b_conv1_TotalBytes(XClassifier_top *InstancePtr);
u32 XClassifier_top_Get_b_conv1_BitWidth(XClassifier_top *InstancePtr);
u32 XClassifier_top_Get_b_conv1_Depth(XClassifier_top *InstancePtr);
u32 XClassifier_top_Write_b_conv1_Words(XClassifier_top *InstancePtr, int offset, word_type *data, int length);
u32 XClassifier_top_Read_b_conv1_Words(XClassifier_top *InstancePtr, int offset, word_type *data, int length);
u32 XClassifier_top_Write_b_conv1_Bytes(XClassifier_top *InstancePtr, int offset, char *data, int length);
u32 XClassifier_top_Read_b_conv1_Bytes(XClassifier_top *InstancePtr, int offset, char *data, int length);
u32 XClassifier_top_Get_b_conv2_BaseAddress(XClassifier_top *InstancePtr);
u32 XClassifier_top_Get_b_conv2_HighAddress(XClassifier_top *InstancePtr);
u32 XClassifier_top_Get_b_conv2_TotalBytes(XClassifier_top *InstancePtr);
u32 XClassifier_top_Get_b_conv2_BitWidth(XClassifier_top *InstancePtr);
u32 XClassifier_top_Get_b_conv2_Depth(XClassifier_top *InstancePtr);
u32 XClassifier_top_Write_b_conv2_Words(XClassifier_top *InstancePtr, int offset, word_type *data, int length);
u32 XClassifier_top_Read_b_conv2_Words(XClassifier_top *InstancePtr, int offset, word_type *data, int length);
u32 XClassifier_top_Write_b_conv2_Bytes(XClassifier_top *InstancePtr, int offset, char *data, int length);
u32 XClassifier_top_Read_b_conv2_Bytes(XClassifier_top *InstancePtr, int offset, char *data, int length);

void XClassifier_top_InterruptGlobalEnable(XClassifier_top *InstancePtr);
void XClassifier_top_InterruptGlobalDisable(XClassifier_top *InstancePtr);
void XClassifier_top_InterruptEnable(XClassifier_top *InstancePtr, u32 Mask);
void XClassifier_top_InterruptDisable(XClassifier_top *InstancePtr, u32 Mask);
void XClassifier_top_InterruptClear(XClassifier_top *InstancePtr, u32 Mask);
u32 XClassifier_top_InterruptGetEnabled(XClassifier_top *InstancePtr);
u32 XClassifier_top_InterruptGetStatus(XClassifier_top *InstancePtr);

#ifdef __cplusplus
}
#endif

#endif
