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
#include "xclassifier_top.h"

extern XClassifier_top_Config XClassifier_top_ConfigTable[];

#ifdef SDT
XClassifier_top_Config *XClassifier_top_LookupConfig(UINTPTR BaseAddress) {
	XClassifier_top_Config *ConfigPtr = NULL;

	int Index;

	for (Index = (u32)0x0; XClassifier_top_ConfigTable[Index].Name != NULL; Index++) {
		if (!BaseAddress || XClassifier_top_ConfigTable[Index].Control_BaseAddress == BaseAddress) {
			ConfigPtr = &XClassifier_top_ConfigTable[Index];
			break;
		}
	}

	return ConfigPtr;
}

int XClassifier_top_Initialize(XClassifier_top *InstancePtr, UINTPTR BaseAddress) {
	XClassifier_top_Config *ConfigPtr;

	Xil_AssertNonvoid(InstancePtr != NULL);

	ConfigPtr = XClassifier_top_LookupConfig(BaseAddress);
	if (ConfigPtr == NULL) {
		InstancePtr->IsReady = 0;
		return (XST_DEVICE_NOT_FOUND);
	}

	return XClassifier_top_CfgInitialize(InstancePtr, ConfigPtr);
}
#else
XClassifier_top_Config *XClassifier_top_LookupConfig(u16 DeviceId) {
	XClassifier_top_Config *ConfigPtr = NULL;

	int Index;

	for (Index = 0; Index < XPAR_XCLASSIFIER_TOP_NUM_INSTANCES; Index++) {
		if (XClassifier_top_ConfigTable[Index].DeviceId == DeviceId) {
			ConfigPtr = &XClassifier_top_ConfigTable[Index];
			break;
		}
	}

	return ConfigPtr;
}

int XClassifier_top_Initialize(XClassifier_top *InstancePtr, u16 DeviceId) {
	XClassifier_top_Config *ConfigPtr;

	Xil_AssertNonvoid(InstancePtr != NULL);

	ConfigPtr = XClassifier_top_LookupConfig(DeviceId);
	if (ConfigPtr == NULL) {
		InstancePtr->IsReady = 0;
		return (XST_DEVICE_NOT_FOUND);
	}

	return XClassifier_top_CfgInitialize(InstancePtr, ConfigPtr);
}
#endif

#endif

