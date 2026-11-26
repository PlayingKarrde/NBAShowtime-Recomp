// Control-flow recovery: per-function reachable instructions, labels and jump tables.
#pragma once
#include <cstdint>
#include <map>
#include <set>
#include <string>
#include <vector>

#include "analysis.h"
#include "image.h"

struct JumpTable {
  uint32_t jr_pc = 0;
  uint32_t table_addr = 0;
  std::vector<uint32_t> targets;
};

struct FuncCfg {
  uint32_t start = 0;
  uint32_t limit = 0;          // next function start (exclusive bound for this body)
  bool syscall_stub = false;   // body is a single `syscall N`
  std::set<uint32_t> insns;    // reachable instruction addresses (incl. delay slots)
  std::set<uint32_t> labels;   // branch / jump-table targets inside the body
  std::map<uint32_t, JumpTable> jump_tables;   // keyed by jr pc
  std::set<uint32_t> unresolved_jr;            // jr reg (not ra) without a recovered table
  bool falls_through = false;  // execution runs past `limit` into the next function
  std::vector<std::string> warnings;
};

struct Program {
  uint32_t text_start = 0, text_end = 0;
  std::map<uint32_t, FuncCfg> funcs;
};

// Builds CFGs for all functions found by `analyze`, discovering extra functions in
// unreachable gaps (statics only reachable through pointers we missed).
Program build_program(const Image& img, const Analysis& a);
