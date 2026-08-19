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
#include "xmaxpool_engine.h"

extern XMaxpool_engine_Config XMaxpool_engine_ConfigTable[];

#ifdef SDT
XMaxpool_engine_Config *XMaxpool_engine_LookupConfig(UINTPTR BaseAddress) {
	XMaxpool_engine_Config *ConfigPtr = NULL;

	int Index;

	for (Index = (u32)0x0; XMaxpool_engine_ConfigTable[Index].Name != NULL; Index++) {
		if (!BaseAddress || XMaxpool_engine_ConfigTable[Index].Ctrl_BaseAddress == BaseAddress) {
			ConfigPtr = &XMaxpool_engine_ConfigTable[Index];
			break;
		}
	}

	return ConfigPtr;
}

int XMaxpool_engine_Initialize(XMaxpool_engine *InstancePtr, UINTPTR BaseAddress) {
	XMaxpool_engine_Config *ConfigPtr;

	Xil_AssertNonvoid(InstancePtr != NULL);

	ConfigPtr = XMaxpool_engine_LookupConfig(BaseAddress);
	if (ConfigPtr == NULL) {
		InstancePtr->IsReady = 0;
		return (XST_DEVICE_NOT_FOUND);
	}

	return XMaxpool_engine_CfgInitialize(InstancePtr, ConfigPtr);
}
#else
XMaxpool_engine_Config *XMaxpool_engine_LookupConfig(u16 DeviceId) {
	XMaxpool_engine_Config *ConfigPtr = NULL;

	int Index;

	for (Index = 0; Index < XPAR_XMAXPOOL_ENGINE_NUM_INSTANCES; Index++) {
		if (XMaxpool_engine_ConfigTable[Index].DeviceId == DeviceId) {
			ConfigPtr = &XMaxpool_engine_ConfigTable[Index];
			break;
		}
	}

	return ConfigPtr;
}

int XMaxpool_engine_Initialize(XMaxpool_engine *InstancePtr, u16 DeviceId) {
	XMaxpool_engine_Config *ConfigPtr;

	Xil_AssertNonvoid(InstancePtr != NULL);

	ConfigPtr = XMaxpool_engine_LookupConfig(DeviceId);
	if (ConfigPtr == NULL) {
		InstancePtr->IsReady = 0;
		return (XST_DEVICE_NOT_FOUND);
	}

	return XMaxpool_engine_CfgInitialize(InstancePtr, ConfigPtr);
}
#endif

#endif

