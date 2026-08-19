#ifndef PACK4_H
#define PACK4_H

#include <ap_int.h>

// ---------------------------------------------------------------------------
// int8x4 packing for the m_axi data path (pool_upsample_route "pack4" variant)
//
// WHY THIS EXISTS
// ---------------
// All 3 engines in this project (maxpool/upsample/route_concat) had already
// reached PIPELINE II=1 on every inner loop, and the real-shape cosim
// measurements landed exactly on the "1 element per beat" floor:
//
//     engine     measured        theoretical min at 1 elem/beat
//     maxpool    6.16 c/output   6  (4 window reads + init + write)
//     upsample   1.37 c/output   ~1.25
//     route      1.22 c/output   ~1
//
// So II=1 was never the limit - the limit is that each AXI beat carried a
// single int8. `m_axi_RD_BUS_WDATA` in the baseline project's own
// maxpool_engine_csynth.rpt is ALREADY 32 bits wide (with a 4-bit WSTRB);
// declaring the port as `ap_int<8>*` just meant 3 of every 4 byte lanes went
// unused on every beat. Packing 4 consecutive channels into each beat is
// therefore up to a 4x throughput win on these 3 engines.
//
// WHAT THIS DOES *NOT* CHANGE (checked, not assumed)
// --------------------------------------------------
//  1. The physical AXI port width. It was 32 bits before this change and is
//     32 bits after it - `ap_uint<32>*` produces exactly the same port
//     geometry that `ap_int<8>*` did. vivado/create_bd.tcl and the HP0/HP1
//     narrowing in create_system_bd.tcl §3 need NO edit.
//  2. The DDR byte layout, i.e. the PS-side data contract. On this
//     little-endian platform, 4 consecutive NHWC channels of an int8 array
//     are ALREADY the 4 bytes of one 32-bit word, lowest channel in the
//     lowest byte. Packing here is a pure reinterpretation of the same
//     bytes - no repacking pass on the PS side, and a buffer written by
//     conv_engine (still int8-addressed) can be read by these engines
//     unchanged.
//
// WHAT IT *DOES* REQUIRE (new contract - assert()ed in every engine)
// ------------------------------------------------------------------
//  1. `ch % 4 == 0` on every engine, plus `ch0 % 4 == 0` on route_concat
//     (src1's channels must start on a word boundary inside ofmap).
//     Every real YOLOv3-tiny-ADAS layer satisfies this: maxpool sees
//     ch = 16/32/64/128/256/512, upsample 128, route ch0/ch1 = 256/0 and
//     128/256.
//  2. Every base pointer must be 4-byte aligned in DDR. Any ordinary
//     allocator result (and any cacheline-aligned CMA buffer) already is.
//
// Both are real narrowings versus the baseline engines, which accepted any
// channel count. They are deliberate: a ch%4!=0 tail would need a
// read-modify-write on the final partial word of every pixel (the write
// side cannot use a whole-word store without clobbering the neighbouring
// pixel's bytes), which costs hardware and a hazard for a case this network
// never produces.
// ---------------------------------------------------------------------------

// One AXI beat = 4 consecutive NHWC channels. Lane L occupies bits
// [8L+7 : 8L], i.e. lane 0 is the LOW byte = the LOWEST channel index -
// matching little-endian DDR byte order, which is what makes point 2 above
// (unchanged PS-side layout) true.
typedef ap_uint<32> pack4_t;

const unsigned PACK4_LANES = 4;

// All 4 lanes = INT8 -128. Used as maxpool's window-init value, where the
// baseline engine wrote MAXPOOL_PAD_VALUE into each int8 slot separately.
const pack4_t PACK4_ALL_MINUS_128 = 0x80808080u;

// Bit-copy accessors, written as explicit .range() moves through a
// same-width temporary rather than a C-style cast between ap_uint<8> and
// ap_int<8>. A cast would rely on ap_int's modular-truncation rule to turn
// 0x80 into -128; the .range() form states the bit copy directly and is
// immune to that rule ever being the thing that is misread here. Same
// "explicit over clever" reasoning route_concat_engine.cpp's round_shift()
// already uses for avoiding a ternary on ap_int.
inline ap_int<8> pack4_get(pack4_t w, unsigned lane) {
#pragma HLS INLINE
    ap_int<8> v;
    v.range(7, 0) = w.range(8 * lane + 7, 8 * lane);
    return v;
}

inline void pack4_set(pack4_t &w, unsigned lane, ap_int<8> v) {
#pragma HLS INLINE
    w.range(8 * lane + 7, 8 * lane) = v.range(7, 0);
}

#endif // PACK4_H
