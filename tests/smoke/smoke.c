// Smoke test: run the recompiled BIOS + game from the game's entry point on top of a
// MAME RAM snapshot, with the runtime's Vegas machine model (CP0, interrupts, IDE).
// Logs syscalls and unmapped hardware accesses so we can see how far boot gets.
//
// usage: smoke <entry_ram.bin> <entry_regs.json> <disk.img> [max_events] [--quiet]
//   --quiet: don't log every syscall (only text written by the game, and hardware events)
#include <setjmp.h>
#include <signal.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include "../../runtime/src/ide.h"
#include "recomp.h"
#include "recomp_funcs.h"
#include "vegas.h"

static jmp_buf g_abort;
static long g_events, g_max_events = 400;
static long g_syscalls;
static int g_trace_syscalls = 1;

static void summary(const char* why) {
  char b[512];
  int n = snprintf(b, sizeof b,
                   "%s\nsummary: %ld events, %ld syscalls, %llu interrupts delivered, IDE: %llu cmds, "
                   "%llu DMA, %llu sectors read, %llu written, virtual time %.3f s\n",
                   why, g_events, g_syscalls, (unsigned long long)g_vegas.interrupts_delivered,
                   (unsigned long long)g_ide_stats.commands, (unsigned long long)g_ide_stats.dma_transfers,
                   (unsigned long long)g_ide_stats.sectors_read,
                   (unsigned long long)g_ide_stats.sectors_written, g_vegas.cop0.cycles / 125e6);
  if (write(1, b, (size_t)n) < 0) { /* nothing to do */ }
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
  if (!recomp_bios_syscall(ctx, code)) event("syscall %u has no BIOS handler", code);
  if (g_trace_syscalls) printf("        -> v0=%08X\n", R32(2));
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
  summary("-- timeout (10 s wall clock) --");
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
  /* VRC5074 register dump lives next to the register JSON (entry_nile.bin). */
  char nile_path[1024];
  snprintf(nile_path, sizeof nile_path, "%s", argv[2]);
  char* slash = strrchr(nile_path, '/');
  snprintf(slash ? slash + 1 : nile_path, sizeof nile_path - (size_t)(slash ? slash + 1 - nile_path : 0),
           "entry_nile.bin");
  if (vegas_init(argv[3], nile_path) != 0) return 1;

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
  alarm(10);

  int rc = setjmp(g_abort);
  if (rc == 0) {
    func_80100000(&ctx);
    printf("-- game entry returned (v0=%08X) --\n", (uint32_t)ctx.r[2]);
  }
  summary("-- stopped --");
  return 0;
}
