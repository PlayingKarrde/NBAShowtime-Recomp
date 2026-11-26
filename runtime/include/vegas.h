// Native model of the Midway Vegas board around the recompiled code:
// CP0 (status/cause/count), interrupt delivery into the recompiled BIOS, and the
// physical address map that routes MMIO to device models.
#ifndef NBA_VEGAS_H
#define NBA_VEGAS_H

#include <stdint.h>

#include "recomp.h"

#ifdef __cplusplus
extern "C" {
#endif

/* CPU interrupt lines as Cause/Status bits (IP0..IP7 = bits 8..15).
   IP2..IP6 are driven by the VRC5074 interrupt controller (nile.c); IP7 is the CP0 timer. */
enum {
  VEGAS_IRQ_TIMER = 1u << 15 /* IP7: CP0 Count == Compare */
};

typedef struct VegasCop0 {
  uint32_t sr;        /* $12 Status */
  uint32_t cause;     /* $13 Cause: IP0/IP1 software bits; hardware bits are computed */
  uint32_t epc;       /* $14 */
  uint32_t compare;   /* $11 */
  uint64_t cycles;    /* virtual time in Count units (CPU clock / 2 = 125 MHz) */
  uint32_t count_base;/* Count = (uint32_t)(cycles - count_base_cycles) */
  uint64_t count_base_cycles;
  uint32_t other[32]; /* remaining CP0 registers, stored but not interpreted */
} VegasCop0;

typedef struct VegasMachine {
  VegasCop0 cop0;
  uint32_t hw_irq;          /* asserted hardware interrupt lines (Cause IP2..IP7 bits) */
  int in_interrupt;         /* nesting guard while the BIOS ISR runs */
  uint64_t interrupts_delivered;
  uint64_t irq_count[8];    /* deliveries per Cause IP bit (diagnostics) */
  uint32_t count_step;      /* virtual cycles added per Count read (spin-loop progress) */
} VegasMachine;

extern VegasMachine g_vegas;

/* Initialise the machine. `disk_image` is the raw hard-disk image (chdman extracthd output);
   writes go to an in-memory overlay and the image file is never modified. `nile_regs` is the
   VRC5074 register dump taken with the RAM snapshot (interrupt routing, timers); `sio_regs` the
   SIO board register dump (interrupt enables); `dcs_rom` the DCS2 boot ROM (vegassio.bin) for the
   sound board (NULL = stand-in board). Returns 0 on success. */
int vegas_init(const char* disk_image, const char* nile_regs, const char* sio_regs, const char* dcs_rom);

/* Raise/lower a hardware interrupt line (VEGAS_IRQ_*). */
void vegas_set_irq(uint32_t line_bit, int asserted);

/* Deliver pending, enabled interrupts by running the recompiled BIOS exception path.
   Called from safe points (Count reads, MMIO reads, syscall return). */
void vegas_poll_interrupts(RecompContext* ctx);

/* Advance virtual time. */
void vegas_advance(uint64_t cycles);

/* Logging hook for unmapped MMIO and device diagnostics (default: stderr). */
extern void (*vegas_log)(const char* fmt, ...);

#ifdef __cplusplus
}
#endif
#endif /* NBA_VEGAS_H */
