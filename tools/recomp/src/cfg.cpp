#include "cfg.h"

#include <cstdio>
#include <deque>

#include "mips_decoder.h"

using mips::Insn;
using mips::Op;

namespace {

std::string hex32(uint32_t v) {
  char b[16];
  std::snprintf(b, sizeof b, "0x%08x", v);
  return b;
}

// Does `i` write GPR r? (approximate, for backward pattern matching)
int gpr_written(const Insn& i) {
  switch (i.op) {
    case Op::LUI: case Op::ADDIU: case Op::ADDI: case Op::ORI: case Op::ANDI: case Op::XORI:
    case Op::SLTI: case Op::SLTIU: case Op::LW: case Op::LH: case Op::LHU: case Op::LB:
    case Op::LBU: case Op::LWL: case Op::LWR: case Op::DADDIU: case Op::MFC1: case Op::MFC0:
    case Op::CFC1: case Op::LL: case Op::LD:
      return i.rt;
    case Op::ADDU: case Op::ADD: case Op::SUBU: case Op::SUB: case Op::AND: case Op::OR:
    case Op::XOR: case Op::NOR: case Op::SLT: case Op::SLTU: case Op::SLL: case Op::SRL:
    case Op::SRA: case Op::SLLV: case Op::SRLV: case Op::SRAV: case Op::MFHI: case Op::MFLO:
    case Op::MOVZ: case Op::MOVN: case Op::DADDU: case Op::JALR:
      return i.rd;
    case Op::JAL:
      return 31;
    default:
      return -1;
  }
}

// Recover a gcc switch table feeding `jr rs` at jr_pc.
//   sltiu at, rI, N ; beqz at, default ; sll rI, rI, 2 ; lui at, %hi(T) ;
//   addu at, at, rI ; lw rX, %lo(T)(at) ; jr rX
bool recover_jump_table(const Image& img, uint32_t fstart, uint32_t limit, uint32_t jr_pc,
                        JumpTable& out) {
  const Insn jr = mips::decode(img.read32(jr_pc));
  const uint32_t lo_bound = jr_pc >= fstart + 4 * 24 ? jr_pc - 4 * 24 : fstart;

  // 1) find the lw that produces the jump register
  uint32_t lw_pc = 0;
  Insn lw;
  for (uint32_t pc = jr_pc - 4; pc >= lo_bound && pc >= fstart; pc -= 4) {
    const Insn i = mips::decode(img.read32(pc));
    if (gpr_written(i) == jr.rs) {
      if (i.op == Op::LW) { lw_pc = pc; lw = i; }
      break;
    }
    if (pc == fstart) break;
  }
  if (!lw_pc) return false;

  // 2) find lui feeding the lw base (directly, or through one addu)
  int base = lw.rs;
  uint32_t hi = 0;
  bool have_hi = false;
  std::set<int> base_regs = {base};
  for (uint32_t pc = lw_pc - 4; pc >= lo_bound && pc >= fstart; pc -= 4) {
    const Insn i = mips::decode(img.read32(pc));
    const int w = gpr_written(i);
    if (w >= 0 && base_regs.count(w)) {
      if (i.op == Op::LUI) { hi = uint32_t(i.uimm) << 16; have_hi = true; break; }
      if (i.op == Op::ADDU) { base_regs.erase(w); base_regs.insert(i.rs); base_regs.insert(i.rt); continue; }
      break;
    }
    if (pc == fstart) break;
  }
  if (!have_hi) return false;
  out.jr_pc = jr_pc;
  out.table_addr = hi + uint32_t(int32_t(lw.simm));

  // 3) entry count from a preceding sltiu bound check, if any
  uint32_t count = 0;
  for (uint32_t pc = lw_pc - 4; pc >= lo_bound && pc >= fstart; pc -= 4) {
    const Insn i = mips::decode(img.read32(pc));
    if (i.op == Op::SLTIU) { count = i.uimm; break; }
    if (pc == fstart) break;
  }
  const uint32_t max_entries = count ? count : 512;
  for (uint32_t k = 0; k < max_entries; ++k) {
    const uint32_t ea = out.table_addr + k * 4;
    if (!img.contains(ea, 4)) break;
    const uint32_t t = img.read32(ea);
    if (t < fstart || t >= limit || (t & 3)) {
      if (count) return false;  // a bounded table must be fully in range
      break;
    }
    out.targets.push_back(t);
  }
  return !out.targets.empty();
}

// Walks the body of a function starting at `start`, bounded by `limit`.
// Returns false if the walk hits an invalid instruction (body is not real code).
bool walk(const Image& img, FuncCfg& f) {
  std::deque<uint32_t> work = {f.start};
  bool ok = true;
  auto in_body = [&](uint32_t a) { return a >= f.start && a < f.limit; };

  const Insn first = mips::decode(img.read32(f.start));
  if (first.op == Op::SYSCALL) {
    f.syscall_stub = true;
    f.insns.insert(f.start);
    return true;
  }

  while (!work.empty()) {
    uint32_t pc = work.front();
    work.pop_front();
    while (true) {
      if (!in_body(pc)) {
        if (pc == f.limit) f.falls_through = true;
        break;
      }
      if (f.insns.count(pc)) break;
      const Insn i = mips::decode(img.read32(pc));
      f.insns.insert(pc);
      if (i.op == Op::INVALID) {
        f.warnings.push_back("invalid instruction at " + hex32(pc));
        ok = false;
        break;
      }
      if (i.op == Op::ERET) break;
      if (i.has_delay_slot()) {
        f.insns.insert(pc + 4);
        if (i.is_branch()) {
          const uint32_t t = i.branch_target(pc);
          if (in_body(t)) { f.labels.insert(t); work.push_back(t); }
          const bool always = (i.op == Op::BEQ && i.rs == 0 && i.rt == 0) ||
                              (i.op == Op::BGEZ && i.rs == 0);
          if (always) break;
          if (i.is_likely()) f.labels.insert(pc + 8);  // not-taken path skips the slot
          pc += 8;
          continue;
        }
        if (i.op == Op::J) {
          const uint32_t t = i.jump_target(pc);
          if (in_body(t)) { f.labels.insert(t); work.push_back(t); }
          break;
        }
        if (i.op == Op::JR) {
          if (i.rs != 31) {
            JumpTable jt;
            if (recover_jump_table(img, f.start, f.limit, pc, jt)) {
              for (uint32_t t : jt.targets) { f.labels.insert(t); work.push_back(t); }
              f.jump_tables[pc] = std::move(jt);
            } else {
              f.unresolved_jr.insert(pc);
            }
          }
          break;
        }
        // jal / jalr: call returns to pc+8
        pc += 8;
        continue;
      }
      pc += 4;
    }
  }
  // A branch into a delay slot needs the following instruction labelled too.
  for (uint32_t l : std::set<uint32_t>(f.labels)) {
    if (f.insns.count(l - 4)) {
      const Insn prev = mips::decode(img.read32(l - 4));
      if (prev.has_delay_slot()) f.labels.insert(l + 4);
    }
  }
  return ok;
}

}  // namespace

Program build_program(const Image& img, const Analysis& a) {
  Program p;
  p.text_start = a.text_start;
  p.text_end = a.text_end;
  std::set<uint32_t> starts;
  for (const auto& [s, f] : a.functions) starts.insert(s);

  // Iterate: build CFGs; look for code in the unreachable tail of each body.
  for (int round = 0; round < 8; ++round) {
    p.funcs.clear();
    std::set<uint32_t> discovered;
    for (auto it = starts.begin(); it != starts.end(); ++it) {
      FuncCfg f;
      f.start = *it;
      auto next = std::next(it);
      f.limit = next == starts.end() ? a.text_end : *next;
      walk(img, f);

      // Unreachable tail: first non-zero word after the last reachable instruction.
      if (!f.syscall_stub) {
        uint32_t last = f.insns.empty() ? f.start : *f.insns.rbegin() + 4;
        while (last < f.limit && img.read32(last) == 0) last += 4;
        if (last < f.limit && !f.falls_through) {
          FuncCfg trial;
          trial.start = last;
          trial.limit = f.limit;
          if (walk(img, trial) && !trial.insns.empty() && !trial.falls_through)
            discovered.insert(last);
        }
      } else if (f.start + 4 < f.limit &&
                 mips::decode(img.read32(f.start + 4)).op == Op::SYSCALL) {
        discovered.insert(f.start + 4);  // next stub in the syscall table
      }
      p.funcs[f.start] = std::move(f);
    }
    size_t before = starts.size();
    starts.insert(discovered.begin(), discovered.end());
    if (starts.size() == before) break;
  }
  return p;
}
