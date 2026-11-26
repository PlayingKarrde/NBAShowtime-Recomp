#pragma once
#include <string>
#include "mips_decoder.h"

namespace mips {

extern const char* const kGprNames[32];

// Formats an instruction at address pc as text, e.g. "addiu   sp, sp, -0x28".
std::string disasm(const Insn& i, uint32_t pc);

}  // namespace mips
