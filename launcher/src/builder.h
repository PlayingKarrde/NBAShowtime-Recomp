// The build pipeline: ROM set -> installed, playable game.
#pragma once
#include <atomic>
#include <mutex>
#include <string>
#include <vector>

#include "romset.h"
#include "util.h"

// What the launcher needs besides the ROM: the prebuilt engine it installs, the headers and glue
// file the game library is compiled with, and Zig. A packaged launcher carries them next to its
// executable (engine/, toolchain/zig/); a developer build falls back to the build and source trees.
struct Toolchain {
  struct File { fs::path src; std::string name; };
  std::vector<File> engine_files;  // engine executable + SDL3, copied into the install folder as `name`
  fs::path include_dir;            // recomp.h, recomp_version.h
  fs::path module_src;             // game_module.c
  fs::path zig;                    // zig executable
  std::string zig_version;
  std::vector<std::string> problems;
  bool ok() const { return problems.empty(); }
};
Toolchain find_toolchain();

struct BuildOptions {
  RomSet roms;
  fs::path install_dir;
  bool keep_build_files = false;  // keep the generated C and objects (install_dir/.build)
  int jobs = 0;                   // parallel compiles (0 = one per CPU)
};

// Shared between the build thread and the UI.
struct BuildState {
  std::mutex m;
  std::vector<std::string> log;
  std::string step;
  float progress = 0;  // 0..1 over the whole build
  std::atomic<bool> running{false}, finished{false}, failed{false}, cancel{false};
  bool echo_stdout = false;  // command-line mode

  void add_log(const std::string& line);
  void set_step(const std::string& s, float p);
  void set_progress(float p);
};

// Runs every step; returns false (with the reason in the log) on failure.
bool run_build(const BuildOptions& opt, const Toolchain& tc, BuildState& st);

fs::path game_executable(const fs::path& install_dir);
fs::path game_library(const fs::path& install_dir);
