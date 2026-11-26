// ADSP-2100 family core (ADSP-2104 configuration). See adsp2100.h.
#include "adsp2100.h"

#include <stdio.h>
#include <string.h>

/* data register indices in the bank array */
enum { AX0, AX1, MX0, MX1, AY0, AY1, MY0, MY1, SI, SE, AR, MR0, MR1, MR2, SR0, SR1, AF, MF };

/* ASTAT bits */
#define AZ 0x01
#define AN 0x02
#define AV 0x04
#define AC 0x08
#define AS 0x10
#define AQ 0x20
#define MV 0x40
#define SS 0x80

/* SSTAT bits */
#define PC_EMPTY 0x01
#define PC_OVER 0x02
#define CNT_EMPTY 0x04
#define CNT_OVER 0x08
#define STAT_EMPTY 0x10
#define STAT_OVER 0x20
#define LOOP_EMPTY 0x40
#define LOOP_OVER 0x80

#define S16(v) ((int32_t)(int16_t)(v))

/* ---------------------------------------------------------------- stacks */
static void pc_push(Adsp2100* c) {
  if (c->pc_sp < 16) c->pc_stack[c->pc_sp++] = c->pc;
  else c->sstat |= PC_OVER;
  c->sstat &= ~PC_EMPTY;
}
static uint16_t pc_pop_val(Adsp2100* c) {
  if (c->pc_sp > 0) c->pc_sp--;
  if (c->pc_sp == 0) c->sstat |= PC_EMPTY;
  return c->pc_stack[c->pc_sp];
}
static uint16_t pc_top(const Adsp2100* c) { return c->pc_sp ? c->pc_stack[c->pc_sp - 1] : c->pc_stack[0]; }

static void loop_push(Adsp2100* c, uint32_t v) {
  if (c->loop_sp < 4) {
    c->loop_stack[c->loop_sp++] = v;
    c->loop_end = (uint16_t)(v >> 4);
    c->loop_cond = (int)(v & 15);
    c->sstat &= ~LOOP_EMPTY;
  } else {
    c->sstat |= LOOP_OVER;
  }
}
static void loop_pop(Adsp2100* c) {
  if (c->loop_sp > 0) c->loop_sp--;
  if (c->loop_sp == 0) {
    c->loop_end = 0xFFFF;
    c->loop_cond = 0;
    c->sstat |= LOOP_EMPTY;
  } else {
    c->loop_end = (uint16_t)(c->loop_stack[c->loop_sp - 1] >> 4);
    c->loop_cond = (int)(c->loop_stack[c->loop_sp - 1] & 15);
  }
}

static void cntr_push(Adsp2100* c) {
  if (c->cntr_sp < 4) c->cntr_stack[c->cntr_sp++] = c->cntr;
  else c->sstat |= CNT_OVER;
  c->sstat &= ~CNT_EMPTY;
}
static void cntr_pop(Adsp2100* c) {
  if (c->cntr_sp > 0) c->cntr = c->cntr_stack[--c->cntr_sp];
  if (c->cntr_sp == 0) c->sstat |= CNT_EMPTY;
}

static void set_mstat(Adsp2100* c, uint16_t v) {
  const uint16_t old = c->mstat;
  c->mstat = v & 0x7F;
  c->r = c->bank[c->mstat & ADSP_MSTAT_SEC_REG];
  if (c->mstat_changed && old != c->mstat) c->mstat_changed(c->user, old, c->mstat);
}

static void stat_push(Adsp2100* c) {
  if (c->stat_sp < 4) {
    c->stat_stack[c->stat_sp][0] = c->astat;
    c->stat_stack[c->stat_sp][1] = c->mstat;
    c->stat_stack[c->stat_sp][2] = c->imask;
    c->stat_sp++;
  } else {
    c->sstat |= STAT_OVER;
  }
  c->sstat &= ~STAT_EMPTY;
}
static void stat_pop(Adsp2100* c) {
  if (c->stat_sp > 0) {
    c->stat_sp--;
    c->astat = c->stat_stack[c->stat_sp][0];
    set_mstat(c, c->stat_stack[c->stat_sp][1]);
    c->imask = c->stat_stack[c->stat_sp][2];
  }
  if (c->stat_sp == 0) c->sstat |= STAT_EMPTY;
}

/* ---------------------------------------------------------------- conditions */
static int condition(Adsp2100* c, int cond) {
  const int az = (c->astat & AZ) != 0, an = (c->astat & AN) != 0, av = (c->astat & AV) != 0;
  switch (cond) {
    case 0: return az;
    case 1: return !az;
    case 2: return !((an ^ av) | az);
    case 3: return (an ^ av) | az;
    case 4: return an ^ av;
    case 5: return !(an ^ av);
    case 6: return av;
    case 7: return !av;
    case 8: return (c->astat & AC) != 0;
    case 9: return (c->astat & AC) == 0;
    case 10: return (c->astat & AS) != 0;
    case 11: return (c->astat & AS) == 0;
    case 12: return (c->astat & MV) != 0;
    case 13: return (c->astat & MV) == 0;
    case 14: /* counter not expired: decrements CNTR, pops the counter stack when it expires */
      if ((int16_t)--c->cntr > 0 && c->cntr != 0) return 1;
      cntr_pop(c);
      return 0;
    default: return 1;
  }
}

/* ---------------------------------------------------------------- registers */
static uint16_t read_dreg(Adsp2100* c, int n) {
  switch (n) {
    case SE: return (uint16_t)(int16_t)(int8_t)c->r[SE];
    case MR2: return (uint16_t)(int16_t)(int8_t)c->r[MR2];
    default: return c->r[n];
  }
}
static void write_dreg(Adsp2100* c, int n, uint16_t v) {
  switch (n) {
    case SE: c->r[SE] = v & 0xFF; break;
    case MR1: c->r[MR1] = v; c->r[MR2] = (v & 0x8000) ? 0xFF : 0x00; break; /* MR1 load sign-extends MR2 */
    case MR2: c->r[MR2] = v & 0xFF; break;
    default: c->r[n] = v; break;
  }
}

static uint16_t read_reg(Adsp2100* c, int group, int n) {
  switch (group) {
    case 0: return read_dreg(c, n);
    case 1:
    case 2: {
      const int base = (group - 1) * 4;
      if (n < 4) return c->i[base + n];
      if (n < 8) return (uint16_t)((c->m[base + n - 4] & 0x2000) ? (c->m[base + n - 4] | 0xC000) : c->m[base + n - 4]);
      if (n < 12) return c->l[base + n - 8];
      return 0;
    }
    default:
      switch (n) {
        case 0: return c->astat;
        case 1: return c->mstat;
        case 2: return c->sstat;
        case 3: return c->imask;
        case 4: return c->icntl;
        case 5: return c->cntr;
        case 6: return (uint16_t)c->sb;
        case 7: return c->px;
        case 8: return c->rx0;
        case 9: return c->tx0;
        case 10: return c->rx1;
        case 11: return c->tx1;
        default: return 0;
      }
  }
}

static void write_reg(Adsp2100* c, int group, int n, uint16_t v) {
  switch (group) {
    case 0: write_dreg(c, n, v); break;
    case 1:
    case 2: {
      const int base = (group - 1) * 4;
      if (n < 4) c->i[base + n] = v & 0x3FFF;
      else if (n < 8) c->m[base + n - 4] = v & 0x3FFF;
      else if (n < 12) c->l[base + n - 8] = v & 0x3FFF;
      break;
    }
    default:
      switch (n) {
        case 0: c->astat = v & 0xFF; break;
        case 1: set_mstat(c, v); break;
        case 3: c->imask = v & 0x3F; break;
        case 4: c->icntl = v & 0x1F; break;
        case 5: cntr_push(c); c->cntr = v & 0x3FFF; break;
        case 6: c->sb = (int16_t)((v & 0x10) ? (v | 0xFFE0) : (v & 0x1F)); break;
        case 7: c->px = v & 0xFF; break;
        case 8: c->rx0 = v; break;
        case 9: c->tx0 = v; break;
        case 10: c->rx1 = v; break;
        case 11: c->tx1 = v; break;
        case 12: /* IFC: bits 15:8 force, 7:0 clear (low bit = timer ... bit 5 = IRQ2) */
          for (int k = 0; k < 6; ++k) {
            const int bit = 5 - k; /* priority index k <-> bit (5 - k) */
            if (v & (0x100u << bit)) c->irq_latch[k] = 1;
            if (v & (1u << bit)) c->irq_latch[k] = 0;
          }
          break;
        case 13: c->cntr = v & 0x3FFF; break; /* OWRCNTR: no push */
        default: break;
      }
      break;
  }
}

/* ---------------------------------------------------------------- DAGs */
static uint16_t bit_reverse14(uint16_t a) {
  uint16_t r = 0;
  for (int b = 0; b < 14; ++b)
    if (a & (1u << b)) r |= (uint16_t)(1u << (13 - b));
  return r;
}

/* Post-modify address generation: returns the address, updates I with circular wrap. */
static uint16_t dag(Adsp2100* c, int ireg, int mreg) {
  const uint16_t addr = c->i[ireg];
  const int32_t m = (c->m[mreg] & 0x2000) ? (int32_t)(c->m[mreg] | 0xFFFFC000u) : (int32_t)c->m[mreg];
  int32_t next = (int32_t)addr + m;
  const uint16_t len = c->l[ireg];
  if (len) {
    uint32_t span = 1;
    while (span < len) span <<= 1;
    const int32_t base = addr & ~(int32_t)(span - 1);
    if (next >= base + len) next -= len;
    else if (next < base) next += len;
  }
  c->i[ireg] = (uint16_t)(next & 0x3FFF);
  if (ireg < 4 && (c->mstat & ADSP_MSTAT_BIT_REV)) return bit_reverse14(addr);
  return addr;
}

static uint16_t dm_r(Adsp2100* c, uint16_t a) { return c->dm_read(c->user, a & 0x3FFF); }
static void dm_w(Adsp2100* c, uint16_t a, uint16_t v) { c->dm_write(c->user, a & 0x3FFF, v); }
static uint32_t pm_r(Adsp2100* c, uint16_t a) { return c->pm_read(c->user, a & 0x3FFF) & 0xFFFFFF; }
static void pm_w(Adsp2100* c, uint16_t a, uint32_t v) { c->pm_write(c->user, a & 0x3FFF, v & 0xFFFFFF); }

/* 16-bit register <-> 24-bit program memory: upper 16 bits to the register, low 8 via PX. */
static uint16_t pm_read16(Adsp2100* c, uint16_t a) {
  const uint32_t w = pm_r(c, a);
  c->px = w & 0xFF;
  return (uint16_t)(w >> 8);
}
static void pm_write16(Adsp2100* c, uint16_t a, uint16_t v) { pm_w(c, a, ((uint32_t)v << 8) | c->px); }

/* ---------------------------------------------------------------- ALU */
static uint16_t alu_x(Adsp2100* c, int x) {
  static const int map[8] = {AX0, AX1, AR, MR0, MR1, MR2, SR0, SR1};
  return read_dreg(c, map[x]);
}
static uint16_t alu_y(Adsp2100* c, int y) {
  switch (y) { case 0: return c->r[AY0]; case 1: return c->r[AY1]; case 2: return c->r[AF]; default: return 0; }
}

static uint16_t add_flags(uint32_t x, uint32_t y, uint32_t cin, uint16_t* flags) {
  const uint32_t r = x + y + cin;
  uint16_t f = 0;
  if ((r & 0xFFFF) == 0) f |= AZ;
  if (r & 0x8000) f |= AN;
  if (~(x ^ y) & (x ^ r) & 0x8000) f |= AV;
  if (r & 0x10000) f |= AC;
  *flags = f;
  return (uint16_t)r;
}

/* Executes an ALU function; z = 0 -> AR, 1 -> AF. */
static void alu_op(Adsp2100* c, int amf, int yop, int xop, int z) {
  const uint32_t x = alu_x(c, xop), y = alu_y(c, yop);
  const uint32_t carry = (c->astat & AC) ? 1 : 0;
  uint16_t res = 0, f = 0;
  uint16_t astat_keep = c->astat & (AS | AQ | MV | SS);
  switch (amf & 15) {
    case 0x0: res = (uint16_t)y; f = 0; break;                                   /* PASS Y */
    case 0x1: res = add_flags(y, 1, 0, &f); break;                             /* Y + 1 */
    case 0x2: res = add_flags(x, y, carry, &f); break;                         /* X + Y + C */
    case 0x3: res = add_flags(x, y, 0, &f); break;                             /* X + Y */
    case 0x4: res = (uint16_t)~y; f = 0; break;                                   /* NOT Y */
    case 0x5: res = add_flags(0, (~y) & 0xFFFF, 1, &f); break;                 /* -Y */
    case 0x6: res = add_flags(x, (~y) & 0xFFFF, carry, &f); break;             /* X - Y + C - 1 */
    case 0x7: res = add_flags(x, (~y) & 0xFFFF, 1, &f); break;                 /* X - Y */
    case 0x8: res = add_flags(y, 0xFFFF, 0, &f); break;                        /* Y - 1 */
    case 0x9: res = add_flags(y, (~x) & 0xFFFF, 1, &f); break;                 /* Y - X */
    case 0xA: res = add_flags(y, (~x) & 0xFFFF, carry, &f); break;             /* Y - X + C - 1 */
    case 0xB: res = (uint16_t)~x; f = 0; break;                                   /* NOT X */
    case 0xC: res = (uint16_t)(x & y); f = 0; break;                              /* AND */
    case 0xD: res = (uint16_t)(x | y); f = 0; break;                              /* OR */
    case 0xE: res = (uint16_t)(x ^ y); f = 0; break;                              /* XOR */
    case 0xF:                                                                     /* ABS X */
      astat_keep &= ~AS;
      if (x & 0x8000) { astat_keep |= AS; res = (uint16_t)(0 - x); if (x == 0x8000) f |= AV; }
      else res = (uint16_t)x;
      break;
  }
  if ((amf & 15) == 0 || (amf & 15) == 4 || (amf & 15) >= 0xB) {
    if (res == 0) f |= AZ;
    if (res & 0x8000) f |= AN;
  }
  if (c->mstat & ADSP_MSTAT_AV_LATCH) f |= (c->astat & AV);
  c->astat = astat_keep | f;
  if (z) {
    c->r[AF] = res;
  } else {
    if ((c->mstat & ADSP_MSTAT_AR_SAT) && (f & AV) && (amf & 15) != 0xF)
      res = (f & AC) ? 0x8000 : 0x7FFF;
    c->r[AR] = res;
  }
}

/* ---------------------------------------------------------------- MAC */
static uint16_t mac_x(Adsp2100* c, int x) {
  static const int map[8] = {MX0, MX1, AR, MR0, MR1, MR2, SR0, SR1};
  return read_dreg(c, map[x]);
}
static uint16_t mac_y(Adsp2100* c, int y) {
  switch (y) { case 0: return c->r[MY0]; case 1: return c->r[MY1]; case 2: return c->r[MF]; default: return 0; }
}
static int64_t mr_value(Adsp2100* c) {
  int64_t v = ((int64_t)(int8_t)c->r[MR2] << 32) | ((int64_t)c->r[MR1] << 16) | c->r[MR0];
  return v;
}

static void mac_op(Adsp2100* c, int amf, int yop, int xop, int z) {
  const uint16_t xu = mac_x(c, xop), yu = mac_y(c, yop);
  int64_t p = 0, r;
  int rnd = 0, acc = 0; /* acc: 0 = set, 1 = add, -1 = subtract */
  switch (amf & 15) {
    case 0x0: /* NOP for MAC: leave everything (no flag update) */
      return;
    case 0x1: p = (int64_t)S16(xu) * S16(yu); rnd = 1; acc = 0; break;
    case 0x2: p = (int64_t)S16(xu) * S16(yu); rnd = 1; acc = 1; break;
    case 0x3: p = (int64_t)S16(xu) * S16(yu); rnd = 1; acc = -1; break;
    case 0x4: p = (int64_t)S16(xu) * S16(yu); break;
    case 0x5: p = (int64_t)S16(xu) * (int64_t)yu; break;
    case 0x6: p = (int64_t)xu * S16(yu); break;
    case 0x7: p = (int64_t)xu * (int64_t)yu; break;
    case 0x8: p = (int64_t)S16(xu) * S16(yu); acc = 1; break;
    case 0x9: p = (int64_t)S16(xu) * (int64_t)yu; acc = 1; break;
    case 0xA: p = (int64_t)xu * S16(yu); acc = 1; break;
    case 0xB: p = (int64_t)xu * (int64_t)yu; acc = 1; break;
    case 0xC: p = (int64_t)S16(xu) * S16(yu); acc = -1; break;
    case 0xD: p = (int64_t)S16(xu) * (int64_t)yu; acc = -1; break;
    case 0xE: p = (int64_t)xu * S16(yu); acc = -1; break;
    case 0xF: p = (int64_t)xu * (int64_t)yu; acc = -1; break;
  }
  if (!(c->mstat & ADSP_MSTAT_M_MODE)) p <<= 1; /* fractional mode */
  if (acc == 0) r = p;
  else if (acc > 0) r = mr_value(c) + p;
  else r = mr_value(c) - p;
  if (rnd) {
    r += 0x8000;
    if ((r & 0xFFFF) == 0) r &= ~(int64_t)0x10000; /* unbiased: exact .5 rounds to even */
  }
  r &= 0xFFFFFFFFFFll; /* 40 bits */
  const int64_t sr = (r & 0x8000000000ll) ? (r | ~0xFFFFFFFFFFll) : r;
  const int mv = sr < -(int64_t)0x80000000ll || sr > 0x7FFFFFFFll;
  c->astat = (uint16_t)((c->astat & ~MV) | (mv ? MV : 0));
  if (z) {
    c->r[MF] = (uint16_t)(r >> 16);
  } else {
    c->r[MR0] = (uint16_t)r;
    c->r[MR1] = (uint16_t)(r >> 16);
    c->r[MR2] = (uint16_t)((r >> 32) & 0xFF);
  }
}

static void compute(Adsp2100* c, int amf, int yop, int xop, int z) {
  if (amf & 0x10) alu_op(c, amf, yop, xop, z);
  else mac_op(c, amf, yop, xop, z);
}

/* ---------------------------------------------------------------- shifter */
static uint16_t shift_x(Adsp2100* c, int x) {
  static const int map[8] = {SI, SI, AR, MR0, MR1, MR2, SR0, SR1};
  return read_dreg(c, map[x]);
}

static uint32_t do_shift(uint32_t v, int n, int arith) {
  if (n >= 32 || n <= -32) return (arith && n < 0 && (v & 0x80000000u)) ? 0xFFFFFFFFu : 0;
  if (n >= 0) return v << n;
  if (arith) return (uint32_t)((int32_t)v >> -n);
  return v >> -n;
}

static int leading_signs(uint16_t x) { /* redundant sign bits after the sign bit (0..15) */
  const int s = (x >> 15) & 1;
  int k = 0;
  for (int b = 14; b >= 0 && (((x >> b) & 1) == s); --b) ++k;
  return k;
}

static void shifter(Adsp2100* c, int sf, uint16_t x, int amount) {
  uint32_t sr = ((uint32_t)c->r[SR1] << 16) | c->r[SR0];
  uint32_t res;
  const int hi = !(sf & 2), orv = sf & 1;
  switch (sf >> 2) {
    case 0: /* LSHIFT */
      res = do_shift(hi ? ((uint32_t)x << 16) : (uint32_t)x, amount, 0);
      sr = orv ? (sr | res) : res;
      break;
    case 1: /* ASHIFT */
      res = do_shift(hi ? ((uint32_t)x << 16) : (uint32_t)(int32_t)(int16_t)x, amount, 1);
      sr = orv ? (sr | res) : res;
      break;
    case 2: { /* NORM: shift by -SE; a positive SE (overflow) shifts right bringing in AC */
      const int n = -amount;
      uint32_t v = hi ? ((uint32_t)x << 16) : (uint32_t)x;
      if (n >= 0) res = do_shift(v, n, 0);
      else {
        res = do_shift(v, n, 0);
        if (hi && (c->astat & AC)) res |= 0x80000000u >> (-n - 1);
      }
      sr = orv ? (sr | res) : res;
      break;
    }
    default: /* EXP / EXPADJ */
      switch (sf & 3) {
        case 0: /* EXP (HI) */
          c->r[SE] = (uint16_t)(-leading_signs(x)) & 0xFF;
          c->astat = (uint16_t)((c->astat & ~SS) | ((x & 0x8000) ? SS : 0));
          break;
        case 1: /* EXP (HIX) */
          if (c->astat & AV) {
            c->r[SE] = 1;
            c->astat = (uint16_t)((c->astat & ~SS) | ((x & 0x8000) ? 0 : SS));
          } else {
            c->r[SE] = (uint16_t)(-leading_signs(x)) & 0xFF;
            c->astat = (uint16_t)((c->astat & ~SS) | ((x & 0x8000) ? SS : 0));
          }
          break;
        case 2: /* EXP (LO): only continues when the HI word was all sign bits */
          if ((int8_t)c->r[SE] == -15) {
            const int s = (c->astat & SS) ? 1 : 0;
            int k = 0;
            for (int b = 15; b >= 0 && (((x >> b) & 1) == s); --b) ++k;
            c->r[SE] = (uint16_t)(-15 - k) & 0xFF;
          }
          break;
        default: { /* EXPADJ */
          const int e = -leading_signs(x);
          if (e > c->sb) c->sb = (int16_t)e;
          break;
        }
      }
      return; /* SR unchanged */
  }
  c->r[SR0] = (uint16_t)sr;
  c->r[SR1] = (uint16_t)(sr >> 16);
}

static void shift_op(Adsp2100* c, uint32_t op) {
  const int sf = (int)((op >> 11) & 15);
  shifter(c, sf, shift_x(c, (int)((op >> 8) & 7)), (int8_t)c->r[SE]);
}
static void shift_imm(Adsp2100* c, uint32_t op) {
  const int sf = (int)((op >> 11) & 15);
  shifter(c, sf, shift_x(c, (int)((op >> 8) & 7)), (int8_t)(op & 0xFF));
}

/* ---------------------------------------------------------------- interrupts */
static void check_irqs(Adsp2100* c) {
  for (int k = 0; k < 6; ++k) {
    int pending;
    switch (k) { /* IRQ2/IRQ1/IRQ0 can be edge (ICNTL bit) or level */
      case ADSP_IRQ2: pending = (c->icntl & 4) ? c->irq_latch[k] : c->irq_line[k]; break;
      case ADSP_IRQ1: pending = (c->icntl & 2) ? c->irq_latch[k] : c->irq_line[k]; break;
      case ADSP_IRQ0: pending = (c->icntl & 1) ? c->irq_latch[k] : c->irq_line[k]; break;
      default: pending = c->irq_latch[k]; break;
    }
    if (!pending) continue;
    if (!(c->imask & (0x20 >> k))) continue;
    c->irq_latch[k] = 0;
    pc_push(c);
    stat_push(c);
    c->pc = (uint16_t)(0x04 + k * 4);
    c->idle = 0;
    if (c->icntl & 0x10) c->imask &= (uint16_t)~(0x3F >> k); /* nesting: mask this and lower */
    else c->imask = 0;
    return;
  }
}

void adsp_set_irq_line(Adsp2100* c, int irq, int state) {
  if (state && !c->irq_line[irq]) c->irq_latch[irq] = 1; /* edge */
  c->irq_line[irq] = (uint8_t)(state != 0);
}
void adsp_pulse_irq(Adsp2100* c, int irq) { c->irq_latch[irq] = 1; }

/* ---------------------------------------------------------------- reset */
void adsp_reset(Adsp2100* c) {
  memset(c->bank, 0, sizeof c->bank);
  memset(c->i, 0, sizeof c->i);
  memset(c->m, 0, sizeof c->m);
  memset(c->l, 0, sizeof c->l);
  c->pc = 0;
  c->pc_sp = c->loop_sp = c->cntr_sp = c->stat_sp = 0;
  c->loop_end = 0xFFFF;
  c->loop_cond = 0;
  c->astat = 0;
  c->mstat = 0;
  c->r = c->bank[0];
  c->sstat = PC_EMPTY | CNT_EMPTY | STAT_EMPTY | LOOP_EMPTY;
  c->imask = 0;
  c->icntl = 0;
  c->cntr = 0;
  c->sb = 0;
  c->px = 0;
  c->idle = 0;
  memset(c->irq_latch, 0, sizeof c->irq_latch);
  memset(c->irq_line, 0, sizeof c->irq_line);
}

/* ---------------------------------------------------------------- execution */
static void execute(Adsp2100* c, uint32_t op) {
  const int amf = (int)((op >> 13) & 31), yop = (int)((op >> 11) & 3), xop = (int)((op >> 8) & 7);
  switch (op >> 16) {
    case 0x00: break; /* NOP */
    case 0x02: /* IDLE / modify flag out */
      if ((op & 0xFFFF) >= 0x8000) c->idle = 1;
      break;
    case 0x03: { /* call/jump on flag in: FI is not connected (reads 0) */
      const int on_set = (op >> 1) & 1; /* "IF FLAG_IN" vs "IF NOT FLAG_IN" */
      if (!on_set) {
        const uint16_t addr = (uint16_t)(((op >> 4) & 0xFFF) | ((op & 0xC) << 10));
        if (op & 1) pc_push(c);
        c->pc = addr;
      }
      break;
    }
    case 0x04: /* stack control */
      if (op & 0x10) pc_pop_val(c);
      if (op & 0x08) loop_pop(c);
      if (op & 0x04) cntr_pop(c);
      if (op & 0x02) { if (op & 0x01) stat_pop(c); else stat_push(c); }
      break;
    case 0x05: /* saturate MR */
      if (c->astat & MV) {
        if (c->r[MR2] & 0x80) { c->r[MR2] = 0xFF; c->r[MR1] = 0x8000; c->r[MR0] = 0x0000; }
        else { c->r[MR2] = 0x00; c->r[MR1] = 0x7FFF; c->r[MR0] = 0xFFFF; }
      }
      break;
    case 0x06: { /* DIVS */
      const uint16_t x = alu_x(c, xop), y = alu_y(c, yop);
      const uint16_t t = x ^ y;
      c->astat = (uint16_t)((c->astat & ~AQ) | ((t & 0x8000) ? AQ : 0));
      c->r[AF] = (uint16_t)((y << 1) | (c->r[AY0] >> 15));
      c->r[AY0] = (uint16_t)((c->r[AY0] << 1) | (t >> 15));
      break;
    }
    case 0x07: { /* DIVQ */
      const uint16_t x = alu_x(c, xop);
      const uint16_t res = (c->astat & AQ) ? (uint16_t)(c->r[AF] + x) : (uint16_t)(c->r[AF] - x);
      const uint16_t t = res ^ x;
      c->astat = (uint16_t)((c->astat & ~AQ) | ((t & 0x8000) ? AQ : 0));
      c->r[AF] = (uint16_t)((res << 1) | (c->r[AY0] >> 15));
      c->r[AY0] = (uint16_t)((c->r[AY0] << 1) | ((~t >> 15) & 1));
      break;
    }
    case 0x09: { /* modify address register */
      const int ireg = (int)((op >> 2) & 7);
      dag(c, ireg, (ireg & 4) | (int)(op & 3));
      break;
    }
    case 0x0A: /* conditional return */
      if (condition(c, (int)(op & 15))) {
        c->pc = pc_pop_val(c);
        if (op & 0x10) stat_pop(c);
      }
      break;
    case 0x0B: /* conditional jump/call indirect (I4-I7) */
      if (condition(c, (int)(op & 15))) {
        if (op & 0x10) pc_push(c);
        c->pc = c->i[4 + ((op >> 6) & 3)] & 0x3FFF;
      }
      break;
    case 0x0C: { /* mode control */
      uint16_t ms = c->mstat;
      struct { int bit; uint16_t flag; } modes[] = {
          {2, ADSP_MSTAT_G_MODE}, {4, ADSP_MSTAT_SEC_REG}, {6, ADSP_MSTAT_BIT_REV},
          {8, ADSP_MSTAT_AV_LATCH}, {10, ADSP_MSTAT_AR_SAT}, {12, ADSP_MSTAT_M_MODE}, {14, ADSP_MSTAT_TIMER}};
      for (unsigned k = 0; k < sizeof modes / sizeof modes[0]; ++k)
        if (op & (2u << modes[k].bit)) ms = (op & (1u << modes[k].bit)) ? (ms | modes[k].flag) : (ms & ~modes[k].flag);
      set_mstat(c, ms);
      break;
    }
    case 0x0D: /* internal data move */
      write_reg(c, (int)((op >> 10) & 3), (int)((op >> 4) & 15),
                read_reg(c, (int)((op >> 8) & 3), (int)(op & 15)));
      break;
    case 0x0E: /* conditional shift */
      if (condition(c, (int)(op & 15))) shift_op(c, op);
      break;
    case 0x0F: shift_imm(c, op); break;
    case 0x10: { /* shift with internal data move (move reads its source first) */
      const uint16_t v = read_dreg(c, (int)(op & 15));
      shift_op(c, op);
      write_dreg(c, (int)((op >> 4) & 15), v);
      break;
    }
    case 0x11: { /* shift with PM read/write (DAG2) */
      const int dreg = (int)((op >> 4) & 15);
      const uint16_t a = dag(c, 4 + (int)((op >> 2) & 3), 4 + (int)(op & 3));
      if (op & 0x8000) { pm_write16(c, a, read_dreg(c, dreg)); shift_op(c, op); }
      else { shift_op(c, op); write_dreg(c, dreg, pm_read16(c, a)); }
      break;
    }
    case 0x12:
    case 0x13: { /* shift with DM read/write */
      const int g = (op >> 16) & 1, dreg = (int)((op >> 4) & 15);
      const uint16_t a = dag(c, g * 4 + (int)((op >> 2) & 3), g * 4 + (int)(op & 3));
      if (op & 0x8000) { dm_w(c, a, read_dreg(c, dreg)); shift_op(c, op); }
      else { shift_op(c, op); write_dreg(c, dreg, dm_r(c, a)); }
      break;
    }
    case 0x14: case 0x15: case 0x16: case 0x17: /* DO UNTIL */
      loop_push(c, op & 0x3FFFF);
      pc_push(c);
      break;
    case 0x18: case 0x19: case 0x1A: case 0x1B: /* conditional jump */
      if (condition(c, (int)(op & 15))) c->pc = (uint16_t)((op >> 4) & 0x3FFF);
      break;
    case 0x1C: case 0x1D: case 0x1E: case 0x1F: /* conditional call */
      if (condition(c, (int)(op & 15))) { pc_push(c); c->pc = (uint16_t)((op >> 4) & 0x3FFF); }
      break;
    case 0x20: case 0x21: case 0x22: case 0x23: case 0x24: case 0x25: case 0x26: case 0x27:
      /* conditional ALU/MAC */
      if (condition(c, (int)(op & 15))) compute(c, amf, yop, xop, (op >> 18) & 1);
      break;
    case 0x28: case 0x29: case 0x2A: case 0x2B: case 0x2C: case 0x2D: case 0x2E: case 0x2F: {
      /* ALU/MAC with internal data register move */
      const uint16_t v = read_dreg(c, (int)(op & 15));
      compute(c, amf, yop, xop, (op >> 18) & 1);
      write_dreg(c, (int)((op >> 4) & 15), v);
      break;
    }
    default:
      break;
  }

  if ((op >> 20) == 0x3) { /* load non-data register immediate (14-bit, sign extended) */
    uint16_t d = (uint16_t)((op >> 4) & 0x3FFF);
    if (d & 0x2000) d |= 0xC000;
    write_reg(c, (int)((op >> 18) & 3), (int)(op & 15), d);
  } else if ((op >> 20) == 0x4) { /* load data register immediate */
    write_dreg(c, (int)(op & 15), (uint16_t)(op >> 4));
  } else if ((op >> 20) == 0x5) { /* ALU/MAC with PM read/write (DAG2) */
    const int dreg = (int)((op >> 4) & 15), z = (op >> 18) & 1;
    const uint16_t a = dag(c, 4 + (int)((op >> 2) & 3), 4 + (int)(op & 3));
    if (op & 0x80000) { pm_write16(c, a, read_dreg(c, dreg)); compute(c, amf, yop, xop, z); }
    else { compute(c, amf, yop, xop, z); write_dreg(c, dreg, pm_read16(c, a)); }
  } else if ((op >> 21) == 0x3) { /* ALU/MAC with DM read/write */
    const int g = (op >> 20) & 1, dreg = (int)((op >> 4) & 15), z = (op >> 18) & 1;
    const uint16_t a = dag(c, g * 4 + (int)((op >> 2) & 3), g * 4 + (int)(op & 3));
    if (op & 0x80000) { dm_w(c, a, read_dreg(c, dreg)); compute(c, amf, yop, xop, z); }
    else { compute(c, amf, yop, xop, z); write_dreg(c, dreg, dm_r(c, a)); }
  } else if ((op >> 21) == 0x4) { /* DM read/write, immediate address */
    const int group = (int)((op >> 18) & 3), reg = (int)(op & 15);
    const uint16_t addr = (uint16_t)((op >> 4) & 0x3FFF);
    if (op & 0x100000) dm_w(c, addr, read_reg(c, group, reg));
    else write_reg(c, group, reg, dm_r(c, addr));
  } else if ((op >> 21) == 0x5) { /* DM write, immediate data */
    const int g = (op >> 20) & 1;
    const uint16_t a = dag(c, g * 4 + (int)((op >> 2) & 3), g * 4 + (int)(op & 3));
    dm_w(c, a, (uint16_t)(op >> 4));
  } else if ((op >> 22) == 0x3) { /* ALU/MAC with DM and PM reads (result to AR/MR) */
    static const int ddst[4] = {AX0, AX1, MX0, MX1}, pdst[4] = {AY0, AY1, MY0, MY1};
    const uint16_t da = dag(c, (int)((op >> 2) & 3), (int)(op & 3));
    const uint16_t pa = dag(c, 4 + (int)((op >> 6) & 3), 4 + (int)((op >> 4) & 3));
    compute(c, amf, yop, xop, 0);
    write_dreg(c, ddst[(op >> 18) & 3], dm_r(c, da));
    write_dreg(c, pdst[(op >> 20) & 3], pm_read16(c, pa));
  }
}

int adsp_run(Adsp2100* c, int cycles) {
  int done = 0;
  while (done < cycles) {
    check_irqs(c);
    if (c->idle) { c->cycles += (uint64_t)(cycles - done); return cycles; }
    const uint32_t op = pm_r(c, c->pc);
    if (c->trace_on) {
      c->trace_pc[c->trace_pos & 255] = c->pc;
      c->trace_op[c->trace_pos & 255] = op;
      c->trace_pos++;
    }
    if (c->pc != c->loop_end) {
      c->pc++;
    } else if (condition(c, c->loop_cond)) {
      c->pc = pc_top(c); /* loop back to the start */
    } else {
      loop_pop(c);
      pc_pop_val(c);
      c->pc++;
    }
    execute(c, op);
    ++done;
    c->cycles++;
  }
  return done;
}

/* ---------------------------------------------------------------- disassembler (minimal) */
char* adsp_disasm(uint32_t op, uint16_t pc, char* out, int size) {
  const char* name;
  switch (op >> 16) {
    case 0x00: name = "nop"; break;
    case 0x02: name = (op & 0xFFFF) >= 0x8000 ? "idle" : "flag"; break;
    case 0x04: name = "stackctl"; break;
    case 0x0A: name = (op & 0x10) ? "rti" : "rts"; break;
    case 0x0B: name = (op & 0x10) ? "call (i)" : "jump (i)"; break;
    case 0x0C: name = "mode"; break;
    case 0x0D: name = "move"; break;
    case 0x0E: case 0x0F: case 0x10: case 0x11: case 0x12: case 0x13: name = "shift"; break;
    case 0x14: case 0x15: case 0x16: case 0x17: name = "do"; break;
    case 0x18: case 0x19: case 0x1A: case 0x1B: name = "jump"; break;
    case 0x1C: case 0x1D: case 0x1E: case 0x1F: name = "call"; break;
    default:
      if ((op >> 20) == 3) name = "ld nreg,#";
      else if ((op >> 20) == 4) name = "ld dreg,#";
      else if ((op >> 20) == 5) name = "alu/mac + pm";
      else if ((op >> 21) == 3) name = "alu/mac + dm";
      else if ((op >> 21) == 4) name = (op & 0x100000) ? "dm(#)=reg" : "reg=dm(#)";
      else if ((op >> 21) == 5) name = "dm()=#";
      else if ((op >> 22) == 3) name = "alu/mac + dm + pm";
      else name = "alu/mac";
      break;
  }
  snprintf(out, (size_t)size, "%04X: %06X  %s", pc, op & 0xFFFFFF, name);
  return out;
}
