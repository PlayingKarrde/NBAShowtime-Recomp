#include "builder.h"

#include <algorithm>
#include <cstring>
#include <ctime>
#include <thread>

#include <SDL3/SDL.h>
#include <libchdr/chd.h>

#include "miniz.h"
#include "vegas_gen.h"

extern "C" {
extern const unsigned char nba_asset_entry_recipe[], nba_asset_entry_regs[], nba_asset_entry_nile[],
    nba_asset_entry_sio[];
extern const size_t nba_asset_entry_recipe_size, nba_asset_entry_regs_size, nba_asset_entry_nile_size,
    nba_asset_entry_sio_size;
}

#if defined(_WIN32)
static const char* kExe = ".exe";
static const char* kGameLib = "nbagame.dll";
#elif defined(__APPLE__)
static const char* kExe = "";
static const char* kGameLib = "nbagame.dylib";
#else
static const char* kExe = "";
static const char* kGameLib = "nbagame.so";
#endif

fs::path game_executable(const fs::path& install_dir) {
  return install_dir / (std::string("nbashowtime") + kExe);
}
fs::path game_library(const fs::path& install_dir) { return install_dir / kGameLib; }

/* ---- BuildState ---- */
void BuildState::add_log(const std::string& line) {
  std::lock_guard<std::mutex> lk(m);
  log.push_back(line);
  if (echo_stdout) std::printf("%s\n", line.c_str()), std::fflush(stdout);
}
void BuildState::set_step(const std::string& s, float p) {
  {
    std::lock_guard<std::mutex> lk(m);
    step = s;
    progress = p;
  }
  add_log("== " + s);
}
void BuildState::set_progress(float p) {
  std::lock_guard<std::mutex> lk(m);
  progress = p;
}

/* ---- toolchain discovery ---- */
Toolchain find_toolchain() {
  Toolchain tc;
  const fs::path base = launcher_dir();
  std::error_code ec;
  const fs::path packaged = base / "engine";
#ifdef NBA_DEV_SOURCE_ROOT
  const fs::path dev = from_u8(NBA_DEV_SOURCE_ROOT);
#else
  const fs::path dev;
#endif
  const std::string exe_name = std::string("nbashowtime") + kExe;

  if (fs::exists(packaged / "bin" / exe_name, ec)) {
    // Packaged: engine/bin holds the engine and its SDL3, installed as they are named.
    for (const auto& e : fs::directory_iterator(packaged / "bin", ec))
      if (e.is_regular_file(ec)) tc.engine_files.push_back({e.path(), u8(e.path().filename())});
    tc.include_dir = packaged / "include";
    tc.module_src = packaged / "module" / "game_module.c";
  } else if (!dev.empty()) {
    tc.include_dir = dev / "runtime" / "include";
    tc.module_src = dev / "runtime" / "module" / "game_module.c";
#ifdef NBA_DEV_ENGINE_EXE
    tc.engine_files.push_back({from_u8(NBA_DEV_ENGINE_EXE), exe_name});
    tc.engine_files.push_back({from_u8(NBA_DEV_SDL3_LIB), NBA_DEV_SDL3_SONAME});
#endif
  }
  bool have_exe = false;
  for (const auto& f : tc.engine_files) have_exe |= f.name == exe_name && fs::exists(f.src, ec);
  if (!have_exe) tc.problems.push_back("the game engine was not found (expected " + u8(packaged / "bin" / exe_name) + ")");
  if (!fs::exists(tc.include_dir / "recomp.h", ec) || !fs::exists(tc.module_src, ec))
    tc.problems.push_back("the game library sources were not found (expected " + u8(packaged / "module") + ")");

  // Zig: bundled, then the developer tree, then PATH.
  const std::string zig_name = std::string("zig") + kExe;
  for (const fs::path& p : {base / "toolchain" / "zig" / zig_name, dev / "toolchain" / "zig" / zig_name}) {
    if (!p.empty() && fs::exists(p, ec)) { tc.zig = p; break; }
  }
  if (tc.zig.empty()) tc.zig = zig_name;  // try PATH
  std::string out;
  int code = -1;
  if (run_process({u8(tc.zig), "version"}, out, code) && code == 0) {
    tc.zig_version = out.substr(0, out.find_first_of("\r\n"));
  } else {
    tc.problems.push_back("Zig compiler not found (expected " + u8(base / "toolchain" / "zig" / zig_name) + ")");
    tc.zig.clear();
  }
  return tc;
}

/* ---- boot snapshot ---- */
namespace {

struct Disk {
  chd_file* chd = nullptr;
  std::vector<uint8_t> hunk;
  int64_t cur = -1;
  uint32_t hunk_bytes = 0;
  uint64_t size = 0;

  bool read(uint64_t off, uint8_t* out, uint64_t len) {
    while (len) {
      const int64_t h = static_cast<int64_t>(off / hunk_bytes);
      if (h != cur) {
        if (chd_read(chd, static_cast<uint32_t>(h), hunk.data()) != CHDERR_NONE) return false;
        cur = h;
      }
      const uint64_t in = off % hunk_bytes;
      const uint64_t n = std::min<uint64_t>(len, hunk_bytes - in);
      std::memcpy(out, hunk.data() + in, n);
      out += n, off += n, len -= n;
    }
    return true;
  }
};

uint32_t le32(const uint8_t* p) { return p[0] | (p[1] << 8) | (p[2] << 16) | (uint32_t(p[3]) << 24); }
uint64_t le64(const uint8_t* p) { return le32(p) | (uint64_t(le32(p + 4)) << 32); }

// Replays assets/boot/entry_recipe.bin (see tools/recipe/make_recipe.py) against the user's ROM.
bool rebuild_snapshot(const std::vector<uint8_t>& bios, const fs::path& chd_path, std::vector<uint8_t>& ram,
                      std::string& err) {
  const uint8_t* r = nba_asset_entry_recipe;
  const size_t rn = nba_asset_entry_recipe_size;
  if (rn < 24 || std::memcmp(r, "NBARCP1\0", 8) != 0) { err = "bad built-in snapshot recipe"; return false; }
  const uint32_t ram_size = le32(r + 8), crc = le32(r + 12), nsegs = le32(r + 16), nlit = le32(r + 20);
  const size_t seg_off = 24, lit_off = seg_off + size_t(nsegs) * 24;
  if (lit_off + nlit != rn) { err = "bad built-in snapshot recipe"; return false; }

  Disk disk;
  FILE* f = open_file(chd_path, "rb");
  if (!f || chd_open_file(f, CHD_OPEN_READ, nullptr, &disk.chd) != CHDERR_NONE) {
    if (f) std::fclose(f);
    err = "cannot open " + u8(chd_path);
    return false;
  }
  disk.hunk_bytes = chd_get_header(disk.chd)->hunkbytes;
  disk.size = chd_get_header(disk.chd)->logicalbytes;
  disk.hunk.resize(disk.hunk_bytes);

  ram.assign(ram_size, 0);
  bool ok = true;
  for (uint32_t i = 0; i < nsegs && ok; ++i) {
    const uint8_t* s = r + seg_off + size_t(i) * 24;
    const uint32_t dst = le32(s), len = le32(s + 4), src = le32(s + 8);
    const uint64_t so = le64(s + 16);
    if (uint64_t(dst) + len > ram_size) { ok = false; break; }
    switch (src) {
      case 1:  // BIOS ROM
        if (so + len > bios.size()) ok = false;
        else std::memcpy(&ram[dst], bios.data() + so, len);
        break;
      case 2:  // disk
        ok = so + len <= disk.size && disk.read(so, &ram[dst], len);
        break;
      case 3:  // literal
        if (so + len > nlit) ok = false;
        else std::memcpy(&ram[dst], r + lit_off + so, len);
        break;
      default: ok = false;
    }
  }
  chd_close(disk.chd);
  std::fclose(f);
  if (!ok) { err = "the snapshot recipe does not fit this ROM set"; return false; }
  const uint32_t got = static_cast<uint32_t>(mz_crc32(MZ_CRC32_INIT, ram.data(), ram.size()));
  if (got != crc) {
    char buf[128];
    std::snprintf(buf, sizeof buf, "rebuilt boot snapshot has CRC %08x, expected %08x", got, crc);
    err = buf;
    return false;
  }
  return true;
}

/* ---- compiling ---- */
struct CompileJob {
  fs::path src, obj;
  std::vector<std::string> flags;
};

std::vector<fs::path> c_files(const fs::path& dir) {
  std::vector<fs::path> v;
  std::error_code ec;
  for (const auto& e : fs::directory_iterator(dir, ec))
    if (e.path().extension() == ".c") v.push_back(e.path());
  std::sort(v.begin(), v.end());
  return v;
}

}  // namespace

bool run_build(const BuildOptions& opt, const Toolchain& tc, BuildState& st) {
  auto fail = [&](const std::string& why) {
    st.add_log("ERROR: " + why);
    st.failed = true;
    return false;
  };
  if (!tc.ok()) return fail(tc.problems.front());
  if (!opt.roms.complete()) return fail("the ROM set is incomplete");
  std::error_code ec;
  const fs::path inst = opt.install_dir;
  const fs::path data = inst / "data";
  const fs::path work = inst / ".build";
  fs::create_directories(data, ec);
  if (ec) return fail("cannot create " + u8(data) + ": " + ec.message());
  fs::remove_all(work, ec);
  fs::create_directories(work, ec);
  st.add_log("Installing to " + u8(inst));
  st.add_log("Compiler: zig " + tc.zig_version + " (" + u8(tc.zig) + ")");

  // 1) ROM data the game reads at run time.
  st.set_step("Copying the hard disk image (nbashowt.chd)", 0.0f);
  std::string err;
  if (!install_chd(opt.roms.chd, data / "nbashowt.chd",
                   [&](uint64_t d, uint64_t t) { st.set_progress(0.15f * float(d) / float(t ? t : 1)); }, err))
    return fail(err);
  if (!write_file(data / "vegassio.bin", opt.roms.sound_data.data(), opt.roms.sound_data.size()))
    return fail("cannot write " + u8(data / "vegassio.bin"));
  if (st.cancel) return fail("cancelled");

  // 2) Boot snapshot (RAM at the game's entry point) + register state.
  st.set_step("Rebuilding the boot snapshot from the ROM", 0.15f);
  std::vector<uint8_t> ram;
  if (!rebuild_snapshot(opt.roms.bios_data, data / "nbashowt.chd", ram, err)) return fail(err);
  if (!write_file(data / "entry_ram.bin", ram.data(), ram.size()) ||
      !write_file(data / "entry_regs.json", nba_asset_entry_regs, nba_asset_entry_regs_size) ||
      !write_file(data / "entry_nile.bin", nba_asset_entry_nile, nba_asset_entry_nile_size) ||
      !write_file(data / "entry_sio.bin", nba_asset_entry_sio, nba_asset_entry_sio_size))
    return fail("cannot write the boot files to " + u8(data));
  st.add_log("boot snapshot verified (8 MB)");

  // 3) Recompile MIPS -> C.
  st.set_step("Recompiling the game to C", 0.18f);
  const fs::path gen = work / "generated";
  if (!generate_vegas(u8(data / "entry_ram.bin"), u8(gen), [&](const std::string& l) { st.add_log(l); }))
    return fail("recompilation failed");
  if (st.cancel) return fail("cancelled");

  // 4) Compile the game library with Zig. Only the recompiled code is built here; the engine
  //    ships prebuilt, so engine updates don't need this step again (see recomp_version.h).
  st.set_step("Compiling", 0.22f);
  const fs::path obj = work / "obj";
  fs::create_directories(obj, ec);
  const std::vector<std::string> flags = {
      "-O2", "-std=gnu11", "-DNDEBUG", "-fno-sanitize=all", "-w", "-ffp-contract=off", "-fno-strict-aliasing",
      "-fPIC", "-fvisibility=hidden", "-DRECOMP_GAME_MODULE", "-I" + u8(tc.include_dir), "-I" + u8(gen)};
  std::vector<CompileJob> jobs;
  // Biggest first, so the long generated files don't end up last.
  std::vector<fs::path> srcs = c_files(gen);
  std::sort(srcs.begin(), srcs.end(), [](const fs::path& a, const fs::path& b) {
    std::error_code e;
    return fs::file_size(a, e) > fs::file_size(b, e);
  });
  srcs.push_back(tc.module_src);
  for (const auto& src : srcs) jobs.push_back({src, obj / (u8(src.stem()) + ".o"), flags});

  const int n_threads = std::max(1, opt.jobs > 0 ? opt.jobs : static_cast<int>(std::thread::hardware_concurrency()));
  std::atomic<size_t> next{0}, done{0};
  std::atomic<bool> bad{false};
  std::mutex err_m;
  std::string first_error;
  auto worker = [&] {
    for (;;) {
      const size_t i = next++;
      if (i >= jobs.size() || bad || st.cancel) return;
      const CompileJob& j = jobs[i];
      std::vector<std::string> args = {u8(tc.zig), "cc"};
      args.insert(args.end(), j.flags.begin(), j.flags.end());
      args.insert(args.end(), {"-c", u8(j.src), "-o", u8(j.obj)});
      std::string out;
      int code = -1;
      const bool started = run_process(args, out, code);
      if (!started || code != 0) {
        std::lock_guard<std::mutex> lk(err_m);
        if (!bad) first_error = u8(j.src.filename()) + ":\n" + out;
        bad = true;
        return;
      }
      const size_t d = ++done;
      st.add_log("compiled " + u8(j.src.filename()) + " (" + std::to_string(d) + "/" + std::to_string(jobs.size()) + ")");
      st.set_progress(0.22f + 0.70f * float(d) / float(jobs.size()));
    }
  };
  st.add_log("compiling " + std::to_string(jobs.size()) + " files with " + std::to_string(n_threads) + " jobs");
  std::vector<std::thread> pool;
  for (int k = 0; k < n_threads; ++k) pool.emplace_back(worker);
  for (auto& t : pool) t.join();
  if (st.cancel) return fail("cancelled");
  if (bad) return fail("compile error in " + first_error);

  // 5) Link the library and install the engine next to it.
  st.set_step("Linking", 0.93f);
  const fs::path lib = game_library(inst);
  std::vector<std::string> link = {u8(tc.zig), "cc", "-shared", "-o", u8(lib)};
  for (const auto& j : jobs) link.push_back(u8(j.obj));
#ifndef _WIN32
  link.push_back("-lm");
#endif
  std::string out;
  int code = -1;
  if (!run_process(link, out, code) || code != 0) return fail("link failed:\n" + out);
  for (const auto& f : tc.engine_files) {
    fs::copy_file(f.src, inst / from_u8(f.name), fs::copy_options::overwrite_existing, ec);
    if (ec) return fail("cannot copy " + u8(f.src) + ": " + ec.message());
  }
  const fs::path exe = game_executable(inst);
#ifndef _WIN32
  fs::permissions(exe, fs::perms::owner_exec | fs::perms::group_exec | fs::perms::others_exec, fs::perm_options::add, ec);
#endif

  // 6) Notes for later.
  {
    char when[64];
    const std::time_t now = std::time(nullptr);
    std::strftime(when, sizeof when, "%Y-%m-%d %H:%M:%S", std::localtime(&now));
    const std::string info = std::string("Built by nbalauncher on ") + when + "\nzig " + tc.zig_version +
                             "\nBIOS: " + opt.roms.bios.where + "\nSound ROM: " + opt.roms.sound.where +
                             "\nDisk: " + opt.roms.disk.where + "\n";
    write_file(data / "build.txt", info.data(), info.size());
  }
  if (!opt.keep_build_files) fs::remove_all(work, ec);
  st.set_step("Done", 1.0f);
  st.add_log("Built " + u8(lib) + " and installed " + u8(exe));
  return true;
}
