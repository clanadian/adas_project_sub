/* Host shim - fake s_axilite base addresses. Values are arbitrary but must be
 * far apart and 64 KB aligned, so a stray write to one engine's window cannot
 * land in the other's and go unnoticed. The REAL addresses come from the
 * Vivado address editor; this file exists only so the driver headers compile
 * and run on a PC. */
#ifndef XPARAMETERS_H_SHIM
#define XPARAMETERS_H_SHIM
#define XPAR_CONV_ENGINE_0_S_AXI_CTRL_BASEADDR     0x40000000u
#define XPAR_CONV0_ENGINE_0_S_AXI_CTRL_BASEADDR    0x40010000u
#define XPAR_MAXPOOL_ENGINE_0_S_AXI_CTRL_BASEADDR  0x40020000u
#endif
