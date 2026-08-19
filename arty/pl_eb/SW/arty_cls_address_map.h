#ifndef ARTY_CLS_ADDRESS_MAP_H
#define ARTY_CLS_ADDRESS_MAP_H

/* Buffer byte counts are DERIVED from the network geometry - see the sizing
 * block below. This include is what makes that possible. */
#include "../HW/classifier_net.h"
/* ===========================================================================
 * Arty Z7-20 ROI classifier — PL address map
 *
 * Generated from the Vivado build's own assign_bd_address output
 * (this tree's logs/vivado_build.log), not written by hand.
 * The authoritative source once the PS project exists is the XSA's generated
 * xparameters.h; this header exists so PS work can start before that, and so
 * the two can be diffed.
 *
 *   TREE: hls/arty_96_classifier   (96x96 ROI)
 *   XSA : system/arty96_classifier_v1.xsa
 *   BD  : cls   ·  part xc7z020clg400-1  ·  FCLK0 100 MHz
 *
 * 2026-08-19 FIX: these two lines used to name `hls/arty_classifier/` - the
 * 64x64 tree - because this file was copied from there. The NUMBERS were
 * right (bases and every register offset are byte-identical across the 64 /
 * 96 / 128 trees; verified by hashing all offsets), but the PROVENANCE was
 * false, and this file ships to PS. A reader diffing it against the 64 tree's
 * copy sees the buffer-size defines differ (they are resolution-derived) and
 * has no way to tell which tree this one describes.
 *
 * ⚠️ If the BD is rebuilt with a different engine order, these move.
 *    python/check_shapes.py checks this file against the build log.
 * ===========================================================================
 */

/* --- s_axilite control windows (PS -> engines, via M_AXI_GP0) ------------ */
/* Each window is 64 KB. Offsets within a window come from the per-engine
 * driver headers (conv_engine_hw_driver.h etc.) - they are NOT the same
 * across engines, see the warning below. */
#define ARTY_CLS_CONV_ENGINE_0_BASE     0x40000000u   /* conv_engine    (TR=8) */
#define ARTY_CLS_CONV0_ENGINE_0_BASE    0x40010000u   /* conv0_engine          */
#define ARTY_CLS_MAXPOOL_ENGINE_0_BASE  0x40020000u   /* maxpool_engine        */
#define ARTY_CLS_CTRL_WINDOW_BYTES      0x10000u      /* 64K each              */

/* The driver headers expect these XPAR_* names. Define them here when
 * building against this map instead of a generated xparameters.h. */
#ifndef XPAR_CONV_ENGINE_0_S_AXI_CTRL_BASEADDR
#define XPAR_CONV_ENGINE_0_S_AXI_CTRL_BASEADDR     ARTY_CLS_CONV_ENGINE_0_BASE
#endif
#ifndef XPAR_CONV0_ENGINE_0_S_AXI_CTRL_BASEADDR
#define XPAR_CONV0_ENGINE_0_S_AXI_CTRL_BASEADDR    ARTY_CLS_CONV0_ENGINE_0_BASE
#endif
#ifndef XPAR_MAXPOOL_ENGINE_0_S_AXI_CTRL_BASEADDR
#define XPAR_MAXPOOL_ENGINE_0_S_AXI_CTRL_BASEADDR  ARTY_CLS_MAXPOOL_ENGINE_0_BASE
#endif

/* --- m_axi data path (engines -> DDR, via SmartConnect -> S_AXI_HP0) ----- */
/* All 7 master ports see the same 512 MB window at 0x0000_0000. Engine
 * pointer registers therefore take plain physical DDR addresses - no
 * translation, no per-port offset.
 *
 *   conv_engine_0    : RD_BUS, RD_BUS2, WR_BUS   (3)
 *   conv0_engine_0   : RD_BUS, WR_BUS            (2)
 *   maxpool_engine_0 : RD_BUS, WR_BUS            (2)
 */
#define ARTY_CLS_DDR_WINDOW_BASE        0x00000000u
#define ARTY_CLS_DDR_WINDOW_BYTES       0x20000000u   /* 512 MB (Arty DDR3) */
#define ARTY_CLS_MASTER_PORTS           7

/* --- buffer sizing (see HW/classifier_net.h) ----------------------------- */
/* Pointer registers are 32-bit LO + 32-bit HI. Zynq-7000 is 32-bit
 * addressed, so HI is always written as 0 - the drivers do this for you. */
/* ⚠️ Do not write these as literals. They are ROI_SIZE-dependent and this
 * tree shipped 64-build values into the 96 build once already - the sizes
 * were wrong while every shape in classifier_net.h was right. */
#define ARTY_CLS_WIRE_INPUT_BYTES       CLS_WIRE_INPUT_BYTES  /* PRE-PADDED  */
#define ARTY_CLS_ACT_BUF_BYTES          CLS_ACT_BUF_BYTES     /* ping-pong x2 */
#define ARTY_CLS_W_CONV0_BYTES            432u   /* OIHW, NOT transposed    */
#define ARTY_CLS_W_CONV1_BYTES           4608u   /* WPACK                   */
#define ARTY_CLS_W_CONV2_BYTES          18432u   /* WPACK                   */
#define ARTY_CLS_B_CONV0_BYTES             64u   /* int32 x16               */
#define ARTY_CLS_B_CONV1_BYTES            128u   /* int32 x32               */
#define ARTY_CLS_B_CONV2_BYTES            256u   /* int32 x64               */
#define ARTY_CLS_PL_OUTPUT_BYTES        CLS_PL_OUTPUT_BYTES   /* GAP input   */

/* Minimum alignment is 4 bytes (the m_axi ports are 32-bit). 64 is
 * recommended so each buffer owns its cache lines - the PS must flush before
 * a kernel start and invalidate after done, and partial lines are how that
 * goes wrong. */
#define ARTY_CLS_MIN_ALIGN_BYTES           4u
#define ARTY_CLS_RECOMMENDED_ALIGN_BYTES  64u

/* ===========================================================================
 * ⚠️ Register OFFSETS differ between engines. Do not reuse one engine's map.
 *
 *                      conv_engine   conv0_engine   maxpool_engine
 *   CTRL                     0x00           0x00             0x00
 *   ifmap  LO/HI        0x10/0x14      0x10/0x14        0x10/0x14
 *   weights LO/HI       0x1c/0x20      0x1c/0x20              n/a
 *   weights_hi LO/HI    0x28/0x2c            n/a              n/a
 *   bias   LO/HI        0x34/0x38      0x28/0x2c              n/a
 *   ofmap  LO/HI        0x40/0x44      0x34/0x38        0x1c/0x20
 *   img_h                    0x4c           0x40             0x28
 *   img_w                    0x54           0x48             0x30
 *   in_ch / ch               0x5c            n/a             0x38
 *   out_ch                   0x64            n/a              n/a
 *   k / stride / pad    0x6c/0x74/0x7c        n/a   n/a/0x40/0x48,0x50
 *   requant mult             0x84           0x50              n/a
 *   requant shift            0x8c           0x58              n/a
 *   leaky enable             0x94           0x60              n/a
 *
 * conv_engine and maxpool_engine headers additionally define SOME OF THESE
 * MACRO NAMES WITH DIFFERENT VALUES (REG_IMG_H, REG_IMG_W, REG_STRIDE,
 * REG_CTRL, REG_IFMAP_ADDR_*, REG_OFMAP_ADDR_*). Including both in one
 * translation unit produces redefinition warnings and, worse, makes any
 * later use of those bare names pick up whichever header came last.
 * SW/classifier_run.c breaks the chain with #undef between the includes.
 * conv0_engine_hw_driver.h prefixes everything with C0_ and does not collide.
 * =========================================================================== */

#endif /* ARTY_CLS_ADDRESS_MAP_H */
