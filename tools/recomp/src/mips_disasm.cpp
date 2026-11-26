#include "mips_disasm.h"

#include <cstdio>

namespace mips {

const char* const kGprNames[32] = {
    "zero", "at", "v0", "v1", "a0", "a1", "a2", "a3",  // 0-7
    "t0",   "t1", "t2", "t3", "t4", "t5", "t6", "t7",  // 8-15
    "s0",   "s1", "s2", "s3", "s4", "s5", "s6", "s7",  // 16-23
    "t8",   "t9", "k0", "k1", "gp", "sp", "fp", "ra"}; // 24-31
static_assert(sizeof(kGprNames) / sizeof(kGprNames[0]) == 32);

static std::string hex(int64_t v) {
  char b[24];
  if (v < 0) std::snprintf(b, sizeof b, "-0x%llx", static_cast<unsigned long long>(-v));
  else std::snprintf(b, sizeof b, "0x%llx", static_cast<unsigned long long>(v));
  return b;
}

static std::string addr(uint32_t a) {
  char b[16];
  std::snprintf(b, sizeof b, "0x%08x", a);
  return b;
}

static const char* fmt_suffix(uint8_t f) {
  switch (f) {
    case FMT_S: return ".s";
    case FMT_D: return ".d";
    case FMT_W: return ".w";
    case FMT_L: return ".l";
  }
  return "";
}

static const char* const kCondNames[16] = {"f",  "un",  "eq",  "ueq", "olt", "ult",
                                           "ole", "ule", "sf",  "ngle", "seq", "ngl",
                                           "lt", "nge", "le",  "ngt"};

static std::string f(int n) { return "f" + std::to_string(n); }
static std::string g(int n) { return kGprNames[n]; }
static std::string mem(const Insn& i) { return hex(i.simm) + "(" + g(i.rs) + ")"; }

std::string disasm(const Insn& i, uint32_t pc) {
  std::string m(op_name(i.op));
  std::string a;
  using enum Op;
  switch (i.op) {
    case INVALID: {
      char b[32];
      std::snprintf(b, sizeof b, ".word 0x%08x", i.raw);
      return b;
    }
    case SLL:
      if (i.raw == 0) return "nop";
      [[fallthrough]];
    case SRL: case SRA: case DSLL: case DSRL: case DSRA: case DSLL32: case DSRL32: case DSRA32:
      a = g(i.rd) + ", " + g(i.rt) + ", " + std::to_string(i.sa); break;
    case SLLV: case SRLV: case SRAV: case DSLLV: case DSRLV: case DSRAV:
      a = g(i.rd) + ", " + g(i.rt) + ", " + g(i.rs); break;
    case MOVCI_F: case MOVCI_T:
      a = g(i.rd) + ", " + g(i.rs) + ", fcc" + std::to_string(i.cc); break;
    case JR: a = g(i.rs); break;
    case JALR: a = (i.rd == 31) ? g(i.rs) : g(i.rd) + ", " + g(i.rs); break;
    case SYSCALL: case BREAK: case SYNC: case TLBR: case TLBWI: case TLBWR: case TLBP: case ERET:
      return m;
    case MFHI: case MFLO: a = g(i.rd); break;
    case MTHI: case MTLO: a = g(i.rs); break;
    case MULT: case MULTU: case DIV: case DIVU: case DMULT: case DMULTU: case DDIV: case DDIVU:
    case TGE: case TGEU: case TLT: case TLTU: case TEQ: case TNE:
      a = g(i.rs) + ", " + g(i.rt); break;
    case ADD: case ADDU: case SUB: case SUBU: case AND: case OR: case XOR: case NOR:
    case SLT: case SLTU: case DADD: case DADDU: case DSUB: case DSUBU: case MOVZ: case MOVN:
      if (i.op == OR && i.rt == 0) return "move    " + g(i.rd) + ", " + g(i.rs);
      if (i.op == ADDU && i.rt == 0) return "move    " + g(i.rd) + ", " + g(i.rs);
      a = g(i.rd) + ", " + g(i.rs) + ", " + g(i.rt); break;
    case BLTZ: case BGEZ: case BLTZL: case BGEZL: case BLTZAL: case BGEZAL: case BLTZALL:
    case BGEZALL: case BLEZ: case BGTZ: case BLEZL: case BGTZL:
      a = g(i.rs) + ", " + addr(i.branch_target(pc)); break;
    case TGEI: case TGEIU: case TLTI: case TLTIU: case TEQI: case TNEI:
      a = g(i.rs) + ", " + hex(i.simm); break;
    case J: case JAL: a = addr(i.jump_target(pc)); break;
    case BEQ: case BNE: case BEQL: case BNEL:
      if (i.op == BEQ && i.rs == 0 && i.rt == 0) return "b       " + addr(i.branch_target(pc));
      a = g(i.rs) + ", " + g(i.rt) + ", " + addr(i.branch_target(pc)); break;
    case ADDI: case ADDIU: case SLTI: case SLTIU: case DADDI: case DADDIU:
      a = g(i.rt) + ", " + g(i.rs) + ", " + hex(i.simm); break;
    case ANDI: case ORI: case XORI:
      a = g(i.rt) + ", " + g(i.rs) + ", " + hex(i.uimm); break;
    case LUI: a = g(i.rt) + ", " + hex(i.uimm); break;
    case LB: case LH: case LWL: case LW: case LBU: case LHU: case LWR: case LWU: case SB: case SH:
    case SWL: case SW: case SDL: case SDR: case SWR: case LL: case LLD: case LD: case SC: case SCD:
    case SD: case LDL: case LDR:
      a = g(i.rt) + ", " + mem(i); break;
    case LWC1: case LDC1: case SWC1: case SDC1:
      a = f(i.ft) + ", " + mem(i); break;
    case CACHE: case PREF:
      a = hex(i.rt) + ", " + mem(i); break;
    case MFC0: case DMFC0: case MTC0: case DMTC0:
      a = g(i.rt) + ", $" + std::to_string(i.rd); break;
    case MFC1: case DMFC1: case MTC1: case DMTC1:
      a = g(i.rt) + ", " + f(i.fs); break;
    case CFC1: case CTC1:
      a = g(i.rt) + ", $" + std::to_string(i.fs); break;
    case BC1F: case BC1T: case BC1FL: case BC1TL:
      a = (i.cc ? "fcc" + std::to_string(i.cc) + ", " : std::string()) + addr(i.branch_target(pc));
      break;
    case FADD: case FSUB: case FMUL: case FDIV:
      m += fmt_suffix(i.fmt);
      a = f(i.fd) + ", " + f(i.fs) + ", " + f(i.ft); break;
    case FMOVZ: case FMOVN:
      m += fmt_suffix(i.fmt);
      a = f(i.fd) + ", " + f(i.fs) + ", " + g(i.ft); break;
    case FMOVCF: case FMOVCT:
      m += fmt_suffix(i.fmt);
      a = f(i.fd) + ", " + f(i.fs) + ", fcc" + std::to_string(i.cc); break;
    case FSQRT: case FABS: case FMOV: case FNEG: case FROUND_L: case FTRUNC_L: case FCEIL_L:
    case FFLOOR_L: case FROUND_W: case FTRUNC_W: case FCEIL_W: case FFLOOR_W: case FRECIP:
    case FRSQRT: case FCVT_S: case FCVT_D: case FCVT_W: case FCVT_L:
      m += fmt_suffix(i.fmt);
      a = f(i.fd) + ", " + f(i.fs); break;
    case FC_COND:
      m = std::string("c.") + kCondNames[i.cond] + fmt_suffix(i.fmt);
      a = (i.cc ? "fcc" + std::to_string(i.cc) + ", " : std::string()) + f(i.fs) + ", " + f(i.ft);
      break;
    case LWXC1: case LDXC1:
      a = f(i.fd) + ", " + g(i.rt) + "(" + g(i.rs) + ")"; break;
    case SWXC1: case SDXC1:
      a = f(i.fs) + ", " + g(i.rt) + "(" + g(i.rs) + ")"; break;
    case PREFX:
      a = hex(i.rd) + ", " + g(i.rt) + "(" + g(i.rs) + ")"; break;
    case FMADD: case FMSUB: case FNMADD: case FNMSUB:
      m += fmt_suffix(i.fmt);
      a = f(i.fd) + ", " + f(i.fr) + ", " + f(i.fs) + ", " + f(i.ft); break;
    case COUNT: break;
  }
  do m += ' '; while (m.size() < 8);
  return m + a;
}

}  // namespace mips
