#pragma once
#include <string>

#include "cfg.h"
#include "image.h"

struct CodegenStats {
  size_t functions = 0, instructions = 0, files = 0;
  size_t unresolved_jr = 0, unhandled_ops = 0;
};

// Emits C sources for every function in `prog` into out_dir:
//   funcs_NNN.c, recomp_funcs.h, func_table.c (recomp_func_table)
// or, for a named unit (another program, e.g. "diag"):
//   diag_funcs_NNN.c, diag_funcs.h, diag_func_table.c (recomp_diag_func_table)
CodegenStats generate_c(const Image& img, const Program& prog, const std::string& out_dir,
                        const std::string& unit = "", size_t funcs_per_file = 120);
