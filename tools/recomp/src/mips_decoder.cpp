#include "mips_decoder.h"

namespace mips {

static constexpr std::string_view kOpNames[] = {
#define X(n, s) s,
    MIPS_OPS(X)
#undef X
};

std::string_view op_name(Op op) { return kOpNames[static_cast<size_t>(op)]; }

static Op special(uint32_t f, uint32_t w) {
  switch (f) {
    case 0: return Op::SLL;
    case 1: return (w >> 16) & 1 ? Op::MOVCI_T : Op::MOVCI_F;
    case 2: return Op::SRL;
    case 3: return Op::SRA;
    case 4: return Op::SLLV;
    case 6: return Op::SRLV;
    case 7: return Op::SRAV;
    case 8: return Op::JR;
    case 9: return Op::JALR;
    case 10: return Op::MOVZ;
    case 11: return Op::MOVN;
    case 12: return Op::SYSCALL;
    case 13: return Op::BREAK;
    case 15: return Op::SYNC;
    case 16: return Op::MFHI;
    case 17: return Op::MTHI;
    case 18: return Op::MFLO;
    case 19: return Op::MTLO;
    case 20: return Op::DSLLV;
    case 22: return Op::DSRLV;
    case 23: return Op::DSRAV;
    case 24: return Op::MULT;
    case 25: return Op::MULTU;
    case 26: return Op::DIV;
    case 27: return Op::DIVU;
    case 28: return Op::DMULT;
    case 29: return Op::DMULTU;
    case 30: return Op::DDIV;
    case 31: return Op::DDIVU;
    case 32: return Op::ADD;
    case 33: return Op::ADDU;
    case 34: return Op::SUB;
    case 35: return Op::SUBU;
    case 36: return Op::AND;
    case 37: return Op::OR;
    case 38: return Op::XOR;
    case 39: return Op::NOR;
    case 42: return Op::SLT;
    case 43: return Op::SLTU;
    case 44: return Op::DADD;
    case 45: return Op::DADDU;
    case 46: return Op::DSUB;
    case 47: return Op::DSUBU;
    case 48: return Op::TGE;
    case 49: return Op::TGEU;
    case 50: return Op::TLT;
    case 51: return Op::TLTU;
    case 52: return Op::TEQ;
    case 54: return Op::TNE;
    case 56: return Op::DSLL;
    case 58: return Op::DSRL;
    case 59: return Op::DSRA;
    case 60: return Op::DSLL32;
    case 62: return Op::DSRL32;
    case 63: return Op::DSRA32;
  }
  return Op::INVALID;
}

static Op regimm(uint32_t rt) {
  switch (rt) {
    case 0: return Op::BLTZ;
    case 1: return Op::BGEZ;
    case 2: return Op::BLTZL;
    case 3: return Op::BGEZL;
    case 8: return Op::TGEI;
    case 9: return Op::TGEIU;
    case 10: return Op::TLTI;
    case 11: return Op::TLTIU;
    case 12: return Op::TEQI;
    case 14: return Op::TNEI;
    case 16: return Op::BLTZAL;
    case 17: return Op::BGEZAL;
    case 18: return Op::BLTZALL;
    case 19: return Op::BGEZALL;
  }
  return Op::INVALID;
}

static Op cop0(Insn& i, uint32_t w) {
  if (i.rs & 0x10) {
    switch (w & 0x3F) {
      case 1: return Op::TLBR;
      case 2: return Op::TLBWI;
      case 6: return Op::TLBWR;
      case 8: return Op::TLBP;
      case 24: return Op::ERET;
    }
    return Op::INVALID;
  }
  switch (i.rs) {
    case 0: return Op::MFC0;
    case 1: return Op::DMFC0;
    case 4: return Op::MTC0;
    case 5: return Op::DMTC0;
  }
  return Op::INVALID;
}

static Op cop1_arith(Insn& i, uint32_t w) {
  const uint32_t f = w & 0x3F;
  const uint8_t fmt = i.fmt;
  const bool sd = fmt == FMT_S || fmt == FMT_D;
  if (f >= 48) {
    if (!sd) return Op::INVALID;
    i.cond = f & 0xF;
    i.cc = i.fd >> 2;
    return Op::FC_COND;
  }
  switch (f) {
    case 0: return sd ? Op::FADD : Op::INVALID;
    case 1: return sd ? Op::FSUB : Op::INVALID;
    case 2: return sd ? Op::FMUL : Op::INVALID;
    case 3: return sd ? Op::FDIV : Op::INVALID;
    case 4: return sd ? Op::FSQRT : Op::INVALID;
    case 5: return sd ? Op::FABS : Op::INVALID;
    case 6: return sd ? Op::FMOV : Op::INVALID;
    case 7: return sd ? Op::FNEG : Op::INVALID;
    case 8: return sd ? Op::FROUND_L : Op::INVALID;
    case 9: return sd ? Op::FTRUNC_L : Op::INVALID;
    case 10: return sd ? Op::FCEIL_L : Op::INVALID;
    case 11: return sd ? Op::FFLOOR_L : Op::INVALID;
    case 12: return sd ? Op::FROUND_W : Op::INVALID;
    case 13: return sd ? Op::FTRUNC_W : Op::INVALID;
    case 14: return sd ? Op::FCEIL_W : Op::INVALID;
    case 15: return sd ? Op::FFLOOR_W : Op::INVALID;
    case 17:
      if (!sd) return Op::INVALID;
      i.cc = i.ft >> 2;
      return (i.ft & 1) ? Op::FMOVCT : Op::FMOVCF;
    case 18: return sd ? Op::FMOVZ : Op::INVALID;
    case 19: return sd ? Op::FMOVN : Op::INVALID;
    case 21: return sd ? Op::FRECIP : Op::INVALID;
    case 22: return sd ? Op::FRSQRT : Op::INVALID;
    case 32: return fmt != FMT_S ? Op::FCVT_S : Op::INVALID;
    case 33: return fmt != FMT_D ? Op::FCVT_D : Op::INVALID;
    case 36: return sd ? Op::FCVT_W : Op::INVALID;
    case 37: return sd ? Op::FCVT_L : Op::INVALID;
  }
  return Op::INVALID;
}

static Op cop1(Insn& i, uint32_t w) {
  switch (i.rs) {
    case 0: return Op::MFC1;
    case 1: return Op::DMFC1;
    case 2: return Op::CFC1;
    case 4: return Op::MTC1;
    case 5: return Op::DMTC1;
    case 6: return Op::CTC1;
    case 8: {
      i.cc = (w >> 18) & 7;
      const bool nd = (w >> 17) & 1, tf = (w >> 16) & 1;
      if (nd) return tf ? Op::BC1TL : Op::BC1FL;
      return tf ? Op::BC1T : Op::BC1F;
    }
    case FMT_S: case FMT_D: case FMT_W: case FMT_L:
      i.fmt = i.rs;
      return cop1_arith(i, w);
  }
  return Op::INVALID;
}

static Op cop1x(Insn& i, uint32_t w) {
  const uint32_t f = w & 0x3F;
  switch (f) {
    case 0: return Op::LWXC1;
    case 1: return Op::LDXC1;
    case 8: return Op::SWXC1;
    case 9: return Op::SDXC1;
    case 15: return Op::PREFX;
  }
  const uint32_t fmt3 = f & 7;
  if (fmt3 > 1) return Op::INVALID;
  i.fmt = fmt3 == 0 ? FMT_S : FMT_D;
  switch (f >> 3) {
    case 4: return Op::FMADD;
    case 5: return Op::FMSUB;
    case 6: return Op::FNMADD;
    case 7: return Op::FNMSUB;
  }
  i.fmt = FMT_NONE;
  return Op::INVALID;
}

Insn decode(uint32_t w) {
  Insn i;
  i.raw = w;
  i.rs = (w >> 21) & 31;
  i.rt = (w >> 16) & 31;
  i.rd = (w >> 11) & 31;
  i.sa = (w >> 6) & 31;
  i.ft = i.rt;
  i.fs = i.rd;
  i.fd = i.sa;
  i.fr = i.rs;
  i.simm = static_cast<int16_t>(w & 0xFFFF);
  i.uimm = static_cast<uint16_t>(w & 0xFFFF);
  i.target = w & 0x03FFFFFF;

  switch (w >> 26) {
    case 0: i.op = special(w & 0x3F, w); if (i.op == Op::MOVCI_F || i.op == Op::MOVCI_T) i.cc = (w >> 18) & 7; break;
    case 1: i.op = regimm(i.rt); break;
    case 2: i.op = Op::J; break;
    case 3: i.op = Op::JAL; break;
    case 4: i.op = Op::BEQ; break;
    case 5: i.op = Op::BNE; break;
    case 6: i.op = Op::BLEZ; break;
    case 7: i.op = Op::BGTZ; break;
    case 8: i.op = Op::ADDI; break;
    case 9: i.op = Op::ADDIU; break;
    case 10: i.op = Op::SLTI; break;
    case 11: i.op = Op::SLTIU; break;
    case 12: i.op = Op::ANDI; break;
    case 13: i.op = Op::ORI; break;
    case 14: i.op = Op::XORI; break;
    case 15: i.op = Op::LUI; break;
    case 16: i.op = cop0(i, w); break;
    case 17: i.op = cop1(i, w); break;
    case 19: i.op = cop1x(i, w); break;
    case 20: i.op = Op::BEQL; break;
    case 21: i.op = Op::BNEL; break;
    case 22: i.op = Op::BLEZL; break;
    case 23: i.op = Op::BGTZL; break;
    case 24: i.op = Op::DADDI; break;
    case 25: i.op = Op::DADDIU; break;
    case 26: i.op = Op::LDL; break;
    case 27: i.op = Op::LDR; break;
    case 32: i.op = Op::LB; break;
    case 33: i.op = Op::LH; break;
    case 34: i.op = Op::LWL; break;
    case 35: i.op = Op::LW; break;
    case 36: i.op = Op::LBU; break;
    case 37: i.op = Op::LHU; break;
    case 38: i.op = Op::LWR; break;
    case 39: i.op = Op::LWU; break;
    case 40: i.op = Op::SB; break;
    case 41: i.op = Op::SH; break;
    case 42: i.op = Op::SWL; break;
    case 43: i.op = Op::SW; break;
    case 44: i.op = Op::SDL; break;
    case 45: i.op = Op::SDR; break;
    case 46: i.op = Op::SWR; break;
    case 47: i.op = Op::CACHE; break;
    case 48: i.op = Op::LL; break;
    case 49: i.op = Op::LWC1; break;
    case 51: i.op = Op::PREF; break;
    case 52: i.op = Op::LLD; break;
    case 53: i.op = Op::LDC1; break;
    case 55: i.op = Op::LD; break;
    case 56: i.op = Op::SC; break;
    case 57: i.op = Op::SWC1; break;
    case 60: i.op = Op::SCD; break;
    case 61: i.op = Op::SDC1; break;
    case 63: i.op = Op::SD; break;
    default: i.op = Op::INVALID; break;
  }
  return i;
}

bool Insn::is_branch() const {
  switch (op) {
    case Op::BEQ: case Op::BNE: case Op::BLEZ: case Op::BGTZ:
    case Op::BEQL: case Op::BNEL: case Op::BLEZL: case Op::BGTZL:
    case Op::BLTZ: case Op::BGEZ: case Op::BLTZL: case Op::BGEZL:
    case Op::BLTZAL: case Op::BGEZAL: case Op::BLTZALL: case Op::BGEZALL:
    case Op::BC1F: case Op::BC1T: case Op::BC1FL: case Op::BC1TL:
      return true;
    default:
      return false;
  }
}

bool Insn::is_likely() const {
  switch (op) {
    case Op::BEQL: case Op::BNEL: case Op::BLEZL: case Op::BGTZL:
    case Op::BLTZL: case Op::BGEZL: case Op::BLTZALL: case Op::BGEZALL:
    case Op::BC1FL: case Op::BC1TL:
      return true;
    default:
      return false;
  }
}

bool Insn::is_jump() const {
  return op == Op::J || op == Op::JAL || op == Op::JR || op == Op::JALR;
}

bool Insn::is_call() const {
  switch (op) {
    case Op::JAL: case Op::JALR: case Op::BLTZAL: case Op::BGEZAL:
    case Op::BLTZALL: case Op::BGEZALL:
      return true;
    default:
      return false;
  }
}

bool Insn::ends_block() const {
  if (op == Op::J || op == Op::JR) return true;
  // "b label" is encoded as beq $0,$0
  if (op == Op::BEQ && rs == 0 && rt == 0) return true;
  if (op == Op::BGEZ && rs == 0) return true;  // also an unconditional "b"
  return false;
}

}  // namespace mips
