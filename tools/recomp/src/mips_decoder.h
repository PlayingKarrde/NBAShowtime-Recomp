// MIPS IV (NEC/QED R5000) instruction decoder, little-endian words.
#pragma once
#include <cstdint>
#include <string_view>

namespace mips {

// X(name, mnemonic)
#define MIPS_OPS(X)                                                             \
  X(INVALID, "invalid")                                                         \
  /* SPECIAL */                                                                 \
  X(SLL, "sll") X(SRL, "srl") X(SRA, "sra") X(SLLV, "sllv") X(SRLV, "srlv")     \
  X(SRAV, "srav") X(MOVCI_F, "movf") X(MOVCI_T, "movt") X(JR, "jr")            \
  X(JALR, "jalr") X(MOVZ, "movz") X(MOVN, "movn") X(SYSCALL, "syscall")         \
  X(BREAK, "break") X(SYNC, "sync") X(MFHI, "mfhi") X(MTHI, "mthi")             \
  X(MFLO, "mflo") X(MTLO, "mtlo") X(DSLLV, "dsllv") X(DSRLV, "dsrlv")           \
  X(DSRAV, "dsrav") X(MULT, "mult") X(MULTU, "multu") X(DIV, "div")             \
  X(DIVU, "divu") X(DMULT, "dmult") X(DMULTU, "dmultu") X(DDIV, "ddiv")         \
  X(DDIVU, "ddivu") X(ADD, "add") X(ADDU, "addu") X(SUB, "sub")                 \
  X(SUBU, "subu") X(AND, "and") X(OR, "or") X(XOR, "xor") X(NOR, "nor")         \
  X(SLT, "slt") X(SLTU, "sltu") X(DADD, "dadd") X(DADDU, "daddu")               \
  X(DSUB, "dsub") X(DSUBU, "dsubu") X(TGE, "tge") X(TGEU, "tgeu")               \
  X(TLT, "tlt") X(TLTU, "tltu") X(TEQ, "teq") X(TNE, "tne") X(DSLL, "dsll")     \
  X(DSRL, "dsrl") X(DSRA, "dsra") X(DSLL32, "dsll32") X(DSRL32, "dsrl32")       \
  X(DSRA32, "dsra32")                                                           \
  /* REGIMM */                                                                  \
  X(BLTZ, "bltz") X(BGEZ, "bgez") X(BLTZL, "bltzl") X(BGEZL, "bgezl")           \
  X(TGEI, "tgei") X(TGEIU, "tgeiu") X(TLTI, "tlti") X(TLTIU, "tltiu")           \
  X(TEQI, "teqi") X(TNEI, "tnei") X(BLTZAL, "bltzal") X(BGEZAL, "bgezal")       \
  X(BLTZALL, "bltzall") X(BGEZALL, "bgezall")                                   \
  /* primary */                                                                 \
  X(J, "j") X(JAL, "jal") X(BEQ, "beq") X(BNE, "bne") X(BLEZ, "blez")           \
  X(BGTZ, "bgtz") X(ADDI, "addi") X(ADDIU, "addiu") X(SLTI, "slti")             \
  X(SLTIU, "sltiu") X(ANDI, "andi") X(ORI, "ori") X(XORI, "xori")               \
  X(LUI, "lui") X(BEQL, "beql") X(BNEL, "bnel") X(BLEZL, "blezl")               \
  X(BGTZL, "bgtzl") X(DADDI, "daddi") X(DADDIU, "daddiu") X(LDL, "ldl")         \
  X(LDR, "ldr") X(LB, "lb") X(LH, "lh") X(LWL, "lwl") X(LW, "lw")               \
  X(LBU, "lbu") X(LHU, "lhu") X(LWR, "lwr") X(LWU, "lwu") X(SB, "sb")           \
  X(SH, "sh") X(SWL, "swl") X(SW, "sw") X(SDL, "sdl") X(SDR, "sdr")             \
  X(SWR, "swr") X(CACHE, "cache") X(LL, "ll") X(LWC1, "lwc1") X(PREF, "pref")   \
  X(LLD, "lld") X(LDC1, "ldc1") X(LD, "ld") X(SC, "sc") X(SWC1, "swc1")         \
  X(SCD, "scd") X(SDC1, "sdc1") X(SD, "sd")                                     \
  /* COP0 */                                                                    \
  X(MFC0, "mfc0") X(DMFC0, "dmfc0") X(MTC0, "mtc0") X(DMTC0, "dmtc0")           \
  X(TLBR, "tlbr") X(TLBWI, "tlbwi") X(TLBWR, "tlbwr") X(TLBP, "tlbp")           \
  X(ERET, "eret")                                                               \
  /* COP1 moves / branches */                                                   \
  X(MFC1, "mfc1") X(DMFC1, "dmfc1") X(CFC1, "cfc1") X(MTC1, "mtc1")             \
  X(DMTC1, "dmtc1") X(CTC1, "ctc1") X(BC1F, "bc1f") X(BC1T, "bc1t")             \
  X(BC1FL, "bc1fl") X(BC1TL, "bc1tl")                                           \
  /* COP1 arithmetic (format in Insn::fmt) */                                   \
  X(FADD, "add") X(FSUB, "sub") X(FMUL, "mul") X(FDIV, "div")                   \
  X(FSQRT, "sqrt") X(FABS, "abs") X(FMOV, "mov") X(FNEG, "neg")                 \
  X(FROUND_L, "round.l") X(FTRUNC_L, "trunc.l") X(FCEIL_L, "ceil.l")            \
  X(FFLOOR_L, "floor.l") X(FROUND_W, "round.w") X(FTRUNC_W, "trunc.w")          \
  X(FCEIL_W, "ceil.w") X(FFLOOR_W, "floor.w") X(FMOVCF, "movf")                 \
  X(FMOVCT, "movt") X(FMOVZ, "movz") X(FMOVN, "movn") X(FRECIP, "recip")        \
  X(FRSQRT, "rsqrt") X(FCVT_S, "cvt.s") X(FCVT_D, "cvt.d") X(FCVT_W, "cvt.w")   \
  X(FCVT_L, "cvt.l") X(FC_COND, "c")                                            \
  /* COP1X */                                                                   \
  X(LWXC1, "lwxc1") X(LDXC1, "ldxc1") X(SWXC1, "swxc1") X(SDXC1, "sdxc1")       \
  X(PREFX, "prefx") X(FMADD, "madd") X(FMSUB, "msub") X(FNMADD, "nmadd")        \
  X(FNMSUB, "nmsub")

enum class Op : uint16_t {
#define X(n, s) n,
  MIPS_OPS(X)
#undef X
  COUNT
};

std::string_view op_name(Op op);

// FPU formats
enum Fmt : uint8_t { FMT_NONE = 0, FMT_S = 16, FMT_D = 17, FMT_W = 20, FMT_L = 21 };

struct Insn {
  uint32_t raw = 0;
  Op op = Op::INVALID;
  uint8_t rs = 0, rt = 0, rd = 0, sa = 0;  // GPR fields (rd/sa double as fs/fd)
  uint8_t fs = 0, ft = 0, fd = 0, fr = 0;  // FPR fields
  uint8_t fmt = FMT_NONE;                  // FPU format
  uint8_t cc = 0;                          // FPU condition code (0-7)
  uint8_t cond = 0;                        // c.cond.fmt condition (0-15)
  int16_t simm = 0;
  uint16_t uimm = 0;
  uint32_t target = 0;                     // 26-bit jump index

  // Control flow classification
  bool is_branch() const;          // conditional PC-relative branch
  bool is_likely() const;          // branch-likely (delay slot annulled if not taken)
  bool is_jump() const;            // j/jal/jr/jalr
  bool is_call() const;            // jal/jalr/bal-type (links)
  bool has_delay_slot() const { return is_branch() || is_jump(); }
  bool ends_block() const;         // unconditional transfer (j, jr, b via beq $0,$0)

  // Target helpers (pc = address of this instruction)
  uint32_t branch_target(uint32_t pc) const { return pc + 4 + (int32_t(simm) << 2); }
  uint32_t jump_target(uint32_t pc) const { return ((pc + 4) & 0xF0000000u) | (target << 2); }
};

Insn decode(uint32_t word);

}  // namespace mips
