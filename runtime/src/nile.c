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

static struct {
  uint32_t regs[0x200 / 4];
  uint16_t level;   /* sources currently asserted by devices */
  uint16_t latched; /* sources latched as pending */

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
    if ((c & 8) && (int)(c & 7) == line && (nile.latched & (1u << src))) s |= (uint16_t)(1u << src);
  }
  return s;
}

static void update_cpu_lines(void) {
  nile.latched |= nile.level;
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
}

int nile_handles(uint32_t p) { return p >= NILE_BASE && p < NILE_BASE + NILE_SIZE; }

uint32_t nile_read(uint32_t p, int size) {
  const uint32_t off = p - NILE_BASE;
  if (off >= UART_BASE && off < UART_BASE + 0x40) return uart_read((int)((off - UART_BASE) >> 3));
  const uint32_t aligned = off & ~3u;
  uint32_t v;
  switch (aligned) {
    case REG_INTSTAT0: v = line_status(0) | ((uint32_t)line_status(1) << 16); break;
    case REG_INTSTAT0_HI: v = line_status(2) | ((uint32_t)line_status(3) << 16); break;
    case REG_INTSTAT1: v = line_status(4); break;
    default:
      if (aligned < sizeof nile.regs) v = nile.regs[aligned / 4];
      else { vegas_log("nile: read%d of unknown register +%03X", size * 8, off); v = 0; }
  }
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
    nile.latched &= (uint16_t)~(v & 0xFFFF);
    update_cpu_lines(); /* level sources re-latch immediately */
    return;
  }
  if (aligned >= sizeof nile.regs) { vegas_log("nile: write%d of unknown register +%03X <- %08X", size * 8, off, value); return; }
  nile.regs[aligned / 4] = (nile.regs[aligned / 4] & ~mask) | v;
  if (aligned == REG_INTCTRL_LO || aligned == REG_INTCTRL_HI) update_cpu_lines();
}
