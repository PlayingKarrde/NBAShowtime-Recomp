// Smoke test: run the recompiled game from its entry point on top of a MAME RAM
// snapshot, with stub hardware. Logs syscalls, MMIO and COP0 traffic so we can see
// how far the recompiled startup code gets.
//
// usage: smoke <entry_ram.bin> <entry_regs.json> [max_events]
#include <setjmp.h>
#include <signal.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include "recomp.h"
#include "recomp_funcs.h"

static jmp_buf g_abort;
static long g_events, g_max_events = 400;
static uint64_t g_count;          /* fake CP0 Count */
static long g_mmio_reads, g_mmio_writes, g_syscalls;
static volatile sig_atomic_t g_timed_out;

static void event(const char* f, ...) {
  va_list ap;
  va_start(ap, f);
  printf("[%5ld] ", g_events);
  vprintf(f, ap);
  printf("\n");
  va_end(ap);
  if (++g_events >= g_max_events) {
    printf("-- event limit reached --\n");
    longjmp(g_abort, 1);
  }
}

uint32_t recomp_mmio_read(RecompContext* ctx, uint32_t a, int size) {
  (void)ctx;
  g_mmio_reads++;
  event("mmio read%-2d %08X -> 0", size * 8, a);
  return 0;
}
void recomp_mmio_write(RecompContext* ctx, uint32_t a, uint32_t v, int size) {
  (void)ctx;
  g_mmio_writes++;
  event("mmio write%-2d %08X <- %08X", size * 8, a, v);
}
void recomp_syscall(RecompContext* ctx, uint32_t code, uint32_t pc) {
  g_syscalls++;
  event("syscall %3u @%08X  a0=%08X a1=%08X a2=%08X a3=%08X ra=%08X", code, pc, R32(4), R32(5),
        R32(6), R32(7), R32(31));
  if (code == 20 && R32(6) < 512) { /* looks like write(fd, buf, len): show the text */
    char buf[513];
    uint32_t n = R32(6), i;
    for (i = 0; i < n; ++i) {
      char c = (char)mem_r8(ctx, R32(5) + i);
      buf[i] = (c == '\n' || c == '\r') ? '|' : (c < 32 || c > 126) ? '.' : c;
    }
    buf[n] = 0;
    printf("        text: \"%s\"\n", buf);
    ctx->r[2] = SE32(n); /* bytes written */
    return;
  }
  ctx->r[2] = 0; /* v0 = 0 */
}
void recomp_break(RecompContext* ctx, uint32_t code, uint32_t pc) {
  event("BREAK %u @%08X ra=%08X", code, pc, R32(31));
  longjmp(g_abort, 2);
}
void recomp_trap(RecompContext* ctx, uint32_t pc) {
  event("TRAP @%08X ra=%08X", pc, R32(31));
  longjmp(g_abort, 2);
}
uint64_t recomp_cop0_read(RecompContext* ctx, int reg, uint32_t pc) {
  (void)ctx;
  if (reg == 9) { g_count += 125000; return SE32(g_count); } /* Count: +1 ms per read */
  event("cop0 read $%d @%08X", reg, pc);
  return reg == 12 ? 0x30011C01u : 0;
}
void recomp_cop0_write(RecompContext* ctx, int reg, uint64_t v, uint32_t pc) {
  (void)ctx;
  if (reg == 9) { g_count = (uint32_t)v; return; }
  event("cop0 write $%d <- %08X @%08X", reg, (uint32_t)v, pc);
}
void recomp_unhandled(RecompContext* ctx, uint32_t pc, const char* what) {
  event("UNHANDLED %s @%08X ra=%08X", what, pc, R32(31));
  longjmp(g_abort, 3);
}

static void on_alarm(int sig) {
  (void)sig;
  g_timed_out = 1;
  /* async-signal-safe enough for a test harness */
  const char msg[] = "-- timeout: generated code is spinning without touching hardware --\n";
  write(1, msg, sizeof msg - 1);
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
  if (argc < 3) {
    fprintf(stderr, "usage: %s <entry_ram.bin> <entry_regs.json> [max_events]\n", argv[0]);
    return 2;
  }
  if (argc > 3) g_max_events = atol(argv[3]);
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
  uint64_t fcsr = 0;
  if (json_u64(json, "CCR31", &fcsr)) ctx.fcsr = (uint32_t)fcsr;
  uint64_t count = 0;
  if (json_u64(json, "Count", &count)) g_count = count;

  printf("entry: sp=%08X ra=%08X  functions=%u\n", (uint32_t)ctx.r[29], (uint32_t)ctx.r[31], recomp_func_count);
  signal(SIGALRM, on_alarm);
  alarm(10);

  int rc = setjmp(g_abort);
  if (rc == 0) {
    func_80100000(&ctx);
    printf("-- game entry returned (v0=%08X) --\n", (uint32_t)ctx.r[2]);
  }
  printf("summary: %ld events, %ld syscalls, %ld mmio reads, %ld mmio writes, stop reason %d\n",
         g_events, g_syscalls, g_mmio_reads, g_mmio_writes, rc);
  return 0;
}
