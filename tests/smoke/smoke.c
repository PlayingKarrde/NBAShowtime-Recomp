// Smoke test: run the recompiled BIOS + game from the game's entry point on top of a
// MAME RAM snapshot, with the runtime's Vegas machine model (CP0, interrupts, IDE).
// Logs syscalls and unmapped hardware accesses so we can see how far boot gets.
//
// usage: smoke <entry_ram.bin> <entry_regs.json> <disk.img> [max_events] [--quiet]
//   --quiet: don't log every syscall (only text written by the game, and hardware events)
#include <execinfo.h>
#include <setjmp.h>
#include <signal.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include "../../runtime/src/banshee.h"
#include "../../runtime/src/dcs2.h"
#include "../../runtime/src/ide.h"
#include "../../runtime/src/voodoo3d.h"
#include "../../runtime/src/raster_sw.h"
#include "../../runtime/src/ioasic.h"
#include "recomp.h"
#include "recomp_funcs.h"
#include "vegas.h"
#include "../../runtime/src/nile.h"

static jmp_buf g_abort;
extern uint64_t recomp_proc_switches, recomp_proc_created;
static long g_events, g_max_events = 400;
static long g_syscalls;
static int g_trace_syscalls = 1;

static void dump_call_trace(int n) {
#ifdef RECOMP_TRACE_CALLS
  printf("last %d function entries (oldest first):\n", n);
  for (int i = n; i > 0; --i) {
    printf(" %08X", recomp_call_ring[(recomp_call_pos - (uint32_t)i) & (RECOMP_CALL_RING - 1)]);
    if (i % 10 == 1) printf("\n");
  }
#else
  (void)n;
#endif
}

/* Text still queued in the BIOS console ring buffer (0x8002BF08: +4 write ptr, +8 read ptr,
   data at +0xC..+0x10C). Error handlers print with interrupts off, so it never drains. */
static void dump_console_buffer(void) {
  const uint32_t base = 0x8002BF08u, data = base + 0xC, end = base + 0x10C;
  uint32_t wr, rd;
  memcpy(&wr, recomp_rdram + (base + 4 - 0x80000000u), 4);
  memcpy(&rd, recomp_rdram + (base + 8 - 0x80000000u), 4);
  if (wr == rd || wr < data || wr >= end || rd < data || rd >= end) return;
  char text[300];
  int n = 0;
  for (uint32_t p = rd; p != wr && n < (int)sizeof text - 1; p = (p + 1 == end) ? data : p + 1) {
    const char c = (char)recomp_rdram[p - 0x80000000u];
    text[n++] = (c == '\n' || c == '\r') ? '|' : (c < 32 || c > 126) ? '.' : c;
  }
  text[n] = 0;
  printf("undrained console text: \"%s\"\n", text);
}

#ifdef RECOMP_TRACE_CALLS
static void summary(const char* why);
/* WATCH_VERT=<virtual seconds>: at the first Glide vertex draw after that time, watch the first
   vertex's X/Y and report the guest functions that write them. */
void recomp_watch_hit(RecompContext* ctx, uint32_t addr, uint32_t value) {
  (void)ctx;
  static int hits;
  float f;
  memcpy(&f, &value, 4);
  printf("watch %08X <- %08X (%g):", addr, value, f);
  for (int k = 0; k < 6; ++k) printf(" %08X", recomp_call_ring[(recomp_call_pos - 1 - (uint32_t)k) & (RECOMP_CALL_RING - 1)]);
  printf("\n");
  if (++hits >= 40) { recomp_watch_len = 0; summary("-- watch done --"); _exit(0); }
}
void recomp_break_hit(RecompContext* ctx, uint32_t addr) {
  if (getenv("WATCH_VERT")) {
    if (g_vegas.cop0.cycles / 125e6 < atof(getenv("WATCH_VERT")) || recomp_watch_len) return;
    const uint32_t vp = mem_r32(ctx, R32(5)); /* a1 = list of vertex pointers */
    recomp_watch_lo = vp & 0x1FFFFFFFu;
    recomp_watch_len = 8;
    printf("watching vertex at %08X (x %08X)\n", vp, mem_r32(ctx, vp));
    return;
  }
  dcs2_debug_dump();
  printf("-- break at %08X --\n", addr);
  dump_call_trace(RECOMP_CALL_RING - 1);
  printf("-- stopped at break --\n");
  exit(6);
}
#endif

extern uint64_t g_fpe_count;
static int g_frames_dumped;
static void summary(const char* why);
static void dump_frame(void* user, uint64_t frame, const uint8_t* rgba, int w, int h) {
  (void)user;
  char path[512];
  snprintf(path, sizeof path, "%s/frame_%06llu.png", getenv("FRAME_DUMP"), (unsigned long long)frame);
  raster_sw_write_png(path, rgba, w, h);
  extern uint64_t g_sw_stats[8];
  fprintf(stderr, "[frame] %s (virtual %.2f s) yorigin %u px in %llu chroma %llu amask %llu atest %llu depth %llu written %llu off %llu\n", path, g_vegas.cop0.cycles / 125e6,
          (banshee_io_reg(0x10) >> 18) & 0xFFF, (unsigned long long)g_sw_stats[0], (unsigned long long)g_sw_stats[1], (unsigned long long)g_sw_stats[2],
          (unsigned long long)g_sw_stats[3], (unsigned long long)g_sw_stats[4], (unsigned long long)g_sw_stats[5], (unsigned long long)g_sw_stats[6]);
  memset(g_sw_stats, 0, sizeof g_sw_stats);
  if (getenv("FRAME_MAX") && ++g_frames_dumped >= atoi(getenv("FRAME_MAX"))) { summary("-- frame limit --"); _exit(0); }
}
static void summary(const char* why) {
  g_max_events = 1L << 40; /* don't longjmp out of the summary */
  nile_uart_flush();
  dump_console_buffer();
  char b[512];
  int n = snprintf(b, sizeof b,
                   "%s\nsummary: %ld events, %ld syscalls, %llu interrupts delivered, IDE: %llu cmds, "
                   "%llu DMA, %llu sectors read, %llu written, virtual time %.3f s\n"
                   "sound board: %u words, %u uploads (%u PM + %u DM words), program %s\n"
                   "banshee: FIFO %s at %06X+%X, %llu FIFO words, %llu 3D reg writes, %llu VRAM writes, %llu status reads\n",
                   why, g_events, g_syscalls, (unsigned long long)g_vegas.interrupts_delivered,
                   (unsigned long long)g_ide_stats.commands, (unsigned long long)g_ide_stats.dma_transfers,
                   (unsigned long long)g_ide_stats.sectors_read,
                   (unsigned long long)g_ide_stats.sectors_written, g_vegas.cop0.cycles / 125e6,
                   g_dcs.words_received, g_dcs.uploads, g_dcs.pm_words, g_dcs.dm_words,
                   g_dcs.running ? "running" : "not started",
                   g_banshee_stats.fifo_enabled ? "on" : "off", g_banshee_stats.fifo_base,
                   g_banshee_stats.fifo_size, (unsigned long long)g_banshee_stats.fifo_words,
                   (unsigned long long)g_banshee_stats.reg3d_writes,
                   (unsigned long long)g_banshee_stats.vram_writes,
                   (unsigned long long)g_banshee_stats.status_reads);
  if (write(1, b, (size_t)n) < 0) { /* nothing to do */ }
  n = snprintf(b, sizeof b, "interrupts by line: IP2 %llu, IP3 %llu, IP4 %llu, IP5 %llu, IP6 %llu, IP7 %llu; "
               "processes: %llu created, %llu switches\n",
               (unsigned long long)g_vegas.irq_count[2], (unsigned long long)g_vegas.irq_count[3],
               (unsigned long long)g_vegas.irq_count[4], (unsigned long long)g_vegas.irq_count[5],
               (unsigned long long)g_vegas.irq_count[6], (unsigned long long)g_vegas.irq_count[7],
               (unsigned long long)recomp_proc_created, (unsigned long long)recomp_proc_switches);
  if (write(1, b, (size_t)n) < 0) { /* nothing to do */ }
  {
    n = snprintf(b, sizeof b, "3D: %llu triangles (%llu culled), %llu fast fills, %llu swaps, %llu texture words, "
                 "%llu 3D LFB words; packets 0-5: %llu %llu %llu %llu %llu %llu\n",
                 (unsigned long long)g_v3d_stats.triangles, (unsigned long long)g_v3d_stats.culled,
                 (unsigned long long)g_v3d_stats.fastfills, (unsigned long long)g_v3d_stats.swaps,
                 (unsigned long long)g_v3d_stats.texture_words, (unsigned long long)g_v3d_stats.lfb_words,
                 (unsigned long long)g_v3d_stats.packets[0], (unsigned long long)g_v3d_stats.packets[1],
                 (unsigned long long)g_v3d_stats.packets[2], (unsigned long long)g_v3d_stats.packets[3],
                 (unsigned long long)g_v3d_stats.packets[4], (unsigned long long)g_v3d_stats.packets[5]);
    if (write(1, b, (size_t)n) < 0) { /* nothing to do */ }
    if (getenv("V3D_CALLERS")) { extern void v3d_debug_callers(void); v3d_debug_callers(); }
    if (getenv("V3D_STATS")) {
      for (int f = 0; f < 16; ++f)
        if (g_v3d_stats.tex_format_tris[f]) printf("  textured tris, format %d: %llu\n", f, (unsigned long long)g_v3d_stats.tex_format_tris[f]);
      for (int r = 0; r < V3D_REG_COUNT; ++r)
        if (g_v3d_stats.reg_writes[r]) printf("  reg %03X: %llu writes, last %08X\n", r * 4, (unsigned long long)g_v3d_stats.reg_writes[r], v3d_reg_read((uint32_t)r));
      fflush(stdout);
    }
  }
  if (dcs2_present()) {
    n = snprintf(b, sizeof b, "DCS2 DSP: %llu instructions, %llu host words in, %llu replies, program booted: %s, "
                 "%llu audio words; FP exceptions taken: %llu\n",
                 (unsigned long long)g_dcs2_stats.instructions, (unsigned long long)g_dcs2_stats.host_writes,
                 (unsigned long long)g_dcs2_stats.output_writes, g_dcs2_stats.booted_program ? "yes" : "no",
                 (unsigned long long)g_dcs2_stats.samples, (unsigned long long)g_fpe_count);
    if (write(1, b, (size_t)n) < 0) { /* nothing to do */ }
  }
}

static void vevent(const char* f, va_list ap) {
  printf("[%5ld] ", g_events);
  vprintf(f, ap);
  printf("\n");
  if (++g_events >= g_max_events) {
    printf("-- event limit reached --\n");
    longjmp(g_abort, 1);
  }
}
static void event(const char* f, ...) {
  va_list ap;
  va_start(ap, f);
  vevent(f, ap);
  va_end(ap);
}

void recomp_syscall(RecompContext* ctx, uint32_t code, uint32_t pc) {
  g_syscalls++;
  if (g_trace_syscalls)
    event("syscall %3u @%08X  a0=%08X a1=%08X a2=%08X a3=%08X ra=%08X", code, pc, R32(4), R32(5),
          R32(6), R32(7), R32(31));
  if (code == 13 && getenv("TRACE_FILES")) { /* open(path, flags) */
    char path[128];
    int i = 0;
    for (; i < 127; ++i) { path[i] = (char)mem_r8(ctx, R32(4) + (uint32_t)i); if (!path[i]) break; }
    path[i] = 0;
    printf("        open(\"%s\", %u)\n", path, R32(5));
  }
  if (code == 20 && R32(6) < 512) { /* write(fd, buf, len): show the text */
    char buf[513];
    uint32_t n = R32(6), i;
    for (i = 0; i < n; ++i) {
      char c = (char)mem_r8(ctx, R32(5) + i);
      buf[i] = (c == '\n' || c == '\r') ? '|' : (c < 32 || c > 126) ? '.' : c;
    }
    buf[n] = 0;
    printf("        text(fd %u): \"%s\"\n", R32(4), buf);
  }
  if (code == 23) { /* exit: show how we got here, then stop (the BIOS would loop forever) */
    dump_call_trace(400);
    event("game called exit(%d)", (int)R32(4));
    longjmp(g_abort, 5);
  }
  if (!recomp_bios_syscall(ctx, code)) event("syscall %u has no BIOS handler", code);
  if (g_trace_syscalls) printf("        -> v0=%08X\n", R32(2));
  if (getenv("TRACE_FILES") && (code == 13 || code == 14 || code == 0 || code == 24))
    printf("        file syscall %u -> v0=%08X\n", code, R32(2));
  vegas_poll_interrupts(ctx);
}
void recomp_break(RecompContext* ctx, uint32_t code, uint32_t pc) {
  event("BREAK %u @%08X ra=%08X", code, pc, R32(31));
  longjmp(g_abort, 2);
}
void recomp_trap(RecompContext* ctx, uint32_t pc) {
  event("TRAP @%08X ra=%08X", pc, R32(31));
  longjmp(g_abort, 2);
}
void recomp_unhandled(RecompContext* ctx, uint32_t pc, const char* what) {
  event("UNHANDLED %s @%08X ra=%08X", what, pc, R32(31));
  longjmp(g_abort, 3);
}

static void on_alarm(int sig) {
  (void)sig;
  /* Host backtrace: the generated func_XXXXXXXX frames show where the guest is spinning. */
  void* frames[48];
  int n = backtrace(frames, 48);
  backtrace_symbols_fd(frames, n, 1);
  summary("-- timeout (wall clock) --");
  _exit(4);
}

static const char* gpr_names[32] = {"zero", "at", "v0", "v1", "a0", "a1", "a2", "a3",
                                    "t0",   "t1", "t2", "t3", "t4", "t5", "t6", "t7",
                                    "s0",   "s1", "s2", "s3", "s4", "s5", "s6", "s7",
                                    "t8",   "t9", "k0", "k1", "gp", "sp", "fp", "ra"};

static int json_u64(const char* json, const char* key, uint64_t* out) {
  char pat[64];
  snprintf(pat, sizeof pat, "\"%s\": \"", key);
  const char* p = strstr(json, pat);
  if (!p) return 0;
  *out = strtoull(p + strlen(pat), NULL, 16);
  return 1;
}

int main(int argc, char** argv) {
  if (argc < 4) {
    fprintf(stderr, "usage: %s <entry_ram.bin> <entry_regs.json> <disk.img> [max_events] [--quiet]\n",
            argv[0]);
    return 2;
  }
  for (int i = 4; i < argc; ++i) {
    if (!strcmp(argv[i], "--quiet")) g_trace_syscalls = 0;
    else g_max_events = atol(argv[i]);
  }
  setvbuf(stdout, NULL, _IOLBF, 0);

  recomp_rdram = calloc(1, RECOMP_RAM_SIZE);
  FILE* f = fopen(argv[1], "rb");
  if (!f || fread(recomp_rdram, 1, RECOMP_RAM_SIZE, f) != RECOMP_RAM_SIZE) {
    fprintf(stderr, "cannot read RAM image %s\n", argv[1]);
    return 1;
  }
  fclose(f);

  static char json[65536];
  f = fopen(argv[2], "rb");
  if (!f) { fprintf(stderr, "cannot read %s\n", argv[2]); return 1; }
  json[fread(json, 1, sizeof json - 1, f)] = 0;
  fclose(f);

  vegas_log = event;
#ifdef RECOMP_TRACE_CALLS
  if (getenv("BREAK_AT")) recomp_break_addr = (uint32_t)strtoul(getenv("BREAK_AT"), NULL, 16);
#endif
  extern int ide_trace, ide_discard_writes;
  ide_discard_writes = getenv("IDE_DISCARD_WRITES") != NULL;
  ide_trace = getenv("TRACE_IDE") != NULL;
  extern int ioasic_trace;
  ioasic_trace = getenv("TRACE_IOASIC") != NULL;
  /* VRC5074 and SIO register dumps live next to the register JSON. */
  char dir[1024], nile_path[1100], sio_path[1100];
  snprintf(dir, sizeof dir, "%s", argv[2]);
  char* slash = strrchr(dir, '/');
  if (slash) slash[1] = 0; else dir[0] = 0;
  snprintf(nile_path, sizeof nile_path, "%sentry_nile.bin", dir);
  snprintf(sio_path, sizeof sio_path, "%sentry_sio.bin", dir);
  const char* dcs_rom = getenv("DCS_ROM") ? getenv("DCS_ROM") : "work/bios/vegassio.bin";
  if (getenv("NO_DSP")) dcs_rom = NULL;
  if (vegas_init(argv[3], nile_path, sio_path, dcs_rom) != 0) return 1;
  if (getenv("FRAME_DUMP")) {
    /* FRAME_DUMP=dir [FRAME_START=n] [FRAME_EVERY=n] [FRAME_SCALE=s] [FRAME_MAX=n] */
    raster_sw_init(getenv("FRAME_SCALE") ? atoi(getenv("FRAME_SCALE")) : 1,
                   getenv("FRAME_START") ? strtoull(getenv("FRAME_START"), NULL, 0) : 0,
                   getenv("FRAME_EVERY") ? strtoull(getenv("FRAME_EVERY"), NULL, 0) : 60, dump_frame, NULL);
  }

  RecompContext ctx;
  memset(&ctx, 0, sizeof ctx);
  for (int i = 1; i < 32; ++i) json_u64(json, gpr_names[i], &ctx.r[i]);
  json_u64(json, "HI", &ctx.hi);
  json_u64(json, "LO", &ctx.lo);
  for (int i = 0; i < 32; ++i) {
    char k[16];
    uint64_t v = 0;
    snprintf(k, sizeof k, "FPR%d", i);
    if (json_u64(json, k, &v)) ctx.f[i] = (uint32_t)v;
  }
  uint64_t v = 0;
  if (json_u64(json, "CCR31", &v)) ctx.fcsr = (uint32_t)v;
  if (json_u64(json, "SR", &v)) g_vegas.cop0.sr = (uint32_t)v;
  if (json_u64(json, "Count", &v)) g_vegas.cop0.count_base = (uint32_t)v;
  if (json_u64(json, "Compare", &v)) g_vegas.cop0.compare = (uint32_t)v;

  printf("entry: sp=%08X ra=%08X sr=%08X functions=%u\n", (uint32_t)ctx.r[29], (uint32_t)ctx.r[31],
         g_vegas.cop0.sr, recomp_func_count);
  signal(SIGALRM, on_alarm);
  alarm(getenv("SMOKE_SECONDS") ? (unsigned)atoi(getenv("SMOKE_SECONDS")) : 10);

  int rc = setjmp(g_abort);
  if (rc == 0) {
    func_80100000(&ctx);
    printf("-- game entry returned (v0=%08X) --\n", (uint32_t)ctx.r[2]);
  }
  summary("-- stopped --");
  return 0;
}
