// Recompile the Vegas BIOS + NBA Showtime from a full 8 MB RAM snapshot taken at the game's entry
// point (base 0x80000000), plus the other programs the BIOS can exec at 0x80100000 (the operator
// menu, DIAG.EXE). Shared by `nbarecomp generate-vegas` and the launcher.
#include "vegas_gen.h"

#include <algorithm>
#include <cstdarg>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <set>

#include "analysis.h"
#include "cfg.h"
#include "codegen.h"
#include "image.h"

namespace {

uint32_t crc32_bytes(const uint8_t* p, size_t n) {  // zlib / IEEE CRC-32
  uint32_t c = 0xFFFFFFFFu;
  for (size_t i = 0; i < n; ++i) {
    c ^= p[i];
    for (int k = 0; k < 8; ++k) c = (c >> 1) ^ (0xEDB88320u & (0u - (c & 1u)));
  }
  return ~c;
}

// A program loaded at 0x80100000 (exec'd by the BIOS). Its process scheduler is the same Midway
// code in every program, at different addresses: resume `jr ra`, the address sleep/exit jump
// back to, and the sleep routine (see runtime/src/proc.c).
struct ProgramSpec {
  const char* name;   // unit name: "" = the game (part of the main unit, with the BIOS)
  const char* label;  // for logs and runtime messages
  uint32_t text_end, data_end;
  uint32_t proc_switch_in_jr, proc_resume, proc_sleep;
};
constexpr uint32_t kProgBase = 0x80100000;
const ProgramSpec kGame = {"", "game", 0x801E5E50, 0x8035A470, 0x801B4760, 0x801B4764, 0x801B3F98};
// DIAG.EXE (operator menu and tests, 523,228-byte file): text ends at the first string
// ("I40 board found"), the image (data) at 0x8017FBD4; crt0 clears BSS from 0x8017FBD8.
const ProgramSpec kDiag = {"diag", "diag", 0x801647A0, 0x8017FBD4, 0x80132278, 0x8013227C, 0x80131AB0};

}  // namespace

bool generate_vegas(const std::string& ram_path, const std::string& out,
                    const std::function<void(const std::string&)>& log, const std::string& diag_path) {
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
  // BIOS exec (syscall 45, 0x80016DC0): after loading and checking a program it calls the
  // program's entry with this `jalr s0`. It must switch to that program's recompiled code.
  constexpr uint32_t kExecJalr = 0x80017060;

  std::vector<uint32_t> bios_entries;
  for (uint32_t n = 0; n < kSyscallCount; ++n)
    if (uint32_t h = img.read32(kSyscallTable + 4 * n)) bios_entries.push_back(h);

  // Analyse one code section of `im` and add its functions to `prog`; jal targets outside every
  // code section of that image go to `outside`.
  auto add_section = [&](const Image& im, Program& prog, const char* name, uint32_t start, uint32_t end,
                         std::vector<uint32_t> entries, uint32_t ds, uint32_t de,
                         const std::vector<std::pair<uint32_t, uint32_t>>& texts, std::set<uint32_t>& outside) {
    const Analysis a = analyze_section(im, start, end, entries, ds, de);
    for (const auto& [t, n] : a.external_calls) {
      bool in_text = false;
      for (const auto& [s, e] : texts) in_text |= t >= s && t < e;
      if (!in_text && im.contains(t, 4)) outside.insert(t);
    }
    const Program p = build_program(im, a, {prog.proc_resume});
    size_t jt = 0, unres = 0, stubs = 0;
    for (const auto& [st, f] : p.funcs) {
      jt += f.jump_tables.size();
      unres += f.unresolved_jr.size();
      stubs += f.syscall_stub;
      for (const auto& w : f.warnings) logf("warning: %s func_%08X: %s", name, st, w.c_str());
    }
    logf("%s: 0x%08x-0x%08x  %zu functions (%zu from analysis), %zu jump tables, %zu unresolved jr, "
         "%zu syscall stubs", name, start, end, p.funcs.size(), a.functions.size(), jt, unres, stubs);
    prog.funcs.insert(p.funcs.begin(), p.funcs.end());
  };
  // Small routines kept in data sections (e.g. the CP0 Count helpers): recompile each block
  // from the call target up to its first `jr ra` + delay slot.
  auto add_outside = [&](const Image& im, Program& prog, const std::set<uint32_t>& outside) {
    for (uint32_t t : outside) {
      uint32_t end = t;
      while (end < t + 256 && im.read32(end) != 0x03E00008) end += 4;
      if (im.read32(end) != 0x03E00008) { logf("warning: no return after 0x%08x", t); continue; }
      end += 8;
      const Analysis a = analyze_section(im, t, end, {t}, t, t);
      const Program p = build_program(im, a);
      prog.funcs.insert(p.funcs.begin(), p.funcs.end());
      logf("data-resident routine: 0x%08x-0x%08x", t, end);
    }
  };
  auto set_proc = [](Program& prog, const ProgramSpec& sp) {
    prog.proc_switch_in.insert(sp.proc_switch_in_jr);
    prog.proc_resume = sp.proc_resume;
  };

  // ---- main unit: BIOS + game (from the entry snapshot) ----
  Program all;
  set_proc(all, kGame);
  all.exec_jalr.insert(kExecJalr);
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
  {
    const std::vector<std::pair<uint32_t, uint32_t>> texts = {{kBiosText, kBiosTextEnd}, {kProgBase, kGame.text_end}};
    std::set<uint32_t> outside;
    add_section(img, all, "bios", kBiosText, kBiosTextEnd, bios_entries, kBiosTextEnd, kProgBase, texts, outside);
    add_section(img, all, "game", kProgBase, kGame.text_end, {kProgBase}, kGame.text_end, kGame.data_end, texts,
                outside);
    add_outside(img, all, outside);
  }
  CodegenStats st = generate_c(img, all, out);
  logf("main (BIOS + game): emitted %zu functions, %zu instructions into %zu files (%zu unhandled ops)",
       st.functions, st.instructions, st.files, st.unhandled_ops);
  if (st.functions == 0) return false;

  // ---- other programs (each its own unit; addresses overlap the game's) ----
  struct Emitted { const ProgramSpec* spec; uint32_t text_crc; size_t count; };
  std::vector<Emitted> programs;
  programs.push_back({&kGame, crc32_bytes(&img.bytes[kProgBase - img.base], kGame.text_end - kProgBase),
                      static_cast<size_t>(std::count_if(all.funcs.begin(), all.funcs.end(),
                                                        [](const auto& f) { return f.first >= kProgBase; }))});
  namespace fs = std::filesystem;
  std::error_code ec;
  for (const auto& e : fs::directory_iterator(out, ec))  // stale output of a unit not built now
    if (e.path().filename().string().rfind("diag_", 0) == 0) fs::remove(e.path(), ec);
  if (!diag_path.empty()) {
    Image dimg;
    if (!dimg.load(diag_path, kProgBase) || dimg.end() != kDiag.data_end) {
      logf("%s is not the expected DIAG.EXE image (%zu bytes)", diag_path.c_str(), dimg.bytes.size());
      return false;
    }
    Program diag;
    diag.prefix = "diag_";
    set_proc(diag, kDiag);
    std::set<uint32_t> outside;
    add_section(dimg, diag, "diag", kProgBase, kDiag.text_end, {kProgBase}, kDiag.text_end, kDiag.data_end,
                {{kProgBase, kDiag.text_end}}, outside);
    add_outside(dimg, diag, outside);
    st = generate_c(dimg, diag, out, kDiag.name);
    logf("diag: emitted %zu functions, %zu instructions into %zu files (%zu unhandled ops)", st.functions,
         st.instructions, st.files, st.unhandled_ops);
    if (st.functions == 0) return false;
    programs.push_back({&kDiag, crc32_bytes(dimg.bytes.data(), kDiag.text_end - kProgBase), diag.funcs.size()});
  } else {
    logf("no DIAG.EXE image: the operator menu (test switch) will not work");
  }

  // ---- program list: lets the runtime tell which program exec loaded ----
  {
    std::ofstream p(fs::path(out) / "programs.c");
    p << "/* Generated by nbarecomp. Do not edit. */\n#include \"recomp.h\"\n";
    for (const Emitted& e : programs)
      if (e.spec->name[0])
        p << "extern const RecompFuncEntry recomp_" << e.spec->name << "_func_table[];\n";
    p << "const RecompProgram recomp_programs[] = {\n";
    for (const Emitted& e : programs) {
      char buf[512];
      const std::string table = e.spec->name[0] ? std::string("recomp_") + e.spec->name + "_func_table"
                                                : "recomp_func_table";
      std::snprintf(buf, sizeof buf, "  {\"%s\", 0x%08Xu, 0x%08Xu, 0x%08Xu, 0x%08Xu, %s, %zuu},\n",
                    e.spec->label, kProgBase, e.spec->text_end, e.text_crc, e.spec->proc_sleep, table.c_str(),
                    e.spec->name[0] ? e.count : all.funcs.size());
      p << buf;
    }
    p << "};\nconst unsigned recomp_program_count = " << programs.size() << ";\n";
  }
  logf("%zu programs -> %s/", programs.size(), out.c_str());
  return true;
}
