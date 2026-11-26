// Core runtime pieces shared by every host build: indirect call lookup and
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

RecompFunc recomp_lookup(RecompContext* ctx, uint32_t vaddr) {
  unsigned lo = 0, hi = recomp_func_count;
  while (lo < hi) {
    unsigned mid = (lo + hi) / 2;
    if (recomp_func_table[mid].addr < vaddr) lo = mid + 1;
    else hi = mid;
  }
  if (lo < recomp_func_count && recomp_func_table[lo].addr == vaddr) return recomp_func_table[lo].fn;
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
