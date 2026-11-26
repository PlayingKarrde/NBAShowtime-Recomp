// R5000 FPU control: FCSR side effects on the host FPU, and floating-point exceptions.
//
// The game sets FCSR = 0x01000C00 in its crt0: FS (flush denormals to zero) and the Invalid and
// Divide-by-zero exception enables. On the real board an invalid operation (0/0, sqrt(-x),
// inf-inf, a signalling compare with a NaN operand, an out-of-range float->int conversion) never
// produces a NaN: it traps (ExcCode 15) into the BIOS, whose FP exception handler (0x800135CC)
// decodes the faulting COP1 instruction and writes a replacement result into the exception frame.
// Recompiled code checks non-finite results inline and calls into here, which raises that
// exception through the recompiled BIOS handler exactly like an interrupt is delivered.
#include <math.h>
#include <stdint.h>
#include <string.h>

#include "recomp.h"
#include "vegas.h"

#if defined(__SSE__) || defined(_M_X64) || defined(_M_IX86_FP)
#include <xmmintrin.h>
#define HAVE_MXCSR 1
#endif

#define BIOS_EXC_FRAME 0x8002B710u
#define BIOS_EXC_HANDLER 0x80013728u
#define FRAME_SIZE 0x320u
#define FRAME_LO 0x100u
#define FRAME_HI 0x108u
#define FRAME_EPC 0x110u
#define FRAME_COP0(n) (0x118u + 8u * (n))
#define FRAME_FPR(n) (0x218u + 8u * (n))
#define FRAME_FIR 0x318u
#define FRAME_FCSR 0x31Cu

#define EXC_FPE 15u

/* FCSR cause bits (12..17): I U O Z V E; enable bit = cause bit - 5, flag bit = cause bit - 10 */
#define FCSR_CAUSE_MASK 0x3F000u
#define CAUSE_Z (1u << 15)
#define CAUSE_V (1u << 16)

static uint32_t host_fcsr = 0xFFFFFFFFu;

void recomp_fpu_host_sync(void) {
#ifdef HAVE_MXCSR
  if (host_fcsr == 0xFFFFFFFFu) return;
  unsigned csr = _mm_getcsr();
  csr &= ~(0x6000u | 0x8000u | 0x0040u); /* RC, FTZ, DAZ */
  static const unsigned rc[4] = {0x0000u, 0x6000u, 0x4000u, 0x2000u}; /* RN RZ RP RM -> MXCSR RC */
  csr |= rc[host_fcsr & 3u];
  if (host_fcsr & (1u << 24)) csr |= 0x8000u | 0x0040u; /* FS: flush results and inputs */
  _mm_setcsr(csr);
#endif
}

void recomp_set_fcsr(RecompContext* ctx, uint32_t v) {
  ctx->fcsr = v;
  if ((v & 0x01000003u) != (host_fcsr & 0x01000003u) || host_fcsr == 0xFFFFFFFFu) {
    host_fcsr = v;
    recomp_fpu_host_sync();
  }
  host_fcsr = v;
}

uint64_t g_fpe_count;

/* Raise ExcCode 15 if the cause is enabled; returns 1 when the BIOS handled it (the destination
   register then holds the handler's result), 0 when the default IEEE result should be written. */
static int fpe_raise(RecompContext* ctx, uint32_t pc, uint32_t cause) {
  if (!((cause >> 5) & ctx->fcsr & 0xF80u)) {
    ctx->fcsr = (ctx->fcsr & ~FCSR_CAUSE_MASK) | cause | ((cause >> 10) & 0x7Cu);
    return 0;
  }
  if (g_fpe_count++ < 8)
    vegas_log("[fpu] %s exception at %08X (fcsr %08X)", cause & CAUSE_V ? "invalid" : "divide-by-zero", pc, ctx->fcsr);
  ctx->fcsr = (ctx->fcsr & ~FCSR_CAUSE_MASK) | cause;

  /* An exception taken inside an interrupt handler reuses the single BIOS frame: keep the outer one. */
  uint8_t outer[FRAME_SIZE];
  const int nested = g_vegas.in_interrupt;
  if (nested)
    for (uint32_t i = 0; i < FRAME_SIZE; ++i) outer[i] = mem_r8(ctx, BIOS_EXC_FRAME + i);

  RecompContext saved = *ctx;
  const uint32_t sr = g_vegas.cop0.sr;
  const uint32_t exc_cause = (g_vegas.cop0.cause & ~0x8000007Cu) | (g_vegas.hw_irq & 0xFC00u) | (EXC_FPE << 2);
  const uint32_t entry_sr = sr | 2u;
  const uint32_t F = BIOS_EXC_FRAME;
  for (int r = 1; r < 32; ++r) mem_w64(ctx, F + 8u * r, ctx->r[r]);
  mem_w64(ctx, F + FRAME_LO, ctx->lo);
  mem_w64(ctx, F + FRAME_HI, ctx->hi);
  mem_w64(ctx, F + FRAME_EPC, SE32(pc));
  mem_w64(ctx, F + FRAME_COP0(12), SE32(entry_sr));
  mem_w64(ctx, F + FRAME_COP0(13), SE32(exc_cause));
  mem_w64(ctx, F + FRAME_COP0(14), SE32(pc));
  /* FR=0 register file as the entry code stores it (64-bit slot per register, saved with FR=1):
     slot 2k = f[2k] | f[2k+1] << 32; odd slots carry the odd register in their low word. */
  for (int k = 0; k < 32; k += 2) {
    mem_w64(ctx, F + FRAME_FPR(k), (uint64_t)ctx->f[k] | ((uint64_t)ctx->f[k + 1] << 32));
    mem_w64(ctx, F + FRAME_FPR(k + 1), ctx->f[k + 1]);
  }
  mem_w32(ctx, F + FRAME_FIR, 0x2310u);
  mem_w32(ctx, F + FRAME_FCSR, ctx->fcsr);

  g_vegas.cop0.sr = entry_sr;
  g_vegas.cop0.cause = exc_cause;
  g_vegas.cop0.epc = pc;
  g_vegas.in_interrupt = 1;
  ctx->r[4] = SE32(exc_cause);
  recomp_lookup(ctx, BIOS_EXC_HANDLER)(ctx);

  /* Return path (eret to EPC + 4): integer state as before, FP state from the frame. */
  const uint32_t frame_sr = mem_r32(ctx, F + FRAME_COP0(12));
  uint32_t fr[32];
  for (int k = 0; k < 32; k += 2) {
    const uint64_t even = mem_r64(ctx, F + FRAME_FPR(k));
    const uint32_t odd_slot = mem_r32(ctx, F + FRAME_FPR(k + 1));
    fr[k] = (uint32_t)even;
    fr[k + 1] = (uint32_t)(even >> 32) != saved.f[k + 1] ? (uint32_t)(even >> 32) : odd_slot;
  }
  const uint32_t fcsr = mem_r32(ctx, F + FRAME_FCSR);
  *ctx = saved;
  memcpy(ctx->f, fr, sizeof fr);
  /* the next FP instruction would rewrite the cause field; generated code does not track it */
  recomp_set_fcsr(ctx, fcsr & ~FCSR_CAUSE_MASK);
  g_vegas.cop0.sr = frame_sr & ~2u;
  g_vegas.in_interrupt = nested;
  if (nested)
    for (uint32_t i = 0; i < FRAME_SIZE; ++i) mem_w8(ctx, BIOS_EXC_FRAME + i, outer[i]);
  return 1;
}

int recomp_fpe_arith(RecompContext* ctx, uint32_t pc, int kind, double a, double b, double c, double r) {
  if (isnan(a) || isnan(b) || isnan(c)) return 0; /* quiet NaN operands propagate without a trap */
  uint32_t cause = 0;
  switch (kind) {
    case RECOMP_FPE_DIV:
      if (b == 0) cause = a == 0 ? CAUSE_V : isinf(a) ? 0 : CAUSE_Z;
      break;
    case RECOMP_FPE_RECIP: if (a == 0) cause = CAUSE_Z; break;
    case RECOMP_FPE_RSQRT: cause = a == 0 ? CAUSE_Z : a < 0 ? CAUSE_V : 0; break;
    case RECOMP_FPE_SQRT: if (a < 0) cause = CAUSE_V; break;
    default: break;
  }
  if (!cause && isnan(r)) cause = CAUSE_V;
  if (!cause) return 0; /* overflow to infinity: the Overflow exception is not enabled */
  return fpe_raise(ctx, pc, cause);
}

int recomp_fpe_invalid(RecompContext* ctx, uint32_t pc) { return fpe_raise(ctx, pc, CAUSE_V); }
