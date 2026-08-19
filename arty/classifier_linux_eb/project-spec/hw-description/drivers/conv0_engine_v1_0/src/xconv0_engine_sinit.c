// ==============================================================
// Vitis HLS - High-Level Synthesis from C, C++ and OpenCL v2024.2 (64-bit)
// Tool Version Limit: 2024.11
// Copyright 1986-2022 Xilinx, Inc. All Rights Reserved.
// Copyright 2022-2024 Advanced Micro Devices, Inc. All Rights Reserved.
// 
// ==============================================================
#ifndef __linux__

#include "xstatus.h"
#ifdef SDT
#include "xparameters.h"
#endif
#include "xconv0_engine.h"

extern XConv0_engine_Config XConv0_engine_ConfigTable[];

#ifdef SDT
XConv0_engine_Config *XConv0_engine_LookupConfig(UINTPTR BaseAddress) {
	XConv0_engine_Config *ConfigPtr = NULL;

	int Index;

	for (Index = (u32)0x0; XConv0_engine_ConfigTable[Index].Name != NULL; Index++) {
		if (!BaseAddress || XConv0_engine_ConfigTable[Index].Ctrl_BaseAddress == BaseAddress) {
			ConfigPtr = &XConv0_engine_ConfigTable[Index];
			break;
		}
	}

	return ConfigPtr;
}

int XConv0_engine_Initialize(XConv0_engine *InstancePtr, UINTPTR BaseAddress) {
	XConv0_engine_Config *ConfigPtr;

	Xil_AssertNonvoid(InstancePtr != NULL);

	ConfigPtr = XConv0_engine_LookupConfig(BaseAddress);
	if (ConfigPtr == NULL) {
		InstancePtr->IsReady = 0;
		return (XST_DEVICE_NOT_FOUND);
	}

	return XConv0_engine_CfgInitialize(InstancePtr, ConfigPtr);
}
#else
XConv0_engine_Config *XConv0_engine_LookupConfig(u16 DeviceId) {
	XConv0_engine_Config *ConfigPtr = NULL;

	int Index;

	for (Index = 0; Index < XPAR_XCONV0_ENGINE_NUM_INSTANCES; Index++) {
		if (XConv0_engine_ConfigTable[Index].DeviceId == DeviceId) {
			ConfigPtr = &XConv0_engine_ConfigTable[Index];
			break;
		}
	}

	return ConfigPtr;
}

int XConv0_engine_Initialize(XConv0_engine *InstancePtr, u16 DeviceId) {
	XConv0_engine_Config *ConfigPtr;

	Xil_AssertNonvoid(InstancePtr != NULL);

	ConfigPtr = XConv0_engine_LookupConfig(DeviceId);
	if (ConfigPtr == NULL) {
		InstancePtr->IsReady = 0;
		return (XST_DEVICE_NOT_FOUND);
	}

	return XConv0_engine_CfgInitialize(InstancePtr, ConfigPtr);
}
#endif

#endif

