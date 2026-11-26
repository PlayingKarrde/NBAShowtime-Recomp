#pragma once
#include <cstdint>
#include <map>
#include <set>
#include <string>
#include <vector>

#include "image.h"
#include "mips_decoder.h"

// Why we believe an address starts a function (bit flags).
enum FuncSource : uint32_t {
  SRC_ENTRY = 1u << 0,     // program entry point
  SRC_JAL = 1u << 1,       // direct call target
  SRC_CODE_PTR = 1u << 2,  // address built in code with lui/addiu (function pointer)
  SRC_DATA_PTR = 1u << 3,  // address stored in the data section, looks like a function start
  SRC_TAIL = 1u << 4,      // target of a `j` that leaves the calling function
};

struct Function {
  uint32_t start = 0;
  uint32_t end = 0;  // exclusive
  uint32_t sources = 0;
  uint32_t call_count = 0;  // static count of direct jal sites targeting it
};

struct Analysis {
  uint32_t text_start = 0, text_end = 0;
  std::map<uint32_t, Function> functions;
  std::map<uint32_t, uint32_t> external_calls;  // jal target outside image -> count
  std::vector<uint32_t> invalid_sites;          // undecodable words inside text
  std::vector<uint32_t> jalr_sites;             // indirect calls
  std::vector<uint32_t> jr_sites;               // indirect jumps (not jr ra): jump tables etc.
  std::set<uint32_t> data_ptrs_into_text;       // all aligned data words that point into text
  std::vector<uint32_t> escaping_branches;      // branches whose target leaves their function
  std::map<mips::Op, uint64_t> op_histogram;
  std::map<uint8_t, uint64_t> fpu_fmt_histogram;
};

Analysis analyze(const Image& img, uint32_t entry, uint32_t text_end);

// Analyze one code section [text_start, text_end) of a larger image. Data pointers into the
// section are searched for in [data_start, data_end) (default: from text_end to image end).
Analysis analyze_section(const Image& img, uint32_t text_start, uint32_t text_end,
                         const std::vector<uint32_t>& entries, uint32_t data_start = 0,
                         uint32_t data_end = 0);

// Writes report.txt, functions.csv and game.s into out_dir.
void write_outputs(const Image& img, const Analysis& a, const std::string& out_dir);
