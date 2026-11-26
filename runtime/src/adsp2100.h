// Analog Devices ADSP-2100 family DSP core (ADSP-2104 flavour: 2101-style interrupts), used by
// the DCS2 sound board. Written from the ADSP-2100 family instruction set; one instruction per
// cycle, no wait states.
#ifndef NBA_ADSP2100_H
#define NBA_ADSP2100_H

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* Interrupt priorities / vector indices (vector address = 0x04 + 4 * n, IMASK bit = 0x20 >> n). */
enum { ADSP_IRQ2 = 0, ADSP_SPORT0_TX = 1, ADSP_SPORT0_RX = 2, ADSP_IRQ1 = 3, ADSP_IRQ0 = 4, ADSP_TIMER = 5 };

/* MSTAT bits */
enum {
  ADSP_MSTAT_SEC_REG = 0x01,  /* secondary data register bank */
  ADSP_MSTAT_BIT_REV = 0x02,  /* DAG1 bit-reversed addressing */
  ADSP_MSTAT_AV_LATCH = 0x04, /* sticky ALU overflow */
  ADSP_MSTAT_AR_SAT = 0x08,   /* AR saturation */
  ADSP_MSTAT_M_MODE = 0x10,   /* 1 = integer multiply, 0 = fractional */
  ADSP_MSTAT_TIMER = 0x20,    /* timer enable */
  ADSP_MSTAT_G_MODE = 0x40,
};

typedef struct Adsp2100 {
  /* computation registers, two banks: AX0 AX1 MX0 MX1 AY0 AY1 MY0 MY1 SI SE AR MR0 MR1 MR2 SR0 SR1 AF MF */
  uint16_t bank[2][18];
  uint16_t* r; /* active bank */

  uint16_t i[8], m[8], l[8];
  uint16_t pc;
  uint16_t pc_stack[16];
  uint32_t loop_stack[4]; /* (end address << 4) | condition */
  uint16_t cntr_stack[4];
  uint16_t stat_stack[4][3]; /* ASTAT, MSTAT, IMASK */
  int pc_sp, loop_sp, cntr_sp, stat_sp;
  uint16_t loop_end;       /* active loop end address (0xFFFF = none) */
  int loop_cond;
  uint16_t astat, mstat, sstat, imask, icntl, cntr, px;
  int16_t sb;
  uint16_t rx0, tx0, rx1, tx1;
  int idle;
  uint8_t irq_latch[6];
  uint8_t irq_line[6];     /* level-sensitive lines (IRQ2/IRQ1/IRQ0) */
  uint64_t cycles;

  /* debugging: ring buffer of recently executed instructions */
  int trace_on;
  uint16_t trace_pc[256];
  uint32_t trace_op[256];
  uint32_t trace_pos;

  /* memory / board callbacks */
  void* user;
  uint16_t (*dm_read)(void* user, uint16_t addr);
  void (*dm_write)(void* user, uint16_t addr, uint16_t value);
  uint32_t (*pm_read)(void* user, uint16_t addr);
  void (*pm_write)(void* user, uint16_t addr, uint32_t value);
  void (*mstat_changed)(void* user, uint16_t old_mstat, uint16_t new_mstat);
} Adsp2100;

void adsp_reset(Adsp2100* c);
/* Run for about `cycles` instructions; returns the number executed. */
int adsp_run(Adsp2100* c, int cycles);
/* Level lines (IRQ2/IRQ1/IRQ0): assert/clear. Edge interrupts: adsp_pulse_irq. */
void adsp_set_irq_line(Adsp2100* c, int irq, int state);
void adsp_pulse_irq(Adsp2100* c, int irq);

/* Register access for the board (autobuffering uses I/M/L). */
static inline uint16_t adsp_get_i(const Adsp2100* c, int n) { return c->i[n]; }
static inline void adsp_set_i(Adsp2100* c, int n, uint16_t v) { c->i[n] = v & 0x3FFF; }

/* Disassemble one instruction (debugging). Returns `out`. */
char* adsp_disasm(uint32_t op, uint16_t pc, char* out, int size);

#ifdef __cplusplus
}
#endif
#endif
