// Vegas machine core: CP0, interrupt delivery into the recompiled BIOS, MMIO routing.
#include <stdarg.h>
#include <stdio.h>
#include <string.h>

#include "banshee.h"
#include "dcs2.h"
#include "ide.h"
#include "ioasic.h"
#include "nile.h"
#include "nvram.h"
#include "pci.h"
#include "sio.h"
#include "vegas.h"

VegasMachine g_vegas;

static void default_log(const char* fmt, ...) {
  va_list ap;
  va_start(ap, fmt);
  vfprintf(stderr, fmt, ap);
  va_end(ap);
  fputc('\n', stderr);
}
void (*vegas_log)(const char* fmt, ...) = default_log;
void (*vegas_vblank_hook)(void);

/* ---- BIOS exception path (found by disassembly, see docs/interrupts.md) ---- */
#define BIOS_EXC_FRAME 0x8002B710u    /* register save area used by the exception entry */
#define BIOS_EXC_HANDLER 0x80013728u  /* C-level handler: a0 = Cause, uses the frame above */
#define FRAME_LO 0x100u
#define FRAME_HI 0x108u
#define FRAME_EPC 0x110u
#define FRAME_COP0(n) (0x118u + 8u * (n)) /* CP0 register n as saved by the entry code */

int vegas_init(const char* disk_image, const char* nile_regs, const char* sio_regs, const char* dcs_rom) {
  memset(&g_vegas, 0, sizeof g_vegas);
  g_vegas.cop0.sr = 0x30011C01u; /* value at game entry (MAME snapshot) */
  g_vegas.cop0.compare = 0xFFFFFFFFu;
  g_vegas.count_step = 40;       /* ~0.3 us per Count/timer read; loops advance time via RECOMP_LOOP */
  nile_init(nile_regs);
  ioasic_init();
  dcs2_init(dcs_rom); /* without the boot ROM the I/O ASIC falls back to its stand-in board */
  sio_init(sio_regs);
  nvram_init(NULL); /* blank board: the game validates and restores defaults */
  pci_init();
  banshee_init();
  return ide_init(disk_image);
}

void vegas_set_irq(uint32_t line_bit, int asserted) {
  if (asserted) g_vegas.hw_irq |= line_bit;
  else g_vegas.hw_irq &= ~line_bit;
}

static uint32_t count_now(void) {
  return (uint32_t)(g_vegas.cop0.cycles - g_vegas.cop0.count_base_cycles) + g_vegas.cop0.count_base;
}

void vegas_advance(uint64_t cycles) {
  uint32_t before = count_now();
  g_vegas.cop0.cycles += cycles;
  uint32_t after = count_now();
  /* Count/Compare timer: fires when Count passes Compare. */
  if ((uint32_t)(g_vegas.cop0.compare - before) <= (uint32_t)(after - before) && after != before)
    vegas_set_irq(VEGAS_IRQ_TIMER, 1);
  nile_advance();
  const uint64_t frames = sio_frames();
  sio_advance(g_vegas.cop0.cycles);
  dcs2_sync(g_vegas.cop0.cycles);
  if (vegas_vblank_hook && sio_frames() != frames) vegas_vblank_hook();
}

static uint32_t cause_now(void) {
  return (g_vegas.cop0.cause & ~0xFC00u) | (g_vegas.hw_irq & 0xFC00u);
}

static void store64(RecompContext* ctx, uint32_t addr, uint64_t v) { mem_w64(ctx, addr, v); }

void vegas_poll_interrupts(RecompContext* ctx) {
  for (int guard = 0; guard < 8; ++guard) {
    if (g_vegas.in_interrupt) return;
    const uint32_t sr = g_vegas.cop0.sr;
    if (!(sr & 1u) || (sr & 6u)) return; /* IE clear, or EXL/ERL set */
    const uint32_t cause = cause_now() & ~0x7Cu; /* ExcCode 0 = interrupt */
    if (!(cause & sr & 0xFF00u)) return;

    /* Enter the exception the way the BIOS assembly entry does: IE off, EXL on,
       registers saved to the frame, then the C-level handler with a0 = Cause. */
    RecompContext saved = *ctx;
    const uint32_t entry_sr = (sr & ~1u) | 2u;
    for (int r = 1; r < 32; ++r) store64(ctx, BIOS_EXC_FRAME + 8u * r, ctx->r[r]);
    store64(ctx, BIOS_EXC_FRAME + FRAME_LO, ctx->lo);
    store64(ctx, BIOS_EXC_FRAME + FRAME_HI, ctx->hi);
    store64(ctx, BIOS_EXC_FRAME + FRAME_EPC, g_vegas.cop0.epc);
    store64(ctx, BIOS_EXC_FRAME + FRAME_COP0(12), SE32(entry_sr));
    store64(ctx, BIOS_EXC_FRAME + FRAME_COP0(13), SE32(cause));
    store64(ctx, BIOS_EXC_FRAME + FRAME_COP0(14), g_vegas.cop0.epc);

    g_vegas.cop0.sr = entry_sr;
    g_vegas.cop0.cause = cause;
    g_vegas.in_interrupt = 1;
    g_vegas.interrupts_delivered++;
    for (int ip = 7; ip >= 0; --ip)
      if (cause & sr & (0x100u << ip)) { g_vegas.irq_count[ip]++; break; }
    ctx->r[4] = SE32(cause);
    recomp_lookup(ctx, BIOS_EXC_HANDLER)(ctx);
    g_vegas.in_interrupt = 0;

    /* Return path: SR comes back from the frame with IE forced on; eret clears EXL. */
    const uint32_t frame_sr = mem_r32(ctx, BIOS_EXC_FRAME + FRAME_COP0(12));
    *ctx = saved;
    g_vegas.cop0.sr = (frame_sr | 1u) & ~2u;
  }
}

/* ---- loop budget ---- */
#define SLICE_INSTRUCTIONS 4000 /* ~16 us of R5000 time between interrupt checks in tight loops */
int32_t recomp_budget = SLICE_INSTRUCTIONS;

void recomp_slice(RecompContext* ctx) {
  const int32_t used = SLICE_INSTRUCTIONS - recomp_budget;
  recomp_budget = SLICE_INSTRUCTIONS;
  vegas_advance((uint64_t)used / 2); /* ~1 instruction per CPU cycle; Count runs at half speed */
  vegas_poll_interrupts(ctx);
}

/* ---- CP0 hooks called by recompiled code ---- */
uint64_t recomp_cop0_read(RecompContext* ctx, int reg, uint32_t pc) {
  (void)pc;
  switch (reg) {
    case 9:
      vegas_advance(g_vegas.count_step);
      vegas_poll_interrupts(ctx);
      return SE32(count_now());
    case 11: return SE32(g_vegas.cop0.compare);
    case 12: return SE32(g_vegas.cop0.sr);
    case 13: return SE32(cause_now());
    case 14: return SE32(g_vegas.cop0.epc);
    default: return SE32(g_vegas.cop0.other[reg & 31]);
  }
}

void recomp_cop0_write(RecompContext* ctx, int reg, uint64_t value, uint32_t pc) {
  (void)pc;
  const uint32_t v = (uint32_t)value;
  switch (reg) {
    case 9:
      g_vegas.cop0.count_base = v;
      g_vegas.cop0.count_base_cycles = g_vegas.cop0.cycles;
      break;
    case 11:
      g_vegas.cop0.compare = v;
      vegas_set_irq(VEGAS_IRQ_TIMER, 0); /* writing Compare acknowledges the timer */
      break;
    case 12:
      g_vegas.cop0.sr = v;
      vegas_poll_interrupts(ctx); /* enabling interrupts may let a pending one in */
      break;
    case 13:
      g_vegas.cop0.cause = (g_vegas.cop0.cause & ~0x300u) | (v & 0x300u); /* only IP0/IP1 writable */
      break;
    case 14: g_vegas.cop0.epc = v; break;
    default: g_vegas.cop0.other[reg & 31] = v; break;
  }
}

/* ---- physical address map ---- */
#define VEGAS_LED 0x04600000u
static uint8_t g_vegas_led;

uint32_t recomp_mmio_read(RecompContext* ctx, uint32_t vaddr, int size) {
  const uint32_t p = recomp_phys(vaddr);
  uint32_t v;
  if (pci_handles(p)) {
    v = pci_read(p, size);
  } else if (ide_handles(p)) {
    v = ide_read(p, size);
  } else if (banshee_io_handles(p)) {
    v = banshee_io_read(p, size);
  } else if (nile_handles(p)) {
    v = nile_read(p, size);
  } else if (ioasic_handles(p)) {
    v = ioasic_read(p, size);
  } else if (sio_handles(p)) {
    v = sio_read(p, size);
  } else if (nvram_handles(p)) {
    v = nvram_read(p, size);
  } else if (p == VEGAS_LED) {
    v = g_vegas_led;
  } else {
    vegas_log("mmio: unmapped read%d %08X", size * 8, vaddr);
    v = 0;
  }
  vegas_advance(4);
  vegas_poll_interrupts(ctx);
  return v;
}

void recomp_mmio_write(RecompContext* ctx, uint32_t vaddr, uint32_t value, int size) {
  const uint32_t p = recomp_phys(vaddr);
  if (pci_handles(p)) {
    pci_write(p, value, size);
  } else if (ide_handles(p)) {
    ide_write(ctx, p, value, size);
  } else if (banshee_io_handles(p)) {
    banshee_io_write(p, value, size);
  } else if (nile_handles(p)) {
    nile_write(p, value, size);
  } else if (ioasic_handles(p)) {
    ioasic_write(p, value, size);
  } else if (sio_handles(p)) {
    sio_write(p, value, size);
  } else if (nvram_handles(p)) {
    nvram_write(p, value, size);
  } else if (p == VEGAS_LED) {
    g_vegas_led = (uint8_t)value; /* board diagnostic LED / 7-segment code */
  } else {
    vegas_log("mmio: unmapped write%d %08X <- %08X", size * 8, vaddr, value);
  }
  vegas_advance(4);
}
