// Interface between recompiled game code (generated/*.c) and the native runtime.
//
// Guest: MIPS R5000, little-endian, 32-bit kernel addresses (kseg0/kseg1), FPU in FR=0
// mode (16 double registers made of even/odd 32-bit pairs).
#ifndef NBA_RECOMP_H
#define NBA_RECOMP_H

#include <stdint.h>
#include <string.h>

#ifdef __cplusplus
extern "C" {
#endif

#define RECOMP_RAM_SIZE 0x00800000u /* 8 MB main RAM at physical 0 */

typedef struct RecompContext {
  uint64_t r[32];   /* GPRs; 32-bit results are kept sign-extended */
  uint64_t hi, lo;
  uint32_t f[32];   /* FPRs as raw 32-bit words (FR=0: double n = f[n] | f[n+1] << 32) */
  uint32_t fcsr;    /* FPU control/status (cp1 $31) */
} RecompContext;

typedef void (*RecompFunc)(RecompContext* ctx);

extern uint8_t* recomp_rdram; /* host pointer to guest physical RAM */

/* ---- runtime hooks (implemented by the runtime) ---- */
uint32_t recomp_mmio_read(RecompContext* ctx, uint32_t vaddr, int size);
void recomp_mmio_write(RecompContext* ctx, uint32_t vaddr, uint32_t value, int size);
RecompFunc recomp_lookup(RecompContext* ctx, uint32_t vaddr); /* indirect calls/jumps */
void recomp_syscall(RecompContext* ctx, uint32_t code, uint32_t pc);
void recomp_break(RecompContext* ctx, uint32_t code, uint32_t pc);
void recomp_trap(RecompContext* ctx, uint32_t pc);
uint64_t recomp_cop0_read(RecompContext* ctx, int reg, uint32_t pc);
void recomp_cop0_write(RecompContext* ctx, int reg, uint64_t value, uint32_t pc);
void recomp_unhandled(RecompContext* ctx, uint32_t pc, const char* what);

/* ---- integer helpers ---- */
#define SE32(x) ((uint64_t)(int64_t)(int32_t)(uint32_t)(x))
#define R32(n) ((uint32_t)ctx->r[n])
#define RS32(n) ((int32_t)ctx->r[n])

static inline uint32_t recomp_phys(uint32_t a) { return a & 0x1FFFFFFFu; }

static inline uint32_t mem_r32(RecompContext* ctx, uint32_t a) {
  uint32_t p = recomp_phys(a);
  if (p < RECOMP_RAM_SIZE) { uint32_t v; memcpy(&v, recomp_rdram + p, 4); return v; }
  return recomp_mmio_read(ctx, a, 4);
}
static inline uint16_t mem_r16(RecompContext* ctx, uint32_t a) {
  uint32_t p = recomp_phys(a);
  if (p < RECOMP_RAM_SIZE) { uint16_t v; memcpy(&v, recomp_rdram + p, 2); return v; }
  return (uint16_t)recomp_mmio_read(ctx, a, 2);
}
static inline uint8_t mem_r8(RecompContext* ctx, uint32_t a) {
  uint32_t p = recomp_phys(a);
  if (p < RECOMP_RAM_SIZE) return recomp_rdram[p];
  return (uint8_t)recomp_mmio_read(ctx, a, 1);
}
static inline uint64_t mem_r64(RecompContext* ctx, uint32_t a) {
  return (uint64_t)mem_r32(ctx, a) | ((uint64_t)mem_r32(ctx, a + 4) << 32);
}
static inline void mem_w32(RecompContext* ctx, uint32_t a, uint32_t v) {
  uint32_t p = recomp_phys(a);
  if (p < RECOMP_RAM_SIZE) { memcpy(recomp_rdram + p, &v, 4); return; }
  recomp_mmio_write(ctx, a, v, 4);
}
static inline void mem_w16(RecompContext* ctx, uint32_t a, uint16_t v) {
  uint32_t p = recomp_phys(a);
  if (p < RECOMP_RAM_SIZE) { memcpy(recomp_rdram + p, &v, 2); return; }
  recomp_mmio_write(ctx, a, v, 2);
}
static inline void mem_w8(RecompContext* ctx, uint32_t a, uint8_t v) {
  uint32_t p = recomp_phys(a);
  if (p < RECOMP_RAM_SIZE) { recomp_rdram[p] = v; return; }
  recomp_mmio_write(ctx, a, v, 1);
}
static inline void mem_w64(RecompContext* ctx, uint32_t a, uint64_t v) {
  mem_w32(ctx, a, (uint32_t)v);
  mem_w32(ctx, a + 4, (uint32_t)(v >> 32));
}

/* Unaligned word access, little-endian semantics. */
static inline uint32_t do_lwl(RecompContext* ctx, uint32_t a, uint32_t rt) {
  uint32_t b = a & 3, m = mem_r32(ctx, a & ~3u);
  return (rt & (0x00FFFFFFu >> (b * 8))) | (m << (24 - b * 8));
}
static inline uint32_t do_lwr(RecompContext* ctx, uint32_t a, uint32_t rt) {
  uint32_t b = a & 3, m = mem_r32(ctx, a & ~3u);
  return (rt & (uint32_t)(0xFFFFFFFF00000000ull >> (b * 8))) | (m >> (b * 8));
}
static inline void do_swl(RecompContext* ctx, uint32_t a, uint32_t rt) {
  uint32_t b = a & 3, m = mem_r32(ctx, a & ~3u);
  uint32_t mask = (uint32_t)(0xFFFFFFFFull >> ((3 - b) * 8));
  mem_w32(ctx, a & ~3u, (m & ~mask) | (rt >> ((3 - b) * 8)));
}
static inline void do_swr(RecompContext* ctx, uint32_t a, uint32_t rt) {
  uint32_t b = a & 3, m = mem_r32(ctx, a & ~3u);
  uint32_t keep = (uint32_t)((1ull << (b * 8)) - 1);
  mem_w32(ctx, a & ~3u, (m & keep) | (rt << (b * 8)));
}

static inline void do_div(RecompContext* ctx, int32_t a, int32_t b) {
  if (b == 0) { ctx->lo = SE32(a < 0 ? 1 : -1); ctx->hi = SE32(a); return; }
  if (a == INT32_MIN && b == -1) { ctx->lo = SE32(a); ctx->hi = 0; return; }
  ctx->lo = SE32(a / b); ctx->hi = SE32(a % b);
}
static inline void do_divu(RecompContext* ctx, uint32_t a, uint32_t b) {
  if (b == 0) { ctx->lo = SE32(0xFFFFFFFFu); ctx->hi = SE32(a); return; }
  ctx->lo = SE32(a / b); ctx->hi = SE32(a % b);
}

/* ---- FPU helpers ---- */
static inline float fpr_s(RecompContext* ctx, int n) { float v; memcpy(&v, &ctx->f[n], 4); return v; }
static inline void set_fpr_s(RecompContext* ctx, int n, float v) { memcpy(&ctx->f[n], &v, 4); }
static inline double fpr_d(RecompContext* ctx, int n) {
  uint64_t b = (uint64_t)ctx->f[n & ~1] | ((uint64_t)ctx->f[(n & ~1) + 1] << 32);
  double v; memcpy(&v, &b, 8); return v;
}
static inline void set_fpr_d(RecompContext* ctx, int n, double v) {
  uint64_t b; memcpy(&b, &v, 8);
  ctx->f[n & ~1] = (uint32_t)b; ctx->f[(n & ~1) + 1] = (uint32_t)(b >> 32);
}
static inline uint64_t fpr_l(RecompContext* ctx, int n) {
  return (uint64_t)ctx->f[n & ~1] | ((uint64_t)ctx->f[(n & ~1) + 1] << 32);
}
static inline void set_fpr_l(RecompContext* ctx, int n, uint64_t v) {
  ctx->f[n & ~1] = (uint32_t)v; ctx->f[(n & ~1) + 1] = (uint32_t)(v >> 32);
}

/* FCSR condition bits: cc0 = bit 23, cc1..7 = bits 25..31 */
static inline uint32_t fcc_bit(int cc) { return cc == 0 ? (1u << 23) : (1u << (24 + cc)); }
static inline int get_fcc(RecompContext* ctx, int cc) { return (ctx->fcsr & fcc_bit(cc)) != 0; }
static inline void set_fcc(RecompContext* ctx, int cc, int v) {
  if (v) ctx->fcsr |= fcc_bit(cc); else ctx->fcsr &= ~fcc_bit(cc);
}

/* c.cond.fmt: cond bits = [less, equal, unordered] in bits 2..0 */
static inline int fcmp(double a, double b, int cond) {
  int unord = (a != a) || (b != b);
  int r = 0;
  if (unord) r = cond & 1;
  else r = ((cond & 4) && a < b) || ((cond & 2) && a == b);
  return r;
}

/* Float -> integer conversions with defined results for NaN/overflow
   (the hardware would raise Invalid; with traps off it yields 0x7fffffff). */
int32_t recomp_cvt_w(double v, int mode); /* mode: 0 round, 1 trunc, 2 ceil, 3 floor, -1 = FCSR */
int64_t recomp_cvt_l(double v, int mode);

#ifdef __cplusplus
}
#endif
#endif /* NBA_RECOMP_H */
