// Interface between recompiled game code (generated/*.c) and the native runtime.
//
// Guest: MIPS R5000, little-endian, 32-bit kernel addresses (kseg0/kseg1), FPU in FR=0
// mode (16 double registers made of even/odd 32-bit pairs).
//
// Three ways to link (see docs/launcher.md):
//   default               generated code and runtime in one program (smoke test, trace builds)
//   RECOMP_GAME_MODULE    compiling the generated code as the game library (nbagame.so/.dll):
//                         runtime hooks are called through the RecompHostApi table the engine
//                         hands over; the library owns the shared state below
//   RECOMP_ENGINE_DYNAMIC compiling the engine that loads that library: the shared state and
//                         the function table are reached through recomp_game
#ifndef NBA_RECOMP_H
#define NBA_RECOMP_H

#include <stdint.h>
#include <string.h>

#include "recomp_version.h"

#if defined(RECOMP_GAME_MODULE) && defined(RECOMP_ENGINE_DYNAMIC)
#error "RECOMP_GAME_MODULE and RECOMP_ENGINE_DYNAMIC are exclusive"
#endif
#if defined(RECOMP_TRACE_CALLS) && (defined(RECOMP_GAME_MODULE) || defined(RECOMP_ENGINE_DYNAMIC))
#error "RECOMP_TRACE_CALLS needs the static build"
#endif
#if defined(RECOMP_GAME_MODULE) && defined(__GNUC__) && !defined(_WIN32)
/* Everything in the library is internal (direct, PC-relative access on hot paths) except the one
   exported entry point. */
#pragma GCC visibility push(hidden)
#endif

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
typedef struct { uint32_t addr; RecompFunc fn; } RecompFuncEntry; /* sorted by addr */

/* A program the BIOS can load at 0x80100000 and exec (syscall 45): the game, the operator menu
   (DIAG.EXE). Their code addresses overlap, so each has its own function table, and indirect
   calls at or above RECOMP_PROGRAM_BASE go to the program that is loaded (generated/programs.c;
   the first entry is the game, whose table is the main one with the BIOS). */
#define RECOMP_PROGRAM_BASE 0x80100000u
typedef struct RecompProgram {
  const char* name;
  uint32_t text_start, text_end, text_crc; /* identifies it in RAM: CRC-32 of its code */
  uint32_t proc_sleep;                     /* its process sleep routine (runtime/src/proc.c) */
  const RecompFuncEntry* funcs;
  unsigned func_count;
} RecompProgram;

/* FP exception kinds for recomp_fpe_arith (runtime/src/fpu.c). */
enum { RECOMP_FPE_GENERIC = 0, RECOMP_FPE_DIV = 1, RECOMP_FPE_SQRT = 2, RECOMP_FPE_RSQRT = 3, RECOMP_FPE_RECIP = 4 };

/* ---- runtime hooks called by generated code (implemented by the runtime) ----
   X(return type, name, parameters). In a game library each is a field of recomp_host.
   - mmio_read/write: physical addresses outside main RAM (hardware registers)
   - lookup: indirect calls/jumps; syscall/break/trap/cop0: privileged instructions
   - slice: loop budget ran out (RECOMP_LOOP); proc_switch_in/out: game process switching
     (runtime/src/proc.c), emitted at the scheduler's resume `jr ra` and the jumps back into it
   - exec: the BIOS exec starting a program it just loaded (never returns)
   - fpe_arith/fpe_invalid: FP exceptions. Generated code checks results inline and calls these
     only for non-finite results / invalid conversions; a nonzero return means the BIOS FP
     exception handler supplied the result (the destination register must not be written)
   - set_fcsr: ctc1 $31, also applies FS / RM to the host FPU
   - cvt_w/cvt_l: float -> integer with the guest's results for NaN / overflow
     (mode: 0 round, 1 trunc, 2 ceil, 3 floor, -1 = FCSR) */
#define RECOMP_HOST_FUNCS(X)                                                                    \
  X(uint32_t, recomp_mmio_read, (RecompContext * ctx, uint32_t vaddr, int size))                 \
  X(void, recomp_mmio_write, (RecompContext * ctx, uint32_t vaddr, uint32_t value, int size))    \
  X(RecompFunc, recomp_lookup, (RecompContext * ctx, uint32_t vaddr))                            \
  X(void, recomp_syscall, (RecompContext * ctx, uint32_t code, uint32_t pc))                     \
  X(void, recomp_break, (RecompContext * ctx, uint32_t code, uint32_t pc))                       \
  X(void, recomp_trap, (RecompContext * ctx, uint32_t pc))                                       \
  X(uint64_t, recomp_cop0_read, (RecompContext * ctx, int reg, uint32_t pc))                     \
  X(void, recomp_cop0_write, (RecompContext * ctx, int reg, uint64_t value, uint32_t pc))        \
  X(void, recomp_unhandled, (RecompContext * ctx, uint32_t pc, const char* what))                \
  X(void, recomp_slice, (RecompContext * ctx))                                                   \
  X(void, recomp_proc_switch_in, (RecompContext * ctx))                                          \
  X(void, recomp_proc_switch_out, (RecompContext * ctx, uint32_t from))                          \
  X(void, recomp_exec, (RecompContext * ctx, uint32_t entry))                                    \
  X(int, recomp_fpe_arith,                                                                       \
    (RecompContext * ctx, uint32_t pc, int kind, double a, double b, double c, double r))        \
  X(int, recomp_fpe_invalid, (RecompContext * ctx, uint32_t pc))                                 \
  X(void, recomp_set_fcsr, (RecompContext * ctx, uint32_t v))                                    \
  X(int32_t, recomp_cvt_w, (double v, int mode))                                                 \
  X(int64_t, recomp_cvt_l, (double v, int mode))

/* What the engine hands the game library. */
typedef struct RecompHostApi {
  uint32_t abi_version; /* RECOMP_GAME_ABI */
  uint32_t size;        /* sizeof(RecompHostApi) */
#define RECOMP_X_FIELD(ret, name, args) ret(*name) args;
  RECOMP_HOST_FUNCS(RECOMP_X_FIELD)
#undef RECOMP_X_FIELD
} RecompHostApi;

/* What the game library hands back. The first two fields never move, so any engine can read
   them and tell the player to rebuild when they don't match. */
typedef struct RecompGameExports {
  uint32_t abi_version;      /* RECOMP_GAME_ABI the library was built with */
  uint32_t codegen_revision; /* RECOMP_CODEGEN_REVISION the code was generated with */
  const RecompFuncEntry* func_table;
  unsigned func_count;
  const RecompProgram* programs;
  unsigned program_count;
  uint8_t** rdram;  /* the library's shared state (see below) */
  int32_t* budget;
  float* ws_xmin;
} RecompGameExports;
typedef const RecompGameExports* (*RecompGameInitFn)(const RecompHostApi* host);
#define RECOMP_GAME_INIT_SYMBOL "recomp_game_init"

/* ---- shared state ----
   recomp_rdram: host pointer to guest physical RAM; recomp_budget: loop budget (RECOMP_LOOP);
   recomp_ws_xmin: widescreen left edge for patched game instructions (runtime/src/widescreen.c,
   0 = original); recomp_func_table / recomp_func_count: every recompiled function. */
#ifdef RECOMP_ENGINE_DYNAMIC
extern const RecompGameExports* recomp_game; /* set by the engine once the library is loaded */
#define recomp_rdram (*recomp_game->rdram)
#define recomp_budget (*recomp_game->budget)
#define recomp_ws_xmin (*recomp_game->ws_xmin)
#define recomp_func_table (recomp_game->func_table)
#define recomp_func_count (recomp_game->func_count)
#define recomp_programs (recomp_game->programs)
#define recomp_program_count (recomp_game->program_count)
#else
extern uint8_t* recomp_rdram;
extern int32_t recomp_budget;
extern float recomp_ws_xmin;
extern const RecompFuncEntry recomp_func_table[];
extern const unsigned recomp_func_count;
extern const RecompProgram recomp_programs[];
extern const unsigned recomp_program_count;
#endif

#ifdef RECOMP_GAME_MODULE
extern RecompHostApi recomp_host;
#define recomp_mmio_read recomp_host.recomp_mmio_read
#define recomp_mmio_write recomp_host.recomp_mmio_write
#define recomp_lookup recomp_host.recomp_lookup
#define recomp_syscall recomp_host.recomp_syscall
#define recomp_break recomp_host.recomp_break
#define recomp_trap recomp_host.recomp_trap
#define recomp_cop0_read recomp_host.recomp_cop0_read
#define recomp_cop0_write recomp_host.recomp_cop0_write
#define recomp_unhandled recomp_host.recomp_unhandled
#define recomp_slice recomp_host.recomp_slice
#define recomp_proc_switch_in recomp_host.recomp_proc_switch_in
#define recomp_proc_switch_out recomp_host.recomp_proc_switch_out
#define recomp_exec recomp_host.recomp_exec
#define recomp_fpe_arith recomp_host.recomp_fpe_arith
#define recomp_fpe_invalid recomp_host.recomp_fpe_invalid
#define recomp_set_fcsr recomp_host.recomp_set_fcsr
#define recomp_cvt_w recomp_host.recomp_cvt_w
#define recomp_cvt_l recomp_host.recomp_cvt_l
#else
#define RECOMP_X_DECL(ret, name, args) ret name args;
RECOMP_HOST_FUNCS(RECOMP_X_DECL)
#undef RECOMP_X_DECL
void recomp_fpu_host_sync(void); /* re-apply the FPU mode after a host context switch */
#endif

/* Vegas BIOS: dispatch syscall `code` the way the BIOS exception handler does
   (handler address from the syscall table in guest RAM, called as a plain function).
   Returns 0 if the table has no handler for `code`. */
#define RECOMP_BIOS_SYSCALL_TABLE 0x8002B3C8u
#define RECOMP_BIOS_SYSCALL_COUNT 104u
#ifndef RECOMP_GAME_MODULE
int recomp_bios_syscall(RecompContext* ctx, uint32_t code);

/* The program whose code is in RAM at 0x80100000 (the game at boot). recomp_exec switches it. */
const RecompProgram* recomp_program(void);
void recomp_set_program(const RecompProgram* p);
/* Which recompiled program is in RAM now (by code CRC), NULL if none. */
const RecompProgram* recomp_identify_program(void);
/* Start `entry` as a new program's main thread: forgets every process of the old program and
   runs it on a fresh host fiber (runtime/src/proc.c). Never returns. */
void recomp_proc_exec(RecompContext* ctx, uint32_t entry);
/* Host hooks around exec (optional): `started` is called once a known program is loaded, before
   it runs (e.g. to re-apply game patches); `unknown` when exec loads a program that wasn't
   recompiled (e.g. DIAG.EXE leaving through BOOT.COM): it may reboot and not return. */
extern void (*recomp_exec_started_hook)(RecompContext* ctx, const RecompProgram* p);
extern void (*recomp_exec_unknown_hook)(RecompContext* ctx, uint32_t entry);
#endif

/* Loop back-edge accounting: every backward branch charges its body size (instructions) to a
   budget; when it runs out the runtime advances virtual time and delivers interrupts. This makes
   loops that only poll RAM (e.g. waiting for a vblank counter) see interrupts, and keeps timing
   deterministic. */
#define RECOMP_LOOP(n) do { recomp_budget -= (n); if (recomp_budget < 0) recomp_slice(ctx); } while (0)

/* Optional function-entry trace for debugging (configure with -DRECOMP_TRACE_CALLS=ON):
   every recompiled function records its guest address in a ring buffer. */
#ifdef RECOMP_TRACE_CALLS
#define RECOMP_CALL_RING 4096
extern uint32_t recomp_call_ring[RECOMP_CALL_RING];
extern uint32_t recomp_call_pos;
extern uint32_t recomp_break_addr;     /* function address to stop at (0 = none) */
void recomp_break_hit(RecompContext* ctx, uint32_t addr); /* provided by the host */
/* store watch (debugging): RAM writes to [recomp_watch_lo, +len) call recomp_watch_hit */
extern uint32_t recomp_watch_lo, recomp_watch_len;
void recomp_watch_hit(RecompContext* ctx, uint32_t addr, uint32_t value);
#define RECOMP_ENTER(a) \
  do { recomp_call_ring[recomp_call_pos++ & (RECOMP_CALL_RING - 1)] = (a); \
       if ((a) == recomp_break_addr) recomp_break_hit(ctx, a); } while (0)
#define RECOMP_WATCH(p, v) \
  do { if (__builtin_expect((p) - recomp_watch_lo < recomp_watch_len, 0)) recomp_watch_hit(ctx, p, v); } while (0)
#else
#define RECOMP_ENTER(a) ((void)0)
#define RECOMP_WATCH(p, v) ((void)0)
#endif

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
  if (p < RECOMP_RAM_SIZE) { RECOMP_WATCH(p, v); memcpy(recomp_rdram + p, &v, 4); return; }
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
static inline int recomp_cvt_w_bad(double v) { return !(v > -2147483648.5 && v < 2147483647.5); }
static inline int recomp_cvt_l_bad(double v) { return !(v > -9.2233720368547758e18 && v < 9.2233720368547758e18); }

static inline uint32_t recomp_ws_xmin_bits(void) { uint32_t b; memcpy(&b, &recomp_ws_xmin, 4); return b; }
#define RECOMP_UNLIKELY(x) __builtin_expect(!!(x), 0)

#ifdef __cplusplus
}
#endif
#if defined(RECOMP_GAME_MODULE) && defined(__GNUC__) && !defined(_WIN32)
#pragma GCC visibility pop
#endif
#endif /* NBA_RECOMP_H */
