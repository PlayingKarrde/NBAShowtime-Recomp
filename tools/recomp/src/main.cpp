// nbarecomp — static analysis / recompilation tool for NBA Showtime (Midway Vegas, MIPS R5000).
//
// Usage:
//   nbarecomp analyze <image.bin> [--base 0x80100000] [--entry 0x80100000]
//                     [--text-end 0x801e5e50] [--out out/analysis]
//   nbarecomp disasm  <image.bin> <addr> [count] [--base 0x80100000]
//   nbarecomp generate <image.bin> [--out generated]   (emit C for every function)
//   nbarecomp generate-vegas <entry_ram.bin> [--out generated]
//       recompile BIOS + game from a full 8 MB RAM snapshot (base 0x80000000)
#include <cstdio>
#include <cstdlib>
#include <set>
#include <string>
#include <vector>

#include "analysis.h"
#include "cfg.h"
#include "codegen.h"
#include "image.h"
#include "mips_disasm.h"

static uint32_t parse_u32(const char* s) { return static_cast<uint32_t>(std::strtoul(s, nullptr, 0)); }

static int usage() {
  std::fprintf(stderr,
               "usage:\n"
               "  nbarecomp analyze <image.bin> [--base A] [--entry A] [--text-end A] [--out DIR]\n"
               "  nbarecomp disasm  <image.bin> <addr> [count] [--base A]\n"
               "  nbarecomp generate <image.bin> [--base A] [--entry A] [--text-end A] [--out DIR]\n"
               "  nbarecomp generate-vegas <entry_ram.bin> [--out DIR]\n");
  return 2;
}

int main(int argc, char** argv) {
  if (argc < 3) return usage();
  const std::string cmd = argv[1];
  const std::string path = argv[2];
  uint32_t base = 0x80100000, entry = 0x80100000, text_end = 0x801E5E50;  // banner strings (rodata) start here
  std::string out = "out/analysis";
  std::vector<std::string> pos;
  for (int k = 3; k < argc; ++k) {
    const std::string a = argv[k];
    if (a == "--base" && k + 1 < argc) base = parse_u32(argv[++k]);
    else if (a == "--entry" && k + 1 < argc) entry = parse_u32(argv[++k]);
    else if (a == "--text-end" && k + 1 < argc) text_end = parse_u32(argv[++k]);
    else if (a == "--out" && k + 1 < argc) out = argv[++k];
    else pos.push_back(a);
  }

  Image img;
  if (!img.load(path, base)) {
    std::fprintf(stderr, "cannot read %s\n", path.c_str());
    return 1;
  }

  if (cmd == "analyze") {
    const Analysis a = analyze(img, entry, text_end);
    write_outputs(img, a, out);
    std::printf("text 0x%08x-0x%08x: %zu functions, %zu invalid words, %zu jalr, %zu jr(non-ra), "
                "%zu escaping branches, %zu external call targets\n",
                a.text_start, a.text_end, a.functions.size(), a.invalid_sites.size(),
                a.jalr_sites.size(), a.jr_sites.size(), a.escaping_branches.size(),
                a.external_calls.size());
    std::printf("wrote %s/{report.txt,functions.csv,game.s}\n", out.c_str());
    return 0;
  }
  if (cmd == "generate-vegas") {
    if (out == "out/analysis") out = "generated";
    // Full RAM snapshot taken at the game's entry point (tools/mame/run_dump.sh).
    if (!img.load(path, 0x80000000)) return 1;
    constexpr uint32_t kBiosText = 0x80004000, kBiosTextEnd = 0x8001BAC0;  // strings follow
    constexpr uint32_t kSyscallTable = 0x8002B3C8, kSyscallCount = 104;
    constexpr uint32_t kGameText = 0x80100000, kGameTextEnd = 0x801E5E50, kGameDataEnd = 0x8035A470;

    std::vector<uint32_t> bios_entries;
    for (uint32_t n = 0; n < kSyscallCount; ++n)
      if (uint32_t h = img.read32(kSyscallTable + 4 * n)) bios_entries.push_back(h);

    struct Sec { const char* name; uint32_t start, end; std::vector<uint32_t> entries; uint32_t ds, de; };
    const Sec secs[] = {
        {"bios", kBiosText, kBiosTextEnd, bios_entries, kBiosTextEnd, kGameText},
        {"game", kGameText, kGameTextEnd, {kGameText}, kGameTextEnd, kGameDataEnd},
    };
    Program all;
    std::set<uint32_t> outside_calls;  // jal targets that land outside both code sections
    auto in_any_text = [&](uint32_t t) {
      for (const Sec& sc : secs) if (t >= sc.start && t < sc.end) return true;
      return false;
    };
    for (const Sec& sc : secs) {
      const Analysis a = analyze_section(img, sc.start, sc.end, sc.entries, sc.ds, sc.de);
      for (const auto& [t, n] : a.external_calls)
        if (!in_any_text(t) && img.contains(t, 4)) outside_calls.insert(t);
      const Program p = build_program(img, a);
      size_t jt = 0, unres = 0, stubs = 0;
      for (const auto& [st, f] : p.funcs) {
        jt += f.jump_tables.size();
        unres += f.unresolved_jr.size();
        stubs += f.syscall_stub;
        for (const auto& w : f.warnings) std::fprintf(stderr, "%s func_%08X: %s\n", sc.name, st, w.c_str());
      }
      std::printf("%s: 0x%08x-0x%08x  %zu functions (%zu from analysis), %zu jump tables, %zu unresolved jr, "
                  "%zu syscall stubs\n", sc.name, sc.start, sc.end, p.funcs.size(), a.functions.size(), jt,
                  unres, stubs);
      all.funcs.insert(p.funcs.begin(), p.funcs.end());
    }
    // Small routines kept in data sections (e.g. the CP0 Count helpers): recompile each block
    // from the call target up to its first `jr ra` + delay slot.
    for (uint32_t t : outside_calls) {
      uint32_t end = t;
      while (end < t + 256 && img.read32(end) != 0x03E00008) end += 4;
      if (img.read32(end) != 0x03E00008) { std::fprintf(stderr, "no return after 0x%08x\n", t); continue; }
      end += 8;
      const Analysis a = analyze_section(img, t, end, {t}, t, t);
      const Program p = build_program(img, a);
      all.funcs.insert(p.funcs.begin(), p.funcs.end());
      std::printf("data-resident routine: 0x%08x-0x%08x\n", t, end);
    }
    const CodegenStats st = generate_c(img, all, out);
    std::printf("emitted %zu functions, %zu instructions into %zu files (%zu unhandled ops) -> %s/\n",
                st.functions, st.instructions, st.files, st.unhandled_ops, out.c_str());
    return 0;
  }
  if (cmd == "generate") {
    if (out == "out/analysis") out = "generated";
    const Analysis a = analyze(img, entry, text_end);
    const Program prog = build_program(img, a);
    size_t jt = 0, unres = 0, warn = 0, stubs = 0, fall = 0;
    for (const auto& [s, f] : prog.funcs) {
      jt += f.jump_tables.size();
      unres += f.unresolved_jr.size();
      warn += f.warnings.size();
      stubs += f.syscall_stub;
      fall += f.falls_through;
      for (const auto& w : f.warnings) std::fprintf(stderr, "func_%08X: %s\n", s, w.c_str());
    }
    const CodegenStats st = generate_c(img, prog, out);
    std::printf("functions: %zu (analysis found %zu, +%zu from gap discovery), syscall stubs: %zu\n",
                prog.funcs.size(), a.functions.size(), prog.funcs.size() - a.functions.size(), stubs);
    std::printf("jump tables recovered: %zu, unresolved jr: %zu, fall-through bodies: %zu, warnings: %zu\n",
                jt, unres, fall, warn);
    std::printf("emitted %zu instructions into %zu files (%zu unhandled ops) -> %s/\n", st.instructions,
                st.files, st.unhandled_ops, out.c_str());
    return 0;
  }
  if (cmd == "disasm") {
    if (pos.empty()) return usage();
    const uint32_t addr = parse_u32(pos[0].c_str());
    const uint32_t count = pos.size() > 1 ? parse_u32(pos[1].c_str()) : 32;
    for (uint32_t k = 0; k < count; ++k) {
      const uint32_t pc = addr + k * 4;
      const uint32_t w = img.read32(pc);
      std::printf("%08x  %08x  %s\n", pc, w, mips::disasm(mips::decode(w), pc).c_str());
    }
    return 0;
  }
  return usage();
}
