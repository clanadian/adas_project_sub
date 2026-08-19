/* Host shim for Xil_In32/Xil_Out32.
 *
 * Every write is RECORDED, not just stored: the point of this harness is to
 * check the SEQUENCE of register writes the sequencer produces, which is the
 * part no simulation of the engines can check (cosim drives the engines from
 * a generated testbench, and the HLS testbenches never touch s_axilite at
 * all). This is the same gap verif/ was created to close for the YOLO build.
 *
 * Xil_In32 on a CTRL register returns AP_IDLE|AP_DONE so the driver's poll
 * loops terminate immediately. That is a deliberate simplification: this
 * harness checks addresses and shapes, NOT handshake timing.
 */
#ifndef XIL_IO_H_SHIM
#define XIL_IO_H_SHIM
#include <stdint.h>
#include <stddef.h>

#define SHIM_MAX_TXN 4096
typedef struct { uint32_t addr, val; } shim_txn_t;
extern shim_txn_t shim_txn[SHIM_MAX_TXN];
extern unsigned   shim_n_txn;
extern int        shim_overflow;

static inline void Xil_Out32(uint32_t addr, uint32_t val)
{
    if (shim_n_txn < SHIM_MAX_TXN) {
        shim_txn[shim_n_txn].addr = addr;
        shim_txn[shim_n_txn].val  = val;
        shim_n_txn++;
    } else {
        shim_overflow = 1;   /* never silently drop - a dropped write would
                              * make a missing register look like a pass */
    }
}

static inline uint32_t Xil_In32(uint32_t addr)
{
    (void)addr;
    return (1u << 1) | (1u << 2);   /* AP_DONE | AP_IDLE */
}
#endif
