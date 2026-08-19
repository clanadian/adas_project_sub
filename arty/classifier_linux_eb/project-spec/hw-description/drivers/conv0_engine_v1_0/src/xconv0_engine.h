// ==============================================================
// Vitis HLS - High-Level Synthesis from C, C++ and OpenCL v2024.2 (64-bit)
// Tool Version Limit: 2024.11
// Copyright 1986-2022 Xilinx, Inc. All Rights Reserved.
// Copyright 2022-2024 Advanced Micro Devices, Inc. All Rights Reserved.
// 
// ==============================================================
#ifndef XCONV0_ENGINE_H
#define XCONV0_ENGINE_H

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
#include "xconv0_engine_hw.h"

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
    u64 Ctrl_BaseAddress;
} XConv0_engine_Config;
#endif

typedef struct {
    u64 Ctrl_BaseAddress;
    u32 IsReady;
} XConv0_engine;

typedef u32 word_type;

/***************** Macros (Inline Functions) Definitions *********************/
#ifndef __linux__
#define XConv0_engine_WriteReg(BaseAddress, RegOffset, Data) \
    Xil_Out32((BaseAddress) + (RegOffset), (u32)(Data))
#define XConv0_engine_ReadReg(BaseAddress, RegOffset) \
    Xil_In32((BaseAddress) + (RegOffset))
#else
#define XConv0_engine_WriteReg(BaseAddress, RegOffset, Data) \
    *(volatile u32*)((BaseAddress) + (RegOffset)) = (u32)(Data)
#define XConv0_engine_ReadReg(BaseAddress, RegOffset) \
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
int XConv0_engine_Initialize(XConv0_engine *InstancePtr, UINTPTR BaseAddress);
XConv0_engine_Config* XConv0_engine_LookupConfig(UINTPTR BaseAddress);
#else
int XConv0_engine_Initialize(XConv0_engine *InstancePtr, u16 DeviceId);
XConv0_engine_Config* XConv0_engine_LookupConfig(u16 DeviceId);
#endif
int XConv0_engine_CfgInitialize(XConv0_engine *InstancePtr, XConv0_engine_Config *ConfigPtr);
#else
int XConv0_engine_Initialize(XConv0_engine *InstancePtr, const char* InstanceName);
int XConv0_engine_Release(XConv0_engine *InstancePtr);
#endif

void XConv0_engine_Start(XConv0_engine *InstancePtr);
u32 XConv0_engine_IsDone(XConv0_engine *InstancePtr);
u32 XConv0_engine_IsIdle(XConv0_engine *InstancePtr);
u32 XConv0_engine_IsReady(XConv0_engine *InstancePtr);
void XConv0_engine_EnableAutoRestart(XConv0_engine *InstancePtr);
void XConv0_engine_DisableAutoRestart(XConv0_engine *InstancePtr);

void XConv0_engine_Set_ifmap(XConv0_engine *InstancePtr, u64 Data);
u64 XConv0_engine_Get_ifmap(XConv0_engine *InstancePtr);
void XConv0_engine_Set_weights(XConv0_engine *InstancePtr, u64 Data);
u64 XConv0_engine_Get_weights(XConv0_engine *InstancePtr);
void XConv0_engine_Set_bias(XConv0_engine *InstancePtr, u64 Data);
u64 XConv0_engine_Get_bias(XConv0_engine *InstancePtr);
void XConv0_engine_Set_ofmap(XConv0_engine *InstancePtr, u64 Data);
u64 XConv0_engine_Get_ofmap(XConv0_engine *InstancePtr);
void XConv0_engine_Set_img_h(XConv0_engine *InstancePtr, u32 Data);
u32 XConv0_engine_Get_img_h(XConv0_engine *InstancePtr);
void XConv0_engine_Set_img_w(XConv0_engine *InstancePtr, u32 Data);
u32 XConv0_engine_Get_img_w(XConv0_engine *InstancePtr);
void XConv0_engine_Set_requant_multiplier(XConv0_engine *InstancePtr, u32 Data);
u32 XConv0_engine_Get_requant_multiplier(XConv0_engine *InstancePtr);
void XConv0_engine_Set_requant_shift(XConv0_engine *InstancePtr, u32 Data);
u32 XConv0_engine_Get_requant_shift(XConv0_engine *InstancePtr);
void XConv0_engine_Set_leaky_relu_enable(XConv0_engine *InstancePtr, u32 Data);
u32 XConv0_engine_Get_leaky_relu_enable(XConv0_engine *InstancePtr);

void XConv0_engine_InterruptGlobalEnable(XConv0_engine *InstancePtr);
void XConv0_engine_InterruptGlobalDisable(XConv0_engine *InstancePtr);
void XConv0_engine_InterruptEnable(XConv0_engine *InstancePtr, u32 Mask);
void XConv0_engine_InterruptDisable(XConv0_engine *InstancePtr, u32 Mask);
void XConv0_engine_InterruptClear(XConv0_engine *InstancePtr, u32 Mask);
u32 XConv0_engine_InterruptGetEnabled(XConv0_engine *InstancePtr);
u32 XConv0_engine_InterruptGetStatus(XConv0_engine *InstancePtr);

#ifdef __cplusplus
}
#endif

#endif
