// Small helpers shared by the launcher: UTF-8 paths, files and child processes.
#pragma once
#include <cstdint>
#include <cstdio>
#include <filesystem>
#include <string>
#include <vector>

namespace fs = std::filesystem;

std::string u8(const fs::path& p);            // path -> UTF-8 string
fs::path from_u8(const std::string& s);       // UTF-8 string -> path
FILE* open_file(const fs::path& p, const char* mode);  // fopen that understands UTF-8 paths on Windows
bool read_file(const fs::path& p, std::vector<uint8_t>& out);
bool write_file(const fs::path& p, const void* data, size_t size);
fs::path launcher_dir();                      // folder holding the launcher executable

// Runs a program, waits for it and collects stdout + stderr. Returns false if it could not start.
bool run_process(const std::vector<std::string>& args, std::string& output, int& exit_code,
                 const fs::path& working_dir = {});
// Starts a program in the background (the game).
bool spawn_detached(const std::vector<std::string>& args, const fs::path& working_dir);
