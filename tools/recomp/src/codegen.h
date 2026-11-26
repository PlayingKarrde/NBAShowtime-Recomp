#pragma once
#include <string>

#include "cfg.h"
#include "image.h"

struct CodegenStats {
  size_t functions = 0, instructions = 0, files = 0;
  size_t unresolved_jr = 0, unhandled_ops = 0;
};

// Emits C sources for every function in `prog` into out_dir:
//   funcs_NNN.c, recomp_funcs.h, func_table.c
CodegenStats generate_c(const Image& img, const Program& prog, const std::string& out_dir,
                        size_t funcs_per_file = 120);
