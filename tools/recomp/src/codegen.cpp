#include "codegen.h"

#include <cstdarg>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <sstream>

#include "mips_decoder.h"
#include "mips_disasm.h"

using mips::Insn;
using mips::Op;

namespace {

std::string fmt(const char* f, ...) __attribute__((format(printf, 1, 2)));
std::string fmt(const char* f, ...) {
  char buf[512];
  va_list ap;
  va_start(ap, f);
  std::vsnprintf(buf, sizeof buf, f, ap);
  va_end(ap);
  return buf;
}

std::string fname(uint32_t a) { return fmt("func_%08X", a); }
std::string lbl(uint32_t a) { return fmt("L_%08X", a); }
std::string r(int n) { return n == 0 ? std::string("0") : fmt("ctx->r[%d]", n); }
std::string r32(int n) { return n == 0 ? std::string("0u") : fmt("R32(%d)", n); }
std::string rs32(int n) { return n == 0 ? std::string("0") : fmt("RS32(%d)", n); }
std::string ea(const Insn& i) { return fmt("(%s + 0x%xu)", r32(i.rs).c_str(), uint32_t(int32_t(i.simm))); }

// Assignment to a GPR; writes to $zero are dropped (but side effects in `expr` are kept).
std::string set(int rd, const std::string& expr) {
  if (rd == 0) return "(void)(" + expr + ");";
  return fmt("ctx->r[%d] = %s;", rd, expr.c_str());
}
std::string set32(int rd, const std::string& expr) { return set(rd, "SE32(" + expr + ")"); }

const char* fp_get(uint8_t f) { return f == mips::FMT_D ? "fpr_d" : "fpr_s"; }
const char* fp_set(uint8_t f) { return f == mips::FMT_D ? "set_fpr_d" : "set_fpr_s"; }
const char* fp_type(uint8_t f) { return f == mips::FMT_D ? "double" : "float"; }

struct Gen {
  const Image& img;
  const Program& prog;
  CodegenStats& stats;
  const FuncCfg* f = nullptr;
  std::ostringstream out;

  explicit Gen(const Image& i, const Program& p, CodegenStats& s) : img(i), prog(p), stats(s) {}

  bool is_func(uint32_t a) const { return prog.funcs.count(a) != 0; }

  // Code to transfer control to `t` leaving the current function (tail call).
  std::string tail_to(uint32_t t) {
    // Always braced: this is used after `if (c)` / `else`.
    if (is_func(t)) return "{ " + fname(t) + "(ctx); return; }";
    return fmt("{ recomp_lookup(ctx, 0x%08Xu)(ctx); return; }", t);
  }
  uint32_t cur_pc = 0;  // address of the branch being emitted

  // Code to transfer control to `t` (inside: goto; outside: tail call). Backward gotos are loop
  // edges and charge the loop body to the time budget (see RECOMP_LOOP in recomp.h).
  std::string go(uint32_t t) {
    if (f->insns.count(t) && t >= f->start && t < f->limit) {
      if (t <= cur_pc) return fmt("{ RECOMP_LOOP(%u); goto %s; }", (cur_pc - t) / 4 + 2, lbl(t).c_str());
      return "goto " + lbl(t) + ";";
    }
    return tail_to(t);
  }
  std::string call(uint32_t t) {
    if (is_func(t)) return fname(t) + "(ctx);";
    return fmt("recomp_lookup(ctx, 0x%08Xu)(ctx);", t);
  }

  std::string branch_cond(const Insn& i) {
    switch (i.op) {
      case Op::BEQ: case Op::BEQL: return fmt("%s == %s", r(i.rs).c_str(), r(i.rt).c_str());
      case Op::BNE: case Op::BNEL: return fmt("%s != %s", r(i.rs).c_str(), r(i.rt).c_str());
      case Op::BLEZ: case Op::BLEZL: return fmt("(int64_t)%s <= 0", r(i.rs).c_str());
      case Op::BGTZ: case Op::BGTZL: return fmt("(int64_t)%s > 0", r(i.rs).c_str());
      case Op::BLTZ: case Op::BLTZL: case Op::BLTZAL: case Op::BLTZALL:
        return fmt("(int64_t)%s < 0", r(i.rs).c_str());
      case Op::BGEZ: case Op::BGEZL: case Op::BGEZAL: case Op::BGEZALL:
        return fmt("(int64_t)%s >= 0", r(i.rs).c_str());
      case Op::BC1F: case Op::BC1FL: return fmt("!get_fcc(ctx, %d)", i.cc);
      case Op::BC1T: case Op::BC1TL: return fmt("get_fcc(ctx, %d)", i.cc);
      default: return "0";
    }
  }

  // Straight-line (non control flow) instruction.
  std::string simple(const Insn& i, uint32_t pc) {
    const uint32_t simm = uint32_t(int32_t(i.simm));
    switch (i.op) {
      // shifts
      case Op::SLL: if (i.raw == 0) return ""; return set32(i.rd, fmt("%s << %d", r32(i.rt).c_str(), i.sa));
      case Op::SRL: return set32(i.rd, fmt("%s >> %d", r32(i.rt).c_str(), i.sa));
      case Op::SRA: return set32(i.rd, fmt("%s >> %d", rs32(i.rt).c_str(), i.sa));
      case Op::SLLV: return set32(i.rd, fmt("%s << (%s & 31)", r32(i.rt).c_str(), r32(i.rs).c_str()));
      case Op::SRLV: return set32(i.rd, fmt("%s >> (%s & 31)", r32(i.rt).c_str(), r32(i.rs).c_str()));
      case Op::SRAV: return set32(i.rd, fmt("%s >> (%s & 31)", rs32(i.rt).c_str(), r32(i.rs).c_str()));
      case Op::DSLL: return set(i.rd, fmt("%s << %d", r(i.rt).c_str(), i.sa));
      case Op::DSRL: return set(i.rd, fmt("%s >> %d", r(i.rt).c_str(), i.sa));
      case Op::DSRA: return set(i.rd, fmt("(uint64_t)((int64_t)%s >> %d)", r(i.rt).c_str(), i.sa));
      case Op::DSLL32: return set(i.rd, fmt("%s << %d", r(i.rt).c_str(), i.sa + 32));
      case Op::DSRL32: return set(i.rd, fmt("%s >> %d", r(i.rt).c_str(), i.sa + 32));
      case Op::DSRA32: return set(i.rd, fmt("(uint64_t)((int64_t)%s >> %d)", r(i.rt).c_str(), i.sa + 32));
      case Op::DSLLV: return set(i.rd, fmt("%s << (%s & 63)", r(i.rt).c_str(), r(i.rs).c_str()));
      case Op::DSRLV: return set(i.rd, fmt("%s >> (%s & 63)", r(i.rt).c_str(), r(i.rs).c_str()));
      case Op::DSRAV: return set(i.rd, fmt("(uint64_t)((int64_t)%s >> (%s & 63))", r(i.rt).c_str(), r(i.rs).c_str()));
      // ALU
      case Op::ADD: case Op::ADDU: return set32(i.rd, fmt("%s + %s", r32(i.rs).c_str(), r32(i.rt).c_str()));
      case Op::SUB: case Op::SUBU: return set32(i.rd, fmt("%s - %s", r32(i.rs).c_str(), r32(i.rt).c_str()));
      case Op::DADD: case Op::DADDU: return set(i.rd, fmt("%s + %s", r(i.rs).c_str(), r(i.rt).c_str()));
      case Op::DSUB: case Op::DSUBU: return set(i.rd, fmt("%s - %s", r(i.rs).c_str(), r(i.rt).c_str()));
      case Op::AND: return set(i.rd, fmt("%s & %s", r(i.rs).c_str(), r(i.rt).c_str()));
      case Op::OR: return set(i.rd, fmt("%s | %s", r(i.rs).c_str(), r(i.rt).c_str()));
      case Op::XOR: return set(i.rd, fmt("%s ^ %s", r(i.rs).c_str(), r(i.rt).c_str()));
      case Op::NOR: return set(i.rd, fmt("~(%s | %s)", r(i.rs).c_str(), r(i.rt).c_str()));
      case Op::SLT: return set(i.rd, fmt("(int64_t)%s < (int64_t)%s", r(i.rs).c_str(), r(i.rt).c_str()));
      case Op::SLTU: return set(i.rd, fmt("(uint64_t)%s < (uint64_t)%s", r(i.rs).c_str(), r(i.rt).c_str()));
      case Op::MOVZ: return i.rd ? fmt("if (%s == 0) %s", r(i.rt).c_str(), set(i.rd, r(i.rs)).c_str()) : "";
      case Op::MOVN: return i.rd ? fmt("if (%s != 0) %s", r(i.rt).c_str(), set(i.rd, r(i.rs)).c_str()) : "";
      case Op::MOVCI_F: return i.rd ? fmt("if (!get_fcc(ctx, %d)) %s", i.cc, set(i.rd, r(i.rs)).c_str()) : "";
      case Op::MOVCI_T: return i.rd ? fmt("if (get_fcc(ctx, %d)) %s", i.cc, set(i.rd, r(i.rs)).c_str()) : "";
      case Op::ADDI: case Op::ADDIU: return set32(i.rt, fmt("%s + 0x%xu", r32(i.rs).c_str(), simm));
      case Op::DADDI: case Op::DADDIU: return set(i.rt, fmt("%s + (uint64_t)(int64_t)%d", r(i.rs).c_str(), i.simm));
      case Op::SLTI: return set(i.rt, fmt("(int64_t)%s < (int64_t)%d", r(i.rs).c_str(), i.simm));
      case Op::SLTIU: return set(i.rt, fmt("(uint64_t)%s < (uint64_t)(int64_t)%d", r(i.rs).c_str(), i.simm));
      case Op::ANDI: return set(i.rt, fmt("%s & 0x%xu", r(i.rs).c_str(), i.uimm));
      case Op::ORI: return set(i.rt, fmt("%s | 0x%xu", r(i.rs).c_str(), i.uimm));
      case Op::XORI: return set(i.rt, fmt("%s ^ 0x%xu", r(i.rs).c_str(), i.uimm));
      case Op::LUI: return set(i.rt, fmt("0x%016llxull", (unsigned long long)SE32v(uint32_t(i.uimm) << 16)));
      // HI/LO
      case Op::MFHI: return set(i.rd, "ctx->hi");
      case Op::MFLO: return set(i.rd, "ctx->lo");
      case Op::MTHI: return fmt("ctx->hi = %s;", r(i.rs).c_str());
      case Op::MTLO: return fmt("ctx->lo = %s;", r(i.rs).c_str());
      case Op::MULT:
        return fmt("{ int64_t p = (int64_t)%s * (int64_t)%s; ctx->lo = SE32(p); ctx->hi = SE32(p >> 32); }",
                   rs32(i.rs).c_str(), rs32(i.rt).c_str());
      case Op::MULTU:
        return fmt("{ uint64_t p = (uint64_t)%s * (uint64_t)%s; ctx->lo = SE32(p); ctx->hi = SE32(p >> 32); }",
                   r32(i.rs).c_str(), r32(i.rt).c_str());
      case Op::DIV: return fmt("do_div(ctx, %s, %s);", rs32(i.rs).c_str(), rs32(i.rt).c_str());
      case Op::DIVU: return fmt("do_divu(ctx, %s, %s);", r32(i.rs).c_str(), r32(i.rt).c_str());
      case Op::DMULT: case Op::DMULTU: case Op::DDIV: case Op::DDIVU:
        stats.unhandled_ops++;
        return fmt("recomp_unhandled(ctx, 0x%08Xu, \"%s\");", pc, std::string(mips::op_name(i.op)).c_str());
      // loads / stores
      case Op::LB: return set(i.rt, fmt("(uint64_t)(int64_t)(int8_t)mem_r8(ctx, %s)", ea(i).c_str()));
      case Op::LBU: return set(i.rt, fmt("mem_r8(ctx, %s)", ea(i).c_str()));
      case Op::LH: return set(i.rt, fmt("(uint64_t)(int64_t)(int16_t)mem_r16(ctx, %s)", ea(i).c_str()));
      case Op::LHU: return set(i.rt, fmt("mem_r16(ctx, %s)", ea(i).c_str()));
      case Op::LW: case Op::LL: return set32(i.rt, fmt("mem_r32(ctx, %s)", ea(i).c_str()));
      case Op::LWU: return set(i.rt, fmt("mem_r32(ctx, %s)", ea(i).c_str()));
      case Op::LD: case Op::LLD: return set(i.rt, fmt("mem_r64(ctx, %s)", ea(i).c_str()));
      case Op::LWL: return set32(i.rt, fmt("do_lwl(ctx, %s, %s)", ea(i).c_str(), r32(i.rt).c_str()));
      case Op::LWR: return set32(i.rt, fmt("do_lwr(ctx, %s, %s)", ea(i).c_str(), r32(i.rt).c_str()));
      case Op::SB: return fmt("mem_w8(ctx, %s, (uint8_t)%s);", ea(i).c_str(), r(i.rt).c_str());
      case Op::SH: return fmt("mem_w16(ctx, %s, (uint16_t)%s);", ea(i).c_str(), r(i.rt).c_str());
      case Op::SW: return fmt("mem_w32(ctx, %s, %s);", ea(i).c_str(), r32(i.rt).c_str());
      case Op::SD: return fmt("mem_w64(ctx, %s, %s);", ea(i).c_str(), r(i.rt).c_str());
      case Op::SC: return fmt("mem_w32(ctx, %s, %s); %s", ea(i).c_str(), r32(i.rt).c_str(), set(i.rt, "1").c_str());
      case Op::SCD: return fmt("mem_w64(ctx, %s, %s); %s", ea(i).c_str(), r(i.rt).c_str(), set(i.rt, "1").c_str());
      case Op::SWL: return fmt("do_swl(ctx, %s, %s);", ea(i).c_str(), r32(i.rt).c_str());
      case Op::SWR: return fmt("do_swr(ctx, %s, %s);", ea(i).c_str(), r32(i.rt).c_str());
      case Op::LDL: case Op::LDR: case Op::SDL: case Op::SDR:
        stats.unhandled_ops++;
        return fmt("recomp_unhandled(ctx, 0x%08Xu, \"%s\");", pc, std::string(mips::op_name(i.op)).c_str());
      case Op::LWC1: return fmt("ctx->f[%d] = mem_r32(ctx, %s);", i.ft, ea(i).c_str());
      case Op::SWC1: return fmt("mem_w32(ctx, %s, ctx->f[%d]);", ea(i).c_str(), i.ft);
      case Op::LDC1: return fmt("set_fpr_l(ctx, %d, mem_r64(ctx, %s));", i.ft, ea(i).c_str());
      case Op::SDC1: return fmt("mem_w64(ctx, %s, fpr_l(ctx, %d));", ea(i).c_str(), i.ft);
      case Op::LWXC1: return fmt("ctx->f[%d] = mem_r32(ctx, %s + %s);", i.fd, r32(i.rs).c_str(), r32(i.rt).c_str());
      case Op::SWXC1: return fmt("mem_w32(ctx, %s + %s, ctx->f[%d]);", r32(i.rs).c_str(), r32(i.rt).c_str(), i.fs);
      case Op::LDXC1: return fmt("set_fpr_l(ctx, %d, mem_r64(ctx, %s + %s));", i.fd, r32(i.rs).c_str(), r32(i.rt).c_str());
      case Op::SDXC1: return fmt("mem_w64(ctx, %s + %s, fpr_l(ctx, %d));", r32(i.rs).c_str(), r32(i.rt).c_str(), i.fs);
      case Op::CACHE: case Op::PREF: case Op::PREFX: case Op::SYNC: return "";
      // system
      case Op::SYSCALL: return fmt("recomp_syscall(ctx, 0x%xu, 0x%08Xu);", (i.raw >> 6) & 0xFFFFF, pc);
      case Op::BREAK: return fmt("recomp_break(ctx, 0x%xu, 0x%08Xu);", (i.raw >> 6) & 0xFFFFF, pc);
      case Op::TGE: return fmt("if ((int64_t)%s >= (int64_t)%s) recomp_trap(ctx, 0x%08Xu);", r(i.rs).c_str(), r(i.rt).c_str(), pc);
      case Op::TGEU: return fmt("if (%s >= %s) recomp_trap(ctx, 0x%08Xu);", r(i.rs).c_str(), r(i.rt).c_str(), pc);
      case Op::TLT: return fmt("if ((int64_t)%s < (int64_t)%s) recomp_trap(ctx, 0x%08Xu);", r(i.rs).c_str(), r(i.rt).c_str(), pc);
      case Op::TLTU: return fmt("if (%s < %s) recomp_trap(ctx, 0x%08Xu);", r(i.rs).c_str(), r(i.rt).c_str(), pc);
      case Op::TEQ: return fmt("if (%s == %s) recomp_trap(ctx, 0x%08Xu);", r(i.rs).c_str(), r(i.rt).c_str(), pc);
      case Op::TNE: return fmt("if (%s != %s) recomp_trap(ctx, 0x%08Xu);", r(i.rs).c_str(), r(i.rt).c_str(), pc);
      case Op::TGEI: case Op::TGEIU: case Op::TLTI: case Op::TLTIU: case Op::TEQI: case Op::TNEI:
        stats.unhandled_ops++;
        return fmt("recomp_unhandled(ctx, 0x%08Xu, \"trap-imm\");", pc);
      case Op::MFC0: case Op::DMFC0: return set(i.rt, fmt("recomp_cop0_read(ctx, %d, 0x%08Xu)", i.rd, pc));
      case Op::MTC0: case Op::DMTC0: return fmt("recomp_cop0_write(ctx, %d, %s, 0x%08Xu);", i.rd, r(i.rt).c_str(), pc);
      case Op::TLBR: case Op::TLBWI: case Op::TLBWR: case Op::TLBP: case Op::ERET:
        stats.unhandled_ops++;
        return fmt("recomp_unhandled(ctx, 0x%08Xu, \"%s\");", pc, std::string(mips::op_name(i.op)).c_str());
      // FPU moves
      case Op::MFC1: return set32(i.rt, fmt("ctx->f[%d]", i.fs));
      case Op::MTC1: return fmt("ctx->f[%d] = %s;", i.fs, r32(i.rt).c_str());
      case Op::DMFC1: return set(i.rt, fmt("fpr_l(ctx, %d)", i.fs));
      case Op::DMTC1: return fmt("set_fpr_l(ctx, %d, %s);", i.fs, r(i.rt).c_str());
      case Op::CFC1: return set32(i.rt, i.fs == 31 ? std::string("ctx->fcsr") : std::string("0x2310u"));
      case Op::CTC1: return i.fs == 31 ? fmt("recomp_set_fcsr(ctx, %s);", r32(i.rt).c_str()) : "";
      // FPU arithmetic
      // Arithmetic: the result goes through a temporary; a non-finite one is checked against the
      // FCSR exception enables (an enabled Invalid/Divide-by-zero traps into the BIOS handler).
      case Op::FADD: case Op::FSUB: case Op::FMUL: case Op::FDIV: {
        const char* o = i.op == Op::FADD ? "+" : i.op == Op::FSUB ? "-" : i.op == Op::FMUL ? "*" : "/";
        return fp_checked(i, pc, fmt("_a %s _b", o), i.op == Op::FDIV ? "RECOMP_FPE_DIV" : "RECOMP_FPE_GENERIC",
                          fmt("%s(ctx, %d)", fp_get(i.fmt), i.fs), fmt("%s(ctx, %d)", fp_get(i.fmt), i.ft), "0");
      }
      case Op::FSQRT:
        return fp_checked(i, pc, i.fmt == mips::FMT_D ? "sqrt(_a)" : "sqrtf(_a)", "RECOMP_FPE_SQRT",
                          fmt("%s(ctx, %d)", fp_get(i.fmt), i.fs), "0", "0");
      case Op::FABS: return fmt("%s(ctx, %d, %s(%s(ctx, %d)));", fp_set(i.fmt), i.fd,
                                i.fmt == mips::FMT_D ? "fabs" : "fabsf", fp_get(i.fmt), i.fs);
      case Op::FNEG: return fmt("%s(ctx, %d, -%s(ctx, %d));", fp_set(i.fmt), i.fd, fp_get(i.fmt), i.fs);
      case Op::FMOV:
        return i.fmt == mips::FMT_D ? fmt("set_fpr_l(ctx, %d, fpr_l(ctx, %d));", i.fd, i.fs)
                                    : fmt("ctx->f[%d] = ctx->f[%d];", i.fd, i.fs);
      case Op::FMOVZ: case Op::FMOVN: case Op::FMOVCF: case Op::FMOVCT: {
        std::string c = i.op == Op::FMOVZ ? r(i.ft) + " == 0" : i.op == Op::FMOVN ? r(i.ft) + " != 0"
                      : i.op == Op::FMOVCF ? fmt("!get_fcc(ctx, %d)", i.cc) : fmt("get_fcc(ctx, %d)", i.cc);
        std::string mv = i.fmt == mips::FMT_D ? fmt("set_fpr_l(ctx, %d, fpr_l(ctx, %d));", i.fd, i.fs)
                                              : fmt("ctx->f[%d] = ctx->f[%d];", i.fd, i.fs);
        return "if (" + c + ") " + mv;
      }
      case Op::FRECIP:
        return fp_checked(i, pc, fmt("(%s)1 / _a", fp_type(i.fmt)), "RECOMP_FPE_RECIP",
                          fmt("%s(ctx, %d)", fp_get(i.fmt), i.fs), "0", "0");
      case Op::FRSQRT:
        return fp_checked(i, pc, fmt("(%s)1 / %s(_a)", fp_type(i.fmt), i.fmt == mips::FMT_D ? "sqrt" : "sqrtf"),
                          "RECOMP_FPE_RSQRT", fmt("%s(ctx, %d)", fp_get(i.fmt), i.fs), "0", "0");
      case Op::FROUND_W: case Op::FTRUNC_W: case Op::FCEIL_W: case Op::FFLOOR_W: case Op::FCVT_W: {
        int mode = i.op == Op::FROUND_W ? 0 : i.op == Op::FTRUNC_W ? 1 : i.op == Op::FCEIL_W ? 2
                 : i.op == Op::FFLOOR_W ? 3 : -1;
        return fmt("{ double _v = %s(ctx, %d); if (RECOMP_UNLIKELY(recomp_cvt_w_bad(_v)) && "
                   "recomp_fpe_invalid(ctx, 0x%08Xu)) {} else ctx->f[%d] = (uint32_t)recomp_cvt_w(_v, %d); }",
                   fp_get(i.fmt), i.fs, pc, i.fd, mode);
      }
      case Op::FROUND_L: case Op::FTRUNC_L: case Op::FCEIL_L: case Op::FFLOOR_L: case Op::FCVT_L: {
        int mode = i.op == Op::FROUND_L ? 0 : i.op == Op::FTRUNC_L ? 1 : i.op == Op::FCEIL_L ? 2
                 : i.op == Op::FFLOOR_L ? 3 : -1;
        return fmt("{ double _v = %s(ctx, %d); if (RECOMP_UNLIKELY(recomp_cvt_l_bad(_v)) && "
                   "recomp_fpe_invalid(ctx, 0x%08Xu)) {} else set_fpr_l(ctx, %d, (uint64_t)recomp_cvt_l(_v, %d)); }",
                   fp_get(i.fmt), i.fs, pc, i.fd, mode);
      }
      case Op::FCVT_S: case Op::FCVT_D: {
        std::string src;
        if (i.fmt == mips::FMT_S) src = fmt("fpr_s(ctx, %d)", i.fs);
        else if (i.fmt == mips::FMT_D) src = fmt("fpr_d(ctx, %d)", i.fs);
        else if (i.fmt == mips::FMT_W) src = fmt("(int32_t)ctx->f[%d]", i.fs);
        else src = fmt("(int64_t)fpr_l(ctx, %d)", i.fs);
        return i.op == Op::FCVT_S ? fmt("set_fpr_s(ctx, %d, (float)%s);", i.fd, src.c_str())
                                  : fmt("set_fpr_d(ctx, %d, (double)%s);", i.fd, src.c_str());
      }
      case Op::FC_COND:
        if (!(i.cond & 8))
          return fmt("set_fcc(ctx, %d, fcmp(%s(ctx, %d), %s(ctx, %d), %d));", i.cc, fp_get(i.fmt), i.fs,
                     fp_get(i.fmt), i.ft, i.cond & 7);
        // Signalling compares (c.sf .. c.ngt, incl. c.lt / c.le) raise Invalid on unordered operands.
        return fmt("{ %s _a = %s(ctx, %d), _b = %s(ctx, %d); if (RECOMP_UNLIKELY(_a != _a || _b != _b) && "
                   "recomp_fpe_invalid(ctx, 0x%08Xu)) {} else set_fcc(ctx, %d, fcmp(_a, _b, %d)); }",
                   fp_type(i.fmt), fp_get(i.fmt), i.fs, fp_get(i.fmt), i.ft, pc, i.cc, i.cond & 7);
      case Op::FMADD: case Op::FMSUB: case Op::FNMADD: case Op::FNMSUB: {
        // Unfused: product rounded to format precision before the add (compiled with -ffp-contract=off).
        const char* t = fp_type(i.fmt);
        std::string prod = fmt("(%s)(%s(ctx, %d) * %s(ctx, %d))", t, fp_get(i.fmt), i.fs, fp_get(i.fmt), i.ft);
        std::string fr = fmt("%s(ctx, %d)", fp_get(i.fmt), i.fr);
        std::string e = i.op == Op::FMADD ? prod + " + " + fr : i.op == Op::FMSUB ? prod + " - " + fr
                      : i.op == Op::FNMADD ? "-(" + prod + " + " + fr + ")" : "-(" + prod + " - " + fr + ")";
        (void)t;
        return fp_checked(i, pc, fmt("(%s)(%s)", t, e.c_str()), "RECOMP_FPE_GENERIC",
                          fmt("%s(ctx, %d)", fp_get(i.fmt), i.fs), fmt("%s(ctx, %d)", fp_get(i.fmt), i.ft),
                          fmt("%s(ctx, %d)", fp_get(i.fmt), i.fr));
      }
      default:
        stats.unhandled_ops++;
        return fmt("recomp_unhandled(ctx, 0x%08Xu, \"%s\");", pc, std::string(mips::op_name(i.op)).c_str());
    }
  }

  // `expr` uses _a/_b/_c (the operands, read before fd is written) and yields the result.
  std::string fp_checked(const mips::Insn& i, uint32_t pc, const std::string& expr, const char* kind,
                         const std::string& a, const std::string& b, const std::string& c) {
    const char* t = fp_type(i.fmt);
    return fmt("{ %s _a = %s, _b = %s, _c = %s; %s _r = %s; (void)_b; (void)_c; "
               "if (RECOMP_UNLIKELY(!isfinite(_r)) && recomp_fpe_arith(ctx, 0x%08Xu, %s, _a, _b, _c, _r)) {} "
               "else %s(ctx, %d, _r); }",
               t, a.c_str(), b.c_str(), c.c_str(), t, expr.c_str(), pc, kind, fp_set(i.fmt), i.fd);
  }

  static uint64_t SE32v(uint32_t v) { return uint64_t(int64_t(int32_t(v))); }

  void emit_function(const FuncCfg& fc) {
    f = &fc;
    out << "\nvoid " << fname(fc.start) << "(RecompContext* ctx) {\n";
    out << fmt("  RECOMP_ENTER(0x%08Xu);\n", fc.start);
    if (fc.syscall_stub) {
      const Insn i = mips::decode(img.read32(fc.start));
      out << fmt("  recomp_syscall(ctx, 0x%xu, 0x%08Xu);\n}\n", (i.raw >> 6) & 0xFFFFF, fc.start);
      stats.functions++;
      stats.instructions++;
      return;
    }
    for (const auto& w : fc.warnings) out << "  /* warning: " << w << " */\n";

    uint32_t skip_until = 0;  // delay slot already emitted with its branch
    for (auto it = fc.insns.begin(); it != fc.insns.end(); ++it) {
      const uint32_t pc = *it;
      const bool labelled = fc.labels.count(pc) != 0;
      if (pc < skip_until) continue;  // delay slot: emitted together with its branch
      // Gap in the reachable set: control never falls into the next instruction directly.
      if (labelled) out << lbl(pc) << ":;\n";
      const Insn i = mips::decode(img.read32(pc));
      stats.instructions++;
      out << "  /* " << fmt("%08X", pc) << ": " << mips::disasm(i, pc) << " */\n";

      if (!i.has_delay_slot()) {
        std::string s = simple(i, pc);
        if (!s.empty()) out << "  " << s << "\n";
        // Falling off the end of the body into the next function.
        auto nx = std::next(it);
        if (fc.falls_through && (nx == fc.insns.end()) && pc + 4 == fc.limit)
          out << "  " << tail_to(fc.limit) << "\n";
        continue;
      }

      cur_pc = pc;
      const uint32_t ds_pc = pc + 4;
      const Insn ds = mips::decode(img.read32(ds_pc));
      std::string slot = simple(ds, ds_pc);
      std::string slot_comment = "/* " + fmt("%08X", ds_pc) + ": " + mips::disasm(ds, ds_pc) + " */ ";
      skip_until = pc + 8;

      if (i.is_branch()) {
        const uint32_t t = i.branch_target(pc);
        const bool link = i.op == Op::BLTZAL || i.op == Op::BGEZAL || i.op == Op::BLTZALL || i.op == Op::BGEZALL;
        out << "  {\n    int c = " << branch_cond(i) << ";\n";
        if (link) out << "    ctx->r[31] = " << fmt("0x%016llxull", (unsigned long long)SE32v(pc + 8)) << ";\n";
        if (i.is_likely()) {
          out << "    if (c) { " << slot_comment << slot << " "
              << (link ? call(t) + " " + go(pc + 8) : go(t)) << " }\n";
          out << "    else " << go(pc + 8) << "\n  }\n";
        } else {
          out << "    " << slot_comment << slot << "\n";
          if (link) out << "    if (c) " << call(t) << "\n";
          else out << "    if (c) " << go(t) << "\n";
          out << "  }\n";
        }
      } else if (i.op == Op::J && prog.proc_resume && i.jump_target(pc) == prog.proc_resume &&
                 !(prog.proc_resume >= fc.start && prog.proc_resume < fc.limit)) {
        // process sleep/exit jumping back into the scheduler: switch fibers
        out << "  " << slot_comment << slot << "\n  "
            << fmt("recomp_proc_switch_out(ctx, 0x%08Xu); return;", fc.start) << "\n";
      } else if (i.op == Op::J) {
        out << "  " << slot_comment << slot << "\n  " << go(i.jump_target(pc)) << "\n";
      } else if (i.op == Op::JAL) {
        out << "  ctx->r[31] = " << fmt("0x%016llxull", (unsigned long long)SE32v(pc + 8)) << ";\n";
        out << "  " << slot_comment << slot << "\n  " << call(i.jump_target(pc)) << "\n";
      } else if (i.op == Op::JALR) {
        out << "  {\n    uint32_t t = " << r32(i.rs) << ";\n";
        if (i.rd) out << "    ctx->r[" << int(i.rd) << "] = " << fmt("0x%016llxull", (unsigned long long)SE32v(pc + 8)) << ";\n";
        out << "    " << slot_comment << slot << "\n    recomp_lookup(ctx, t)(ctx);\n  }\n";
      } else if (i.op == Op::JR) {
        if (i.rs == 31 && prog.proc_switch_in.count(pc)) {
          // scheduler resuming a game process: run it on its fiber, then continue the loop
          out << "  " << slot_comment << slot << "\n  recomp_proc_switch_in(ctx);\n  "
              << go(prog.proc_resume) << "\n";
        } else if (i.rs == 31) {
          out << "  " << slot_comment << slot << "\n  return;\n";
        } else if (auto jt = fc.jump_tables.find(pc); jt != fc.jump_tables.end()) {
          out << "  {\n    uint32_t t = " << r32(i.rs) << ";\n    " << slot_comment << slot << "\n    switch (t) {\n";
          std::set<uint32_t> seen;
          for (uint32_t tg : jt->second.targets)
            if (seen.insert(tg).second) out << fmt("      case 0x%08Xu: goto %s;\n", tg, lbl(tg).c_str());
          out << "      default: recomp_lookup(ctx, t)(ctx); return;\n    }\n  }\n";
        } else {
          stats.unresolved_jr++;
          out << "  {\n    uint32_t t = " << r32(i.rs) << ";\n    " << slot_comment << slot
              << "\n    recomp_lookup(ctx, t)(ctx); return;\n  }\n";
        }
      }

      // Not-taken path continues at pc+8; if that isn't part of the body (e.g. the
      // branch is the last thing before the next function), tail into it.
      if (!i.ends_block() && i.op != Op::JR && !fc.insns.count(pc + 8)) {
        if (pc + 8 == fc.limit) out << "  " << tail_to(pc + 8) << "\n";
        else out << "  " << fmt("recomp_unhandled(ctx, 0x%08Xu, \"fell off body\");", pc + 8) << "\n  return;\n";
      }
      // A label on the delay slot itself (branch into a delay slot).
      if (fc.labels.count(ds_pc)) {
        if (!i.ends_block() && i.op != Op::JR) out << "  " << go(pc + 8) << "\n";
        out << lbl(ds_pc) << ":;\n  " << slot << "\n  " << go(pc + 8) << "\n";
      }
    }
    // Labels referenced but never emitted (shouldn't happen) are caught by the C compiler.
    out << "}\n";
    stats.functions++;
  }
};

}  // namespace

CodegenStats generate_c(const Image& img, const Program& prog, const std::string& out_dir,
                        size_t funcs_per_file) {
  namespace fs = std::filesystem;
  fs::create_directories(out_dir);
  CodegenStats stats;

  const char* prelude =
      "/* Generated by nbarecomp. Do not edit. */\n"
      "#include <math.h>\n#include \"recomp.h\"\n#include \"recomp_funcs.h\"\n"
      "#pragma GCC diagnostic ignored \"-Wunused-label\"\n";

  // Header with prototypes.
  {
    std::ofstream h(fs::path(out_dir) / "recomp_funcs.h");
    h << "/* Generated by nbarecomp. Do not edit. */\n#pragma once\n#include \"recomp.h\"\n";
    h << "#ifdef __cplusplus\nextern \"C\" {\n#endif\n";
    for (const auto& [a, fc] : prog.funcs) h << "void " << fname(a) << "(RecompContext* ctx);\n";
    h << "typedef struct { uint32_t addr; RecompFunc fn; } RecompFuncEntry;\n";
    h << "extern const RecompFuncEntry recomp_func_table[];\nextern const unsigned recomp_func_count;\n";
    h << "#ifdef __cplusplus\n}\n#endif\n";
  }
  // Sorted address -> function table for indirect calls.
  {
    std::ofstream t(fs::path(out_dir) / "func_table.c");
    t << "/* Generated by nbarecomp. Do not edit. */\n#include \"recomp_funcs.h\"\n";
    t << "const RecompFuncEntry recomp_func_table[] = {\n";
    for (const auto& [a, fc] : prog.funcs) t << fmt("  {0x%08Xu, %s},\n", a, fname(a).c_str());
    t << "};\nconst unsigned recomp_func_count = " << prog.funcs.size() << ";\n";
  }
  // Function bodies.
  size_t n = 0, file_idx = 0;
  std::ofstream cur;
  for (const auto& [a, fc] : prog.funcs) {
    if (n % funcs_per_file == 0) {
      if (cur.is_open()) cur.close();
      cur.open(fs::path(out_dir) / fmt("funcs_%03zu.c", file_idx++));
      cur << prelude;
      stats.files++;
    }
    Gen g(img, prog, stats);
    g.emit_function(fc);
    cur << g.out.str();
    ++n;
  }
  return stats;
}
