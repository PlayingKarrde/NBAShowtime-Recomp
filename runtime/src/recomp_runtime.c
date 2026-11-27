// Core runtime pieces shared by every host build: indirect call lookup (per loaded program) and
// float -> integer conversions with defined behaviour.
#include <math.h>
#include <stdint.h>

#include "recomp.h"

#ifdef RECOMP_ENGINE_DYNAMIC
const RecompGameExports* recomp_game; /* the loaded game library (src/host/game_loader.c) */
#else
uint8_t* recomp_rdram;
#endif

#ifdef RECOMP_TRACE_CALLS
uint32_t recomp_call_ring[RECOMP_CALL_RING];
uint32_t recomp_call_pos;
uint32_t recomp_break_addr;
uint32_t recomp_watch_lo, recomp_watch_len;
#endif

static void missing_function(RecompContext* ctx) {
  recomp_unhandled(ctx, (uint32_t)ctx->r[31], "indirect call to unknown address");
}

static const RecompProgram* cur_program; /* NULL = the game (recomp_programs[0]) */

const RecompProgram* recomp_program(void) {
  return cur_program ? cur_program : (recomp_program_count ? &recomp_programs[0] : NULL);
}
void recomp_set_program(const RecompProgram* p) { cur_program = p; }

static uint32_t crc32_ram(uint32_t start, uint32_t end) {
  static uint32_t table[256];
  if (!table[1])
    for (uint32_t i = 0; i < 256; ++i) {
      uint32_t c = i;
      for (int k = 0; k < 8; ++k) c = (c >> 1) ^ (0xEDB88320u & (0u - (c & 1u)));
      table[i] = c;
    }
  uint32_t c = 0xFFFFFFFFu;
  for (uint32_t a = recomp_phys(start); a < recomp_phys(end) && a < RECOMP_RAM_SIZE; ++a)
    c = table[(c ^ recomp_rdram[a]) & 0xFF] ^ (c >> 8);
  return ~c;
}

const RecompProgram* recomp_identify_program(void) {
  for (unsigned i = 0; i < recomp_program_count; ++i) {
    const RecompProgram* p = &recomp_programs[i];
    if (crc32_ram(p->text_start, p->text_end) == p->text_crc) return p;
  }
  return NULL;
}

static RecompFunc find(const RecompFuncEntry* t, unsigned n, uint32_t vaddr) {
  unsigned lo = 0, hi = n;
  while (lo < hi) {
    unsigned mid = (lo + hi) / 2;
    if (t[mid].addr < vaddr) lo = mid + 1;
    else hi = mid;
  }
  return lo < n && t[lo].addr == vaddr ? t[lo].fn : NULL;
}

RecompFunc recomp_lookup(RecompContext* ctx, uint32_t vaddr) {
  /* The BIOS is in the main table; program addresses go to the program that is loaded. */
  RecompFunc fn = cur_program && vaddr >= RECOMP_PROGRAM_BASE
                      ? find(cur_program->funcs, cur_program->func_count, vaddr)
                      : find(recomp_func_table, recomp_func_count, vaddr);
  if (fn) return fn;
  recomp_unhandled(ctx, vaddr, "recomp_lookup: no function at address");
  return missing_function;
}

int recomp_bios_syscall(RecompContext* ctx, uint32_t code) {
  if (code >= RECOMP_BIOS_SYSCALL_COUNT) return 0;
  uint32_t handler = mem_r32(ctx, RECOMP_BIOS_SYSCALL_TABLE + code * 4);
  if (!handler) return 0;
  recomp_lookup(ctx, handler)(ctx);
  return 1;
}

static double round_mode(double v, int mode) {
  switch (mode) {
    case 1: return trunc(v);
    case 2: return ceil(v);
    case 3: return floor(v);
    default: return nearbyint(v); /* FCSR RM = 0 (nearest-even), set by crt0 */
  }
}

int32_t recomp_cvt_w(double v, int mode) {
  double r = round_mode(v, mode);
  if (r != r || r >= 2147483648.0 || r < -2147483648.0) return 0x7FFFFFFF;
  return (int32_t)r;
}

int64_t recomp_cvt_l(double v, int mode) {
  double r = round_mode(v, mode);
  if (r != r || r >= 9223372036854775808.0 || r < -9223372036854775808.0) return INT64_MAX;
  return (int64_t)r;
}
