// DCS2 sound board model. See dcs2.h.
#include "dcs2.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "adsp2100.h"
#include "vegas.h"

Dcs2Stats g_dcs2_stats;

/* ADSP-2104 memory-mapped control registers (DM 0x3FE0 + index) */
enum {
  CR_S1_AUTOBUF = 0x0F, CR_S1_RFSDIV = 0x10, CR_S1_SCLKDIV = 0x11, CR_S1_CONTROL = 0x12,
  CR_TSCALE = 0x1B, CR_TCOUNT = 0x1C, CR_TPERIOD = 0x1D, CR_SYSCONTROL = 0x1F
};

#define AUDIO_RING (1 << 16) /* stereo frames */

static int g_log_io; /* debug: DCS_LOG_IO logs latch traffic */
static int g_trace_cmd = -1;
static uint64_t g_trace_after; /* DCS_TRACE_AFTER: only trigger after this DSP cycle */   /* debug: log instructions after the host writes this value */
static int g_trace_left;

static struct {
  int present;
  Adsp2100 cpu;
  uint8_t bootrom[0x8000];
  uint32_t ipm[0x200];      /* internal program RAM (boot page lands here) */
  uint16_t idm[0x200];      /* internal data RAM at DM 0x3800 */
  uint32_t pm_sram[0x3800]; /* external program SRAM, PM 0x0800-0x3FFF */
  uint16_t dm_sram[0x4000]; /* external data SRAM banks */
  uint16_t* dram;           /* 4 MB sound DRAM = 2M words */
  uint16_t sdrc[4];
  uint8_t sdrc_seed;
  uint16_t ctrl[32];

  /* host latches */
  uint16_t input_data, output_data, output_control;
  int input_full, output_full;
  int in_reset;

  /* timing */
  uint64_t main_synced;     /* virtual time (Count units) the DSP has been run up to */
  uint64_t dsp_cycles;      /* DSP cycles executed in total */
  /* timer */
  int timer_ignore;
  uint64_t timer_next;      /* DSP cycle of the next timer expiry */
  /* SPORT0 receive tick: 1 kHz while SYSCONTROL bit 12 is set (the sound program's timebase) */
  uint64_t sport0_next;
  uint64_t output_control_read; /* DSP cycle of the last output-control read */
  /* SPORT1 autobuffer transmit */
  uint64_t sport_next;      /* DSP cycle of the next transmitted word */
  uint32_t sport_period;    /* DSP cycles per word (fixed point 16.16) */
  uint64_t sport_acc;
  int rate;
  /* audio ring */
  int16_t audio[AUDIO_RING * 2];
  uint32_t audio_wr, audio_rd;
  int16_t pending_left;
  int have_left;
} d;

/* ---------------------------------------------------------------- SDRC decode */
#define SDRC_ROM_ST (d.sdrc[0] & 3)
#define SDRC_ROM_SZ ((d.sdrc[0] >> 4) & 1)
#define SDRC_ROM_MS ((d.sdrc[0] >> 5) & 1)
#define SDRC_ROM_PG ((d.sdrc[0] >> 7) & 7)
#define SDRC_SM_EN ((d.sdrc[0] >> 11) & 1)
#define SDRC_SM_BK ((d.sdrc[0] >> 12) & 1)
#define SDRC_SMODE ((d.sdrc[0] >> 13) & 7)
#define SDRC_DM_ST (d.sdrc[1] & 3)
#define SDRC_DM_PG (d.sdrc[2] & 0x7FF)

static int rom_window(uint16_t a, uint32_t* index) {
  if (!SDRC_ROM_MS || SDRC_ROM_ST == 3) return 0;
  const uint16_t base = SDRC_ROM_ST == 0 ? 0x0000 : SDRC_ROM_ST == 1 ? 0x3000 : 0x3400;
  const uint16_t size = (SDRC_ROM_SZ == 0 && SDRC_ROM_ST != 0) ? 4096 : 1024;
  if (a < base || a >= base + size) return 0;
  *index = (SDRC_ROM_PG * 4096u + (a - base)) % sizeof d.bootrom;
  return 1;
}

static uint16_t* dram_window(uint16_t a) {
  if (!SDRC_DM_ST) return NULL;
  const uint16_t base = SDRC_DM_ST == 1 ? 0x0000 : SDRC_DM_ST == 2 ? 0x3000 : 0x3400;
  if (a < base || a >= base + 0x400) return NULL;
  return &d.dram[(SDRC_DM_PG * 1024u + (a - base)) & 0x1FFFFF];
}

static uint16_t* sram_dm(uint16_t a) {
  if (!SDRC_SM_EN || a < 0x0800 || a >= 0x3800) return NULL;
  if (a < 0x1800) return SDRC_SM_BK ? NULL : &d.dm_sram[0x0000 + (a - 0x0800)];
  if (a < 0x2800) return &d.dm_sram[(SDRC_SM_BK ? 0x3000 : 0x1000) + (a - 0x1800)];
  return &d.dm_sram[0x2000 + (a - 0x2800)];
}

static uint16_t sdrc_read(int reg) {
  if (reg != 3) return d.sdrc[reg];
  switch (SDRC_SMODE) { /* security response sequence */
    case 1: return 0x5AA4;
    case 2: return (uint16_t)(0x5A00 | ((d.sdrc_seed & 0x3F) << 1));
    case 3: return 0x5AB9;
    case 4: return 0x5A03;
    case 5: return 0x5A69;
    case 6: return 0x5A20;
    case 7: return 0x5AFF;
    default: return 0x5A81;
  }
}

static void sdrc_write(int reg, uint16_t v) {
  if (reg < 3) { d.sdrc[reg] = v; return; }
  switch (SDRC_SMODE) {
    case 1: d.sdrc_seed = (uint8_t)v; break;
    case 3: d.sdrc_seed = (uint8_t)((d.sdrc_seed << 1) | 1); break;
    case 4: d.sdrc_seed = (uint8_t)(d.sdrc_seed + (d.sdrc_seed >> 1)); break;
    case 5: d.sdrc_seed = (uint8_t)(d.sdrc_seed ^ ((d.sdrc_seed << 1) | 1)); break;
    case 6: { /* LFSR step: shift right, new bit 7 = bit0 ^ bit2 ^ bit3 ^ bit4 */
      const uint8_t s = d.sdrc_seed;
      const uint8_t fb = (uint8_t)(((s >> 0) ^ (s >> 2) ^ (s >> 3) ^ (s >> 4)) & 1);
      d.sdrc_seed = (uint8_t)((fb << 7) | (s >> 1));
      break;
    }
    case 7: d.sdrc_seed = (uint8_t)~d.sdrc_seed; break;
    default: break;
  }
}

/* ---------------------------------------------------------------- boot */
static void boot(void) {
  const uint8_t* page = &d.bootrom[(SDRC_ROM_PG * 0x1000u) % sizeof d.bootrom];
  const int words = (page[3] + 1) * 8;
  for (int i = 0; i < words && i < 0x200; ++i)
    d.ipm[i] = ((uint32_t)page[i * 4] << 16) | ((uint32_t)page[i * 4 + 1] << 8) | page[i * 4 + 2];
}

static void sport_update(void);
static void timer_restart(void);

/* ---------------------------------------------------------------- control registers */
static void control_write(int reg, uint16_t v) {
  d.ctrl[reg] = v;
  switch (reg) {
    case CR_SYSCONTROL:
      if ((v & 0x1000) && !d.sport0_next) d.sport0_next = d.dsp_cycles + DCS2_CLOCK / 100000; /* first after 10 us */
      if (!(v & 0x1000)) d.sport0_next = 0;
      if (v & 0x0200) { /* software reboot: reload the boot page from the current ROM page */
        adsp_reset(&d.cpu);
        boot();
        d.ctrl[CR_SYSCONTROL] = 0;
        d.sport0_next = 0;
        g_dcs2_stats.booted_program = 1;
      }
      sport_update();
      break;
    case CR_S1_AUTOBUF:
    case CR_S1_CONTROL:
    case CR_S1_SCLKDIV:
      sport_update();
      break;
    case CR_TSCALE:
    case CR_TCOUNT:
    case CR_TPERIOD:
      timer_restart();
      break;
    default:
      break;
  }
}

/* ---------------------------------------------------------------- memory callbacks */
static uint16_t dm_read(void* u, uint16_t a) {
  (void)u;
  uint16_t* p;
  uint32_t ri;
  if ((p = dram_window(a))) return *p;
  if (rom_window(a, &ri)) return d.bootrom[ri];
  if ((p = sram_dm(a))) return *p;
  if (a >= 0x3FE0) return d.ctrl[a - 0x3FE0];
  if (a >= 0x3800 && a < 0x3A00) return d.idm[a - 0x3800];
  switch (a) {
    case 0x0400:
      if (g_log_io) printf("dcs2 @%llu: DSP reads input %04X (pc %04X)\n", (unsigned long long)d.dsp_cycles, d.input_data, d.cpu.pc);
      return d.input_data;
    case 0x0402: d.output_control_read = d.dsp_cycles; return d.output_control;
    case 0x0403: return (uint16_t)((d.input_full ? 0x80 : 0) | (d.output_full ? 0 : 0x40) | 0x08 /* FIFO empty */);
    case 0x0404: case 0x0405: case 0x0406: case 0x0407: return 0xFFFF; /* FIFO: not used */
    case 0x0480: case 0x0481: case 0x0482: case 0x0483: return sdrc_read(a - 0x0480);
    default: return 0xFFFF;
  }
}

static void dm_write(void* u, uint16_t a, uint16_t v) {
  (void)u;
  uint16_t* p;
  if ((p = dram_window(a))) { *p = v; return; }
  if ((p = sram_dm(a))) { *p = v; return; }
  if (a >= 0x3FE0) { control_write(a - 0x3FE0, v); return; }
  if (a >= 0x3800 && a < 0x3A00) { d.idm[a - 0x3800] = v; return; }
  switch (a) {
    case 0x0400: /* acknowledge the input latch */
      if (g_log_io) printf("dcs2 @%llu: DSP acks input (pc %04X)\n", (unsigned long long)d.dsp_cycles, d.cpu.pc);
      d.input_full = 0;
      adsp_set_irq_line(&d.cpu, ADSP_IRQ2, 0);
      break;
    case 0x0401:
      if (g_log_io) printf("dcs2 @%llu: DSP replies %04X (pc %04X)\n", (unsigned long long)d.dsp_cycles, v, d.cpu.pc);
      d.output_data = v;
      d.output_full = 1;
      g_dcs2_stats.output_writes++;
      if (d.cpu.trace_on && (v & 0xFF00) == 0xEE00) { /* monitor error code: show how we got here */
        printf("dcs2: DSP replied error %04X; last instructions:\n", v);
        for (int k = 64; k > 0; --k) {
          const uint32_t ix = (d.cpu.trace_pos - (uint32_t)k) & 255;
          printf("  %04X: %06X\n", d.cpu.trace_pc[ix], d.cpu.trace_op[ix]);
        }
        d.cpu.trace_on = 0; /* only the first one */
      }
      break;
    case 0x0402: d.output_control = v; break;
    case 0x0480: case 0x0481: case 0x0482: case 0x0483: sdrc_write(a - 0x0480, v); break;
    default: break;
  }
}

static uint32_t pm_read(void* u, uint16_t a) {
  (void)u;
  if (a < 0x0200) return d.ipm[a];
  if (a >= 0x0800 && SDRC_SM_EN) return d.pm_sram[a - 0x0800];
  return 0xFFFFFF;
}

static void pm_write(void* u, uint16_t a, uint32_t v) {
  (void)u;
  if (a < 0x0200) {
    if (g_log_io && a < 0x30 && d.ipm[a] != v) printf("dcs2 @%llu: PM %04X <- %06X (pc %04X)\n", (unsigned long long)d.dsp_cycles, a, v, d.cpu.pc);
    d.ipm[a] = v;
  }
  else if (a >= 0x0800 && SDRC_SM_EN) d.pm_sram[a - 0x0800] = v;
}

/* ---------------------------------------------------------------- timer */
static uint32_t timer_scale(void) { return (uint32_t)(d.ctrl[CR_TSCALE] & 0xFF) + 1; }

/* The DCS2 boot ROM's timer handler only pokes the SDRC page register (DRAM refresh); skip those
   interrupts when it is the installed handler. Re-checked whenever the timer is (re)programmed,
   because the game's sound program replaces the vectors with its own timer handler. */
static int refresh_only_timer_handler(void) {
  return d.ipm[0x18] == 0x0C0030 && d.ipm[0x19] == 0x804828 && d.ipm[0x1A] == 0x904828 &&
         d.ipm[0x1B] == 0x0C0020 && d.ipm[0x1C] == 0x0A001F;
}

static void timer_restart(void) {
  d.timer_ignore = refresh_only_timer_handler();
  d.timer_next = d.dsp_cycles + (uint64_t)timer_scale() * ((uint64_t)d.ctrl[CR_TCOUNT] + 1);
}

static void mstat_changed(void* u, uint16_t old_mstat, uint16_t new_mstat) {
  (void)u;
  if ((new_mstat & ADSP_MSTAT_TIMER) && !(old_mstat & ADSP_MSTAT_TIMER)) timer_restart();
}

/* ---------------------------------------------------------------- SPORT1 autobuffer */
static void sport_update(void) {
  const int enabled = (d.ctrl[CR_SYSCONTROL] & 0x0800) && (d.ctrl[CR_S1_AUTOBUF] & 0x0002);
  if (!enabled) { d.sport_period = 0; return; }
  /* frame rate: internal serial clock -> 16 MHz / (2 * (SCLKDIV + 1)) / (16 bits * 2 channels) */
  double frame_rate = 31250.0;
  if (d.ctrl[CR_S1_CONTROL] & 0x4000)
    frame_rate = (double)DCS2_CLOCK / (2.0 * (d.ctrl[CR_S1_SCLKDIV] + 1)) / 32.0;
  d.rate = (int)(frame_rate + 0.5);
  const double word_period = (double)DCS2_CLOCK / (frame_rate * 2.0);
  const uint32_t period = (uint32_t)(word_period * 65536.0);
  if (period != d.sport_period) {
    d.sport_period = period;
    d.sport_acc = (uint64_t)d.dsp_cycles << 16;
  }
}

static void audio_push(int16_t s) {
  if (!d.have_left) { d.pending_left = s; d.have_left = 1; return; }
  d.have_left = 0;
  const uint32_t w = d.audio_wr % AUDIO_RING;
  d.audio[w * 2] = d.pending_left;
  d.audio[w * 2 + 1] = s;
  d.audio_wr++;
  if (d.audio_wr - d.audio_rd > AUDIO_RING) d.audio_rd = d.audio_wr - AUDIO_RING; /* drop oldest */
}

/* Transmit one autobuffered word: read DM[I], post-modify with M and circular L. */
static void sport_word(void) {
  const uint16_t ab = d.ctrl[CR_S1_AUTOBUF];
  const int ireg = (ab >> 9) & 7;
  const int mreg = ((ab >> 7) & 3) | (ireg & 4);
  Adsp2100* c = &d.cpu;
  const uint16_t addr = c->i[ireg];
  audio_push((int16_t)dm_read(NULL, addr));
  g_dcs2_stats.samples++;
  const int32_t m = (c->m[mreg] & 0x2000) ? (int32_t)(c->m[mreg] | 0xFFFFC000u) : (int32_t)c->m[mreg];
  int32_t next = addr + m;
  const uint16_t len = c->l[ireg];
  int wrapped = 0;
  if (len) {
    uint32_t span = 1;
    while (span < len) span <<= 1;
    const int32_t base = addr & ~(int32_t)(span - 1);
    if (next >= base + len) { next -= len; wrapped = 1; }
    else if (next < base) { next += len; wrapped = 1; }
  }
  c->i[ireg] = (uint16_t)(next & 0x3FFF);
  if (wrapped) { adsp_pulse_irq(c, ADSP_IRQ1); g_dcs2_stats.irq1++; } /* buffer complete */
}

/* ---------------------------------------------------------------- run */
static void run_cycles(uint64_t target) {
  while (d.dsp_cycles < target) {
    uint64_t until = target;
    const int timer_on = (d.cpu.mstat & ADSP_MSTAT_TIMER) && !d.timer_ignore;
    if (timer_on && d.timer_next < until) until = d.timer_next;
    if (d.sport0_next && d.sport0_next < until) until = d.sport0_next;
    uint64_t next_word = 0;
    if (d.sport_period) {
      next_word = (d.sport_acc + d.sport_period) >> 16;
      if (next_word < until) until = next_word;
    }
    if (until > d.dsp_cycles) {
      const int n = (int)(until - d.dsp_cycles);
      if (!d.in_reset && g_trace_left) { /* debug: single-step with logging */
        for (int k = 0; k < n && g_trace_left; ++k, --g_trace_left) {
          const uint32_t op = pm_read(NULL, d.cpu.pc);
          printf("dsp %04X: %06X  AX0=%04X AR=%04X imask=%02X icntl=%02X l2=%d/%d full=%d\n", d.cpu.pc, op, d.cpu.r[0], d.cpu.r[10], d.cpu.imask, d.cpu.icntl, d.cpu.irq_latch[0], d.cpu.irq_line[0], d.input_full);
          adsp_run(&d.cpu, 1);
          d.dsp_cycles++;
          g_dcs2_stats.instructions++;
        }
        continue;
      }
      if (!d.in_reset) {
        const uint64_t before = d.cpu.cycles;
        adsp_run(&d.cpu, n);
        g_dcs2_stats.instructions += d.cpu.cycles - before;
      }
      d.dsp_cycles = until;
    }
    if (timer_on && d.dsp_cycles >= d.timer_next) {
      adsp_pulse_irq(&d.cpu, ADSP_TIMER);
      g_dcs2_stats.timer_irqs++;
      d.timer_next += (uint64_t)timer_scale() * ((uint64_t)d.ctrl[CR_TPERIOD] + 1);
    }
    if (d.sport0_next && d.dsp_cycles >= d.sport0_next) {
      d.sport0_next += DCS2_CLOCK / 1000;
      if (!d.in_reset && d.dsp_cycles - d.output_control_read > 5) adsp_pulse_irq(&d.cpu, ADSP_SPORT0_RX);
    }
    if (d.sport_period && d.dsp_cycles >= next_word) {
      d.sport_acc += d.sport_period;
      if (!d.in_reset) sport_word();
    }
  }
}

void dcs2_sync(uint64_t now) {
  if (!d.present || now <= d.main_synced) return;
  d.main_synced = now;
  /* Virtual time is in 125 MHz Count units and the DSP runs at 16 MHz. Convert the absolute time
     (not the increment): hardware accesses advance time by a few units at a time, and truncating
     each increment would starve the DSP. */
  const uint64_t target = (now / 125u) * 16u + ((now % 125u) * 16u) / 125u;
  if (target > d.dsp_cycles) run_cycles(target);
}

/* ---------------------------------------------------------------- host interface */
static void board_reset(void) {
  memset(d.sdrc, 0, sizeof d.sdrc);
  memset(d.ctrl, 0, sizeof d.ctrl);
  d.input_full = d.output_full = 0;
  d.timer_ignore = 0;
  d.sport_period = 0;
  d.sport0_next = 0;
  adsp_reset(&d.cpu);
  boot();
}

void dcs2_reset_line(int run, uint64_t now) {
  if (!d.present) return;
  dcs2_sync(now);
  if (!run) {
    d.in_reset = 1;
  } else if (d.in_reset) {
    d.in_reset = 0;
    board_reset();
    g_dcs2_stats.resets++;
  }
}

void dcs2_data_w(uint16_t v, uint64_t now) {
  if (!d.present) return;
  dcs2_sync(now);
  if (g_log_io) printf("dcs2 @%llu: host writes %04X (full %d, pc %04X imask %02X latch %d)\n", (unsigned long long)d.dsp_cycles, v, d.input_full, d.cpu.pc, d.cpu.imask, d.cpu.irq_latch[0]);
  d.input_data = v;
  d.input_full = 1;
  if (g_trace_cmd >= 0 && v == (uint16_t)g_trace_cmd && !g_trace_left && d.dsp_cycles >= g_trace_after) g_trace_left = 20000;
  adsp_set_irq_line(&d.cpu, ADSP_IRQ2, 1);
  g_dcs2_stats.host_writes++;
}

uint16_t dcs2_data_r(uint64_t now) {
  if (!d.present) return 0;
  dcs2_sync(now);
  g_dcs2_stats.host_reads++;
  if (g_log_io) printf("dcs2 @%llu: host reads reply %04X\n", (unsigned long long)d.dsp_cycles, d.output_data);
  return d.output_data;
}

void dcs2_ack_w(uint64_t now) {
  if (!d.present) return;
  dcs2_sync(now);
  d.output_full = 0;
}

uint16_t dcs2_status(uint64_t now) {
  if (!d.present) return 0;
  dcs2_sync(now);
  return (uint16_t)((d.input_full ? 0 : 0x80) | (d.output_full ? 0x40 : 0) | (d.output_control & 0xFF00));
}

int dcs2_present(void) { return d.present; }

void dcs2_debug_dump(void) {
  if (!d.present) return;
  const Adsp2100* c = &d.cpu;
  printf("dcs2: pc=%04X imask=%02X icntl=%02X mstat=%02X idle=%d reset=%d in_full=%d out_full=%d "
         "sysctrl=%04X autobuf=%04X sdrc=%04X/%04X/%04X loop_sp=%d pc_sp=%d irq2_line=%d irq_latch=%d%d%d%d%d%d\n",
         c->pc, c->imask, c->icntl, c->mstat, c->idle, d.in_reset, d.input_full, d.output_full,
         d.ctrl[CR_SYSCONTROL], d.ctrl[CR_S1_AUTOBUF], d.sdrc[0], d.sdrc[1], d.sdrc[2], c->loop_sp, c->pc_sp,
         c->irq_line[ADSP_IRQ2], c->irq_latch[0], c->irq_latch[1], c->irq_latch[2], c->irq_latch[3],
         c->irq_latch[4], c->irq_latch[5]);
  /* a short window of execution */
  Adsp2100 copy = d.cpu;
  (void)copy;
  printf("dcs2: timer irqs=%llu ignore=%d tscale=%04X tcount=%04X tperiod=%04X next=%llu now=%llu; "
         "irq1=%llu sport_period=%u host_writes=%llu outputs=%llu\n",
         (unsigned long long)g_dcs2_stats.timer_irqs, d.timer_ignore, d.ctrl[CR_TSCALE], d.ctrl[CR_TCOUNT],
         d.ctrl[CR_TPERIOD], (unsigned long long)d.timer_next, (unsigned long long)d.dsp_cycles,
         (unsigned long long)g_dcs2_stats.irq1, d.sport_period, (unsigned long long)g_dcs2_stats.host_writes,
         (unsigned long long)g_dcs2_stats.output_writes);
  printf("dcs2: vectors 0018..001C: %06X %06X %06X %06X %06X\n", d.ipm[0x18], d.ipm[0x19], d.ipm[0x1A], d.ipm[0x1B], d.ipm[0x1C]);
  if (getenv("DCS_DUMP_PM")) { /* program memory image for offline inspection */
    FILE* f = fopen(getenv("DCS_DUMP_PM"), "wb");
    if (f) {
      for (uint32_t a = 0; a < 0x4000; ++a) { const uint32_t w = pm_read(NULL, (uint16_t)a); fwrite(&w, 4, 1, f); }
      fwrite(d.dm_sram, 2, 0x4000, f);
      fwrite(d.idm, 2, 0x200, f);
      fclose(f);
    }
  }
  printf("dcs2: next instructions:");
  uint16_t pc = c->pc;
  for (int k = 0; k < 12; ++k) printf(" %04X:%06X", (uint16_t)(pc + k), pm_read(NULL, (uint16_t)(pc + k)));
  printf("\n");
}
int dcs2_audio_rate(void) { return d.rate ? d.rate : 31250; }

int dcs2_audio_read(int16_t* out, int max_frames) {
  int n = 0;
  while (n < max_frames && d.audio_rd != d.audio_wr) {
    const uint32_t r = d.audio_rd % AUDIO_RING;
    out[n * 2] = d.audio[r * 2];
    out[n * 2 + 1] = d.audio[r * 2 + 1];
    d.audio_rd++;
    ++n;
  }
  return n;
}

int dcs2_init(const char* bootrom_path) {
  memset(&g_dcs2_stats, 0, sizeof g_dcs2_stats);
  free(d.dram);
  memset(&d, 0, sizeof d);
  FILE* f = bootrom_path ? fopen(bootrom_path, "rb") : NULL;
  if (!f) {
    vegas_log("dcs2: no boot ROM (%s); sound board not emulated", bootrom_path ? bootrom_path : "none");
    return -1;
  }
  const size_t n = fread(d.bootrom, 1, sizeof d.bootrom, f);
  fclose(f);
  if (n != sizeof d.bootrom) { vegas_log("dcs2: short boot ROM"); return -1; }
  d.dram = calloc(0x200000, sizeof(uint16_t));
  d.cpu.dm_read = dm_read;
  d.cpu.dm_write = dm_write;
  d.cpu.pm_read = pm_read;
  d.cpu.pm_write = pm_write;
  d.cpu.mstat_changed = mstat_changed;
  d.cpu.trace_on = getenv("DCS_TRACE") != NULL;
  g_log_io = getenv("DCS_LOG_IO") != NULL;
  if (getenv("DCS_TRACE_AFTER")) g_trace_after = strtoull(getenv("DCS_TRACE_AFTER"), NULL, 0);
  if (getenv("DCS_TRACE_ON_CMD")) g_trace_cmd = (int)strtol(getenv("DCS_TRACE_ON_CMD"), NULL, 16);
  d.present = 1;
  d.in_reset = 1; /* held in reset until the host releases it */
  board_reset();
  return 0;
}
