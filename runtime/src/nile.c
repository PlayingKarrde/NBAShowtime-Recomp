// VRC5074 interrupt controller + 16550 UART.
#include "nile.h"

#include <stdio.h>
#include <string.h>

#include "vegas.h"

#define REG_INTCTRL_LO 0x88u /* sources 0-7: 4-bit fields {enable:1, route:3} */
#define REG_INTCTRL_HI 0x8Cu /* sources 8-15 */
#define REG_INTSTAT0 0x90u   /* INT0 status [15:0], INT1 status [31:16] */
#define REG_INTSTAT0_HI 0x94u /* INT2, INT3 */
#define REG_INTSTAT1 0x98u   /* INT4 */
#define REG_INTCLR 0xA0u     /* write 1 to clear a latched source */
#define UART_BASE 0x300u     /* 16550 registers at +0x300 + 8*n */
#define REG_PCIINIT0 0xF0u   /* PCI window 0 cycle type/upper address */
#define REG_PCIINIT1 0xF8u   /* PCI window 1: bits 3:1 type (3 = memory, 5 = config type 0) */
#define REG_TIMER_BASE 0x1C0u /* T0..T3: CTRL at +0x10n (low = initial count, high = control), CNTR at +8 */
#define PCICFG_BASE 0x200u   /* the VRC5074's own PCI configuration space */

static struct {
  uint32_t regs[0x200 / 4];
  uint16_t level;   /* level-triggered sources currently asserted (UART, PCI INTx: SIO, IDE) */
  uint16_t latched; /* edge events (timer reloads) pending until cleared with INTCLR */

  /* timers */
  uint64_t timer_start[4];
  uint32_t timer_init[4];
  uint64_t timer_wraps[4];

  /* own PCI config space (host bridge) */
  uint32_t pcicfg[64];

  /* UART */
  uint8_t ier, lcr, mcr, scr, dll, dlm, fcr;
  int thre_pending;
  char line[256];
  int line_len;
} nile;

static uint32_t intctrl(int src) {
  const uint32_t w = src < 8 ? nile.regs[REG_INTCTRL_LO / 4] : nile.regs[REG_INTCTRL_HI / 4];
  return (w >> (4 * (src & 7))) & 0xF;
}

static uint16_t line_status(int line) {
  uint16_t s = 0;
  for (int src = 0; src < 16; ++src) {
    const uint32_t c = intctrl(src);
    if ((c & 8) && (int)(c & 7) == line && ((nile.level | nile.latched) & (1u << src)))
      s |= (uint16_t)(1u << src);
  }
  return s;
}

static void update_cpu_lines(void) {
  for (int line = 0; line < 5; ++line) vegas_set_irq(1u << (10 + line), line_status(line) != 0);
}

void nile_set_source(int src, int on) {
  if (on) nile.level |= (uint16_t)(1u << src);
  else nile.level &= (uint16_t)~(1u << src);
  update_cpu_lines();
}

/* ---- UART ---- */
static void uart_update(void) { nile_set_source(NILE_SRC_UART, (nile.ier & 0x02) && nile.thre_pending); }

static void uart_putc(char c) {
  if (c == '\r') return;
  if (c == '\n' || nile.line_len == (int)sizeof nile.line - 1) {
    nile.line[nile.line_len] = 0;
    vegas_log("uart: %s", nile.line);
    nile.line_len = 0;
    if (c == '\n') return;
  }
  nile.line[nile.line_len++] = c;
}

/* Emit any partial line still buffered by the UART (diagnostics at exit). */
void nile_uart_flush(void) {
  if (!nile.line_len) return;
  nile.line[nile.line_len] = 0;
  vegas_log("uart (partial): %s", nile.line);
  nile.line_len = 0;
}

static uint32_t uart_read(int reg) {
  const int dlab = nile.lcr & 0x80;
  switch (reg) {
    case 0: return dlab ? nile.dll : 0; /* no receive data */
    case 1: return dlab ? nile.dlm : nile.ier;
    case 2: { /* IIR: THRE interrupt is cleared by reading IIR when it is the source */
      uint8_t iir = 0x01;
      if ((nile.ier & 0x02) && nile.thre_pending) { iir = 0x02; nile.thre_pending = 0; uart_update(); }
      return iir | ((nile.fcr & 1) ? 0xC0 : 0);
    }
    case 3: return nile.lcr;
    case 4: return nile.mcr;
    case 5: return 0x60; /* LSR: THR empty, transmitter empty, no RX data */
    case 6: return 0xB0; /* MSR: CTS, DSR, DCD */
    default: return nile.scr;
  }
}

static void uart_write(int reg, uint8_t v) {
  const int dlab = nile.lcr & 0x80;
  switch (reg) {
    case 0:
      if (dlab) { nile.dll = v; break; }
      uart_putc((char)v);
      nile.thre_pending = 1; /* transmits instantly */
      uart_update();
      break;
    case 1:
      if (dlab) { nile.dlm = v; break; }
      if ((v & 0x02) && !(nile.ier & 0x02)) nile.thre_pending = 1; /* THR is empty */
      nile.ier = v & 0x0F;
      uart_update();
      break;
    case 2: nile.fcr = v; break;
    case 3: nile.lcr = v; break;
    case 4: nile.mcr = v; break;
    case 7: nile.scr = v; break;
    default: break;
  }
}

/* ---- register file ---- */
void nile_init(const char* regs_path) {
  memset(&nile, 0, sizeof nile);
  if (regs_path) {
    FILE* f = fopen(regs_path, "rb");
    if (f) {
      if (fread(nile.regs, 1, sizeof nile.regs, f) != sizeof nile.regs) vegas_log("nile: short register dump");
      fclose(f);
    } else {
      vegas_log("nile: cannot open %s (interrupt routing will be off)", regs_path);
    }
  }
  nile.regs[REG_INTCLR / 4] = 0;
  for (int n = 0; n < 4; ++n) {
    nile.timer_init[n] = nile.regs[(REG_TIMER_BASE + 0x10u * (uint32_t)n) / 4];
    nile.timer_start[n] = 0;
  }
  nile.pcicfg[0] = 0x005A1033u; /* NEC VRC5074 */
  nile.pcicfg[2] = 0x06000004u; /* host bridge */
}

int nile_pci_is_config(void) {
  const uint32_t type = (nile.regs[REG_PCIINIT1 / 4] >> 1) & 7;
  return type == 5 || type == 7;
}

/* Timers count down at the 100 MHz bus clock from the CTRL reload value and reload on reaching 0;
   virtual time is kept in 125 MHz Count units. CTRL high word bit 0 = enable. */
static int timer_enabled(int n) { return (nile.regs[(REG_TIMER_BASE + 0x10u * (uint32_t)n) / 4 + 1] & 1u) != 0; }
static uint64_t timer_ticks(int n) { return (g_vegas.cop0.cycles - nile.timer_start[n]) * 4 / 5; }
static uint32_t timer_count(int n) {
  const uint64_t period = (uint64_t)nile.timer_init[n] + 1;
  return nile.timer_init[n] - (uint32_t)(timer_ticks(n) % period);
}

/* Interrupt source raised when each timer reloads (-1 = none). T3 is the BIOS 800 Hz tick:
   its handler (0x8001168C, VRC5074 source 5) increments the tick counter at 0x8002A270. */
static const int kTimerSource[4] = {-1, -1, -1, 5};

void nile_advance(void) {
  for (int n = 0; n < 4; ++n) {
    if (kTimerSource[n] < 0 || !timer_enabled(n) || !nile.timer_init[n]) continue;
    const uint64_t wraps = timer_ticks(n) / ((uint64_t)nile.timer_init[n] + 1);
    if (wraps != nile.timer_wraps[n]) {
      nile.timer_wraps[n] = wraps;
      nile.latched |= (uint16_t)(1u << kTimerSource[n]); /* edge: latched until INTCLR */
      update_cpu_lines();
    }
  }
}

int nile_handles(uint32_t p) { return p >= NILE_BASE && p < NILE_BASE + NILE_SIZE; }

uint32_t nile_read(uint32_t p, int size) {
  const uint32_t off = p - NILE_BASE;
  if (off >= UART_BASE && off < UART_BASE + 0x40) return uart_read((int)((off - UART_BASE) >> 3));
  const uint32_t aligned = off & ~3u;
  uint32_t v;
  if (aligned >= PCICFG_BASE && aligned < PCICFG_BASE + 0x100) {
    v = nile.pcicfg[(aligned - PCICFG_BASE) / 4];
    goto done;
  }
  if (aligned >= REG_TIMER_BASE && aligned < REG_TIMER_BASE + 0x40 && (aligned & 0xC) == 0x8) {
    vegas_advance(g_vegas.count_step); /* delay loops poll this: let time pass */
    v = timer_count((int)((aligned - REG_TIMER_BASE) >> 4));
    goto done;
  }
  switch (aligned) {
    case REG_INTSTAT0: v = line_status(0) | ((uint32_t)line_status(1) << 16); break;
    case REG_INTSTAT0_HI: v = line_status(2) | ((uint32_t)line_status(3) << 16); break;
    case REG_INTSTAT1: v = line_status(4); break;
    default:
      if (aligned < sizeof nile.regs) v = nile.regs[aligned / 4];
      else { vegas_log("nile: read%d of unknown register +%03X", size * 8, off); v = 0; }
  }
done:;
  const uint32_t shift = (off & 3u) * 8;
  if (size == 4) return v;
  if (size == 2) return (v >> shift) & 0xFFFF;
  return (v >> shift) & 0xFF;
}

void nile_write(uint32_t p, uint32_t value, int size) {
  const uint32_t off = p - NILE_BASE;
  if (off >= UART_BASE && off < UART_BASE + 0x40) { uart_write((int)((off - UART_BASE) >> 3), (uint8_t)value); return; }
  const uint32_t aligned = off & ~3u;
  const uint32_t shift = (off & 3u) * 8;
  const uint32_t mask = size == 4 ? 0xFFFFFFFFu : size == 2 ? (0xFFFFu << shift) : (0xFFu << shift);
  const uint32_t v = size == 4 ? value : (value << shift) & mask;
  if (aligned == REG_INTCLR) {
    nile.latched &= (uint16_t)~(v & 0xFFFF); /* clears edge events; level sources stay while asserted */
    update_cpu_lines();
    return;
  }
  if (aligned >= PCICFG_BASE && aligned < PCICFG_BASE + 0x100) {
    if (aligned >= PCICFG_BASE + 4) { /* vendor/device read-only */
      uint32_t* c = &nile.pcicfg[(aligned - PCICFG_BASE) / 4];
      *c = (*c & ~mask) | v;
    }
    return;
  }
  if (aligned >= REG_TIMER_BASE && aligned < REG_TIMER_BASE + 0x40 && (aligned & 0x8) == 0) {
    const int n = (int)((aligned - REG_TIMER_BASE) >> 4);
    nile.regs[aligned / 4] = (nile.regs[aligned / 4] & ~mask) | v;
    /* Only writing the reload value (low word) restarts the countdown. Toggling the enable bit
       (high word) does not: the BIOS timestamp (syscall 73) pauses T3 around each read. */
    if ((aligned & 0x4) == 0) {
      nile.timer_init[n] = nile.regs[aligned / 4];
      nile.timer_start[n] = g_vegas.cop0.cycles;
      nile.timer_wraps[n] = 0;
    }
    return;
  }
  if (aligned >= sizeof nile.regs) { vegas_log("nile: write%d of unknown register +%03X <- %08X", size * 8, off, value); return; }
  nile.regs[aligned / 4] = (nile.regs[aligned / 4] & ~mask) | v;
  if (aligned == REG_INTCTRL_LO || aligned == REG_INTCTRL_HI) update_cpu_lines();
}
