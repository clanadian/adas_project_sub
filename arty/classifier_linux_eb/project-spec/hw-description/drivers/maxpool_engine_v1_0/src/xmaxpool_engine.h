// ==============================================================
// Vitis HLS - High-Level Synthesis from C, C++ and OpenCL v2024.2 (64-bit)
// Tool Version Limit: 2024.11
// Copyright 1986-2022 Xilinx, Inc. All Rights Reserved.
// Copyright 2022-2024 Advanced Micro Devices, Inc. All Rights Reserved.
// 
// ==============================================================
#ifndef XMAXPOOL_ENGINE_H
#define XMAXPOOL_ENGINE_H

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
#include "xmaxpool_engine_hw.h"

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
} XMaxpool_engine_Config;
#endif

typedef struct {
    u64 Ctrl_BaseAddress;
    u32 IsReady;
} XMaxpool_engine;

typedef u32 word_type;

/***************** Macros (Inline Functions) Definitions *********************/
#ifndef __linux__
#define XMaxpool_engine_WriteReg(BaseAddress, RegOffset, Data) \
    Xil_Out32((BaseAddress) + (RegOffset), (u32)(Data))
#define XMaxpool_engine_ReadReg(BaseAddress, RegOffset) \
    Xil_In32((BaseAddress) + (RegOffset))
#else
#define XMaxpool_engine_WriteReg(BaseAddress, RegOffset, Data) \
    *(volatile u32*)((BaseAddress) + (RegOffset)) = (u32)(Data)
#define XMaxpool_engine_ReadReg(BaseAddress, RegOffset) \
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
int XMaxpool_engine_Initialize(XMaxpool_engine *InstancePtr, UINTPTR BaseAddress);
XMaxpool_engine_Config* XMaxpool_engine_LookupConfig(UINTPTR BaseAddress);
#else
int XMaxpool_engine_Initialize(XMaxpool_engine *InstancePtr, u16 DeviceId);
XMaxpool_engine_Config* XMaxpool_engine_LookupConfig(u16 DeviceId);
#endif
int XMaxpool_engine_CfgInitialize(XMaxpool_engine *InstancePtr, XMaxpool_engine_Config *ConfigPtr);
#else
int XMaxpool_engine_Initialize(XMaxpool_engine *InstancePtr, const char* InstanceName);
int XMaxpool_engine_Release(XMaxpool_engine *InstancePtr);
#endif

void XMaxpool_engine_Start(XMaxpool_engine *InstancePtr);
u32 XMaxpool_engine_IsDone(XMaxpool_engine *InstancePtr);
u32 XMaxpool_engine_IsIdle(XMaxpool_engine *InstancePtr);
u32 XMaxpool_engine_IsReady(XMaxpool_engine *InstancePtr);
void XMaxpool_engine_EnableAutoRestart(XMaxpool_engine *InstancePtr);
void XMaxpool_engine_DisableAutoRestart(XMaxpool_engine *InstancePtr);

void XMaxpool_engine_Set_ifmap(XMaxpool_engine *InstancePtr, u64 Data);
u64 XMaxpool_engine_Get_ifmap(XMaxpool_engine *InstancePtr);
void XMaxpool_engine_Set_ofmap(XMaxpool_engine *InstancePtr, u64 Data);
u64 XMaxpool_engine_Get_ofmap(XMaxpool_engine *InstancePtr);
void XMaxpool_engine_Set_img_h(XMaxpool_engine *InstancePtr, u32 Data);
u32 XMaxpool_engine_Get_img_h(XMaxpool_engine *InstancePtr);
void XMaxpool_engine_Set_img_w(XMaxpool_engine *InstancePtr, u32 Data);
u32 XMaxpool_engine_Get_img_w(XMaxpool_engine *InstancePtr);
void XMaxpool_engine_Set_ch(XMaxpool_engine *InstancePtr, u32 Data);
u32 XMaxpool_engine_Get_ch(XMaxpool_engine *InstancePtr);
void XMaxpool_engine_Set_stride(XMaxpool_engine *InstancePtr, u32 Data);
u32 XMaxpool_engine_Get_stride(XMaxpool_engine *InstancePtr);
void XMaxpool_engine_Set_pad_right(XMaxpool_engine *InstancePtr, u32 Data);
u32 XMaxpool_engine_Get_pad_right(XMaxpool_engine *InstancePtr);
void XMaxpool_engine_Set_pad_bottom(XMaxpool_engine *InstancePtr, u32 Data);
u32 XMaxpool_engine_Get_pad_bottom(XMaxpool_engine *InstancePtr);

void XMaxpool_engine_InterruptGlobalEnable(XMaxpool_engine *InstancePtr);
void XMaxpool_engine_InterruptGlobalDisable(XMaxpool_engine *InstancePtr);
void XMaxpool_engine_InterruptEnable(XMaxpool_engine *InstancePtr, u32 Mask);
void XMaxpool_engine_InterruptDisable(XMaxpool_engine *InstancePtr, u32 Mask);
void XMaxpool_engine_InterruptClear(XMaxpool_engine *InstancePtr, u32 Mask);
u32 XMaxpool_engine_InterruptGetEnabled(XMaxpool_engine *InstancePtr);
u32 XMaxpool_engine_InterruptGetStatus(XMaxpool_engine *InstancePtr);

#ifdef __cplusplus
}
#endif

#endif
