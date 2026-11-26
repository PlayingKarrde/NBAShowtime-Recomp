// Recompile the Vegas BIOS + NBA Showtime from a full 8 MB RAM snapshot taken at the game's entry
// point (base 0x80000000). Shared by `nbarecomp generate-vegas` and the launcher.
#include "vegas_gen.h"

#include <cstdarg>
#include <cstdio>
#include <set>

#include "analysis.h"
#include "cfg.h"
#include "codegen.h"
#include "image.h"

bool generate_vegas(const std::string& ram_path, const std::string& out,
                    const std::function<void(const std::string&)>& log) {
  auto logf = [&](const char* fmt, ...) {
    char buf[1024];
    va_list ap;
    va_start(ap, fmt);
    std::vsnprintf(buf, sizeof buf, fmt, ap);
    va_end(ap);
    if (log) log(buf);
    else std::printf("%s\n", buf);
  };
  Image img;
  if (!img.load(ram_path, 0x80000000)) {
    logf("cannot read %s", ram_path.c_str());
    return false;
  }
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
  // Game process scheduler (main loop 0x801B4270): resumes a process with `jr ra` at
  // 0x801B4760; sleep (0x801B3F98) and exit (0x801B4088) jump back to 0x801B4764.
  constexpr uint32_t kProcSwitchInJr = 0x801B4760, kProcResume = 0x801B4764;
  all.proc_switch_in.insert(kProcSwitchInJr);
  all.proc_resume = kProcResume;
  // Widescreen: the 3D polygon clipper (0x801A2C48) tests screen X against a literal 0; these
  // sites use the runtime left edge instead (recomp_ws_xmin, 0 = original). See
  // runtime/src/widescreen.c for the matching data patches of the right edge.
  {
    const std::string le_x_oow = "set_fcc(ctx, 0, fcmp(fpr_s(ctx, 0), recomp_ws_xmin * fpr_s(ctx, 2), 6));";
    const std::string ge_x = "set_fcc(ctx, 0, fcmp(recomp_ws_xmin, fpr_s(ctx, 0), 6));";
    all.insn_override[0x801A2CD0] = le_x_oow; /* all x*oow <= 0: reject (3 vertices) */
    all.insn_override[0x801A2CE8] = le_x_oow;
    all.insn_override[0x801A2D00] = le_x_oow;
    all.insn_override[0x801A2E04] = ge_x;     /* trivial accept: 0 <= x (3 vertices) */
    all.insn_override[0x801A2E4C] = ge_x;
    all.insn_override[0x801A2E84] = ge_x;
    all.insn_override[0x801A3128] = "mem_w32(ctx, R32(15), recomp_ws_xmin_bits());"; /* left clip edge */
  }
  for (const Sec& sc : secs) {
    const Analysis a = analyze_section(img, sc.start, sc.end, sc.entries, sc.ds, sc.de);
    for (const auto& [t, n] : a.external_calls)
      if (!in_any_text(t) && img.contains(t, 4)) outside_calls.insert(t);
    const Program p = build_program(img, a, {kProcResume});
    size_t jt = 0, unres = 0, stubs = 0;
    for (const auto& [st, f] : p.funcs) {
      jt += f.jump_tables.size();
      unres += f.unresolved_jr.size();
      stubs += f.syscall_stub;
      for (const auto& w : f.warnings) logf("warning: %s func_%08X: %s", sc.name, st, w.c_str());
    }
    logf("%s: 0x%08x-0x%08x  %zu functions (%zu from analysis), %zu jump tables, %zu unresolved jr, "
         "%zu syscall stubs", sc.name, sc.start, sc.end, p.funcs.size(), a.functions.size(), jt, unres, stubs);
    all.funcs.insert(p.funcs.begin(), p.funcs.end());
  }
  // Small routines kept in data sections (e.g. the CP0 Count helpers): recompile each block
  // from the call target up to its first `jr ra` + delay slot.
  for (uint32_t t : outside_calls) {
    uint32_t end = t;
    while (end < t + 256 && img.read32(end) != 0x03E00008) end += 4;
    if (img.read32(end) != 0x03E00008) { logf("warning: no return after 0x%08x", t); continue; }
    end += 8;
    const Analysis a = analyze_section(img, t, end, {t}, t, t);
    const Program p = build_program(img, a);
    all.funcs.insert(p.funcs.begin(), p.funcs.end());
    logf("data-resident routine: 0x%08x-0x%08x", t, end);
  }
  const CodegenStats st = generate_c(img, all, out);
  logf("emitted %zu functions, %zu instructions into %zu files (%zu unhandled ops) -> %s/",
       st.functions, st.instructions, st.files, st.unhandled_ops, out.c_str());
  return st.functions > 0;
}
