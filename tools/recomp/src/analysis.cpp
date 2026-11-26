#include "analysis.h"

#include <algorithm>
#include <cstdio>
#include <filesystem>
#include <fstream>

#include "mips_disasm.h"

using mips::Insn;
using mips::Op;

namespace {

bool in_text(const Analysis& a, uint32_t addr) {
  return addr >= a.text_start && addr < a.text_end && (addr & 3) == 0;
}

// Heuristic: does code at addr look like the start of a function?
bool looks_like_function_start(const Image& img, const Analysis& a, uint32_t addr) {
  if (!in_text(a, addr)) return false;
  const Insn first = mips::decode(img.read32(addr));
  if (first.op == Op::ADDIU && first.rs == 29 && first.rt == 29 && first.simm < 0) return true;
  // Preceded by "jr ra; <delay slot>" or by nop padding after one.
  for (uint32_t back = 8; back <= 32 && addr >= a.text_start + back; back += 4) {
    const uint32_t w = img.read32(addr - back);
    if (w == 0x03E00008) return true;  // jr ra
    if (w != 0 && back > 8) break;
  }
  return false;
}

void add_function(Analysis& a, uint32_t addr, uint32_t src) {
  auto& f = a.functions[addr];
  f.start = addr;
  f.sources |= src;
}

}  // namespace

Analysis analyze(const Image& img, uint32_t entry, uint32_t text_end) {
  Analysis a;
  a.text_start = img.base;
  a.text_end = std::min(text_end, img.end());

  add_function(a, entry, SRC_ENTRY);

  // Pass 1: linear sweep of text.
  std::vector<Insn> insns;
  insns.reserve((a.text_end - a.text_start) / 4);
  for (uint32_t pc = a.text_start; pc < a.text_end; pc += 4) {
    const Insn i = mips::decode(img.read32(pc));
    insns.push_back(i);
    a.op_histogram[i.op]++;
    if (i.fmt != mips::FMT_NONE) a.fpu_fmt_histogram[i.fmt]++;
    if (i.op == Op::INVALID) a.invalid_sites.push_back(pc);
    if (i.op == Op::JAL) {
      const uint32_t t = i.jump_target(pc);
      if (in_text(a, t)) {
        add_function(a, t, SRC_JAL);
        a.functions[t].call_count++;
      } else {
        a.external_calls[t]++;
      }
    }
    if (i.op == Op::JALR) a.jalr_sites.push_back(pc);
    if (i.op == Op::JR && i.rs != 31) a.jr_sites.push_back(pc);
  }
  auto insn_at = [&](uint32_t pc) -> const Insn& { return insns[(pc - a.text_start) / 4]; };

  // Pass 2: function pointers built in code: lui rX, hi ... addiu/ori rY, rX, lo.
  for (uint32_t pc = a.text_start; pc < a.text_end; pc += 4) {
    const Insn& hi = insn_at(pc);
    if (hi.op != Op::LUI) continue;
    for (uint32_t pc2 = pc + 4; pc2 < a.text_end && pc2 <= pc + 32; pc2 += 4) {
      const Insn& lo = insn_at(pc2);
      if ((lo.op == Op::ADDIU || lo.op == Op::ORI) && lo.rs == hi.rt) {
        const uint32_t v = (uint32_t(hi.uimm) << 16) +
                           (lo.op == Op::ADDIU ? uint32_t(int32_t(lo.simm)) : lo.uimm);
        if (looks_like_function_start(img, a, v)) add_function(a, v, SRC_CODE_PTR);
        break;
      }
      if (lo.rt == hi.rt && lo.op != Op::SW && lo.op != Op::LW) break;  // register clobbered
    }
  }

  // Pass 3: pointers into text from the data section.
  for (uint32_t p = a.text_end & ~3u; p + 4 <= img.end(); p += 4) {
    const uint32_t v = img.read32(p);
    if (!in_text(a, v)) continue;
    a.data_ptrs_into_text.insert(v);
    if (looks_like_function_start(img, a, v)) add_function(a, v, SRC_DATA_PTR);
  }

  // Pass 4: tail jumps (j to an address that is itself a known function, or that
  // lands after a jr ra boundary) — iterate until stable.
  for (int iter = 0; iter < 4; ++iter) {
    // compute provisional ends
    for (auto it = a.functions.begin(); it != a.functions.end(); ++it) {
      auto next = std::next(it);
      it->second.end = next == a.functions.end() ? a.text_end : next->first;
    }
    size_t before = a.functions.size();
    for (auto& [start, f] : a.functions) {
      for (uint32_t pc = f.start; pc < f.end; pc += 4) {
        const Insn& i = insn_at(pc);
        if (i.op != Op::J) continue;
        const uint32_t t = i.jump_target(pc);
        if ((t < f.start || t >= f.end) && looks_like_function_start(img, a, t))
          add_function(a, t, SRC_TAIL);
      }
    }
    if (a.functions.size() == before) break;
  }
  for (auto it = a.functions.begin(); it != a.functions.end(); ++it) {
    auto next = std::next(it);
    it->second.end = next == a.functions.end() ? a.text_end : next->first;
  }

  // Pass 5: sanity check — conditional branches should stay inside their function.
  for (const auto& [start, f] : a.functions) {
    for (uint32_t pc = f.start; pc < f.end; pc += 4) {
      const Insn& i = insn_at(pc);
      if (!i.is_branch()) continue;
      const uint32_t t = i.branch_target(pc);
      if (t < f.start || t >= f.end) a.escaping_branches.push_back(pc);
    }
  }
  return a;
}

void write_outputs(const Image& img, const Analysis& a, const std::string& out_dir) {
  namespace fs = std::filesystem;
  fs::create_directories(out_dir);

  // report.txt
  {
    std::ofstream r(fs::path(out_dir) / "report.txt");
    char b[256];
    std::snprintf(b, sizeof b, "text: 0x%08x-0x%08x (%u bytes, %u instructions)\n", a.text_start,
                  a.text_end, a.text_end - a.text_start, (a.text_end - a.text_start) / 4);
    r << b;
    uint32_t by_src[5] = {};
    for (const auto& [s, f] : a.functions)
      for (int k = 0; k < 5; ++k)
        if (f.sources & (1u << k)) by_src[k]++;
    std::snprintf(b, sizeof b,
                  "functions: %zu (entry %u, jal %u, code-ptr %u, data-ptr %u, tail %u)\n",
                  a.functions.size(), by_src[0], by_src[1], by_src[2], by_src[3], by_src[4]);
    r << b;
    std::snprintf(b, sizeof b,
                  "invalid words in text: %zu\nindirect calls (jalr): %zu\nindirect jumps "
                  "(jr non-ra): %zu\nbranches escaping their function: %zu\ndata words "
                  "pointing into text: %zu distinct\n",
                  a.invalid_sites.size(), a.jalr_sites.size(), a.jr_sites.size(),
                  a.escaping_branches.size(), a.data_ptrs_into_text.size());
    r << b;

    r << "\nexternal call targets (outside image):\n";
    for (const auto& [t, n] : a.external_calls) {
      std::snprintf(b, sizeof b, "  0x%08x  x%u\n", t, n);
      r << b;
    }
    r << "\nFPU formats used:";
    for (const auto& [fm, n] : a.fpu_fmt_histogram) {
      const char* nm = fm == mips::FMT_S ? "S" : fm == mips::FMT_D ? "D" : fm == mips::FMT_W ? "W" : "L";
      std::snprintf(b, sizeof b, " %s=%llu", nm, static_cast<unsigned long long>(n));
      r << b;
    }
    r << "\n\nopcode histogram:\n";
    std::vector<std::pair<uint64_t, Op>> hist;
    for (const auto& [op, n] : a.op_histogram) hist.push_back({n, op});
    std::sort(hist.rbegin(), hist.rend());
    for (const auto& [n, op] : hist) {
      std::snprintf(b, sizeof b, "  %-10s %llu\n", std::string(mips::op_name(op)).c_str(),
                    static_cast<unsigned long long>(n));
      r << b;
    }
    r << "\ninvalid sites (first 50):\n";
    for (size_t k = 0; k < a.invalid_sites.size() && k < 50; ++k) {
      std::snprintf(b, sizeof b, "  0x%08x: 0x%08x\n", a.invalid_sites[k],
                    img.read32(a.invalid_sites[k]));
      r << b;
    }
    r << "\nescaping branches (first 50):\n";
    for (size_t k = 0; k < a.escaping_branches.size() && k < 50; ++k) {
      const uint32_t pc = a.escaping_branches[k];
      r << "  " << [&] { std::snprintf(b, sizeof b, "0x%08x: ", pc); return std::string(b); }()
        << mips::disasm(mips::decode(img.read32(pc)), pc) << "\n";
    }
  }

  // functions.csv
  {
    std::ofstream c(fs::path(out_dir) / "functions.csv");
    c << "start,end,size,sources,direct_calls\n";
    char b[128];
    for (const auto& [s, f] : a.functions) {
      std::string src;
      const char* names[] = {"entry", "jal", "codeptr", "dataptr", "tail"};
      for (int k = 0; k < 5; ++k)
        if (f.sources & (1u << k)) src += (src.empty() ? "" : "|") + std::string(names[k]);
      std::snprintf(b, sizeof b, "0x%08x,0x%08x,%u,%s,%u\n", f.start, f.end, f.end - f.start,
                    src.c_str(), f.call_count);
      c << b;
    }
  }

  // game.s
  {
    std::ofstream s(fs::path(out_dir) / "game.s");
    char b[64];
    for (uint32_t pc = a.text_start; pc < a.text_end; pc += 4) {
      if (a.functions.count(pc)) {
        std::snprintf(b, sizeof b, "\nfunc_%08x:\n", pc);
        s << b;
      }
      const uint32_t w = img.read32(pc);
      std::snprintf(b, sizeof b, "  %08x  %08x  ", pc, w);
      s << b << mips::disasm(mips::decode(w), pc) << "\n";
    }
  }
}
