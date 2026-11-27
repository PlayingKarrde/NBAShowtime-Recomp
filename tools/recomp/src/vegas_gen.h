#pragma once
#include <functional>
#include <string>

// Recompile BIOS + game from an 8 MB entry RAM snapshot into C files in out_dir, plus the
// operator menu program from diag_path (DIAG.EXE as loaded at 0x80100000; empty = none), and
// programs.c listing them for the runtime's exec. Progress and warnings go to log (stdout when
// empty). Returns false on failure.
bool generate_vegas(const std::string& ram_path, const std::string& out_dir,
                    const std::function<void(const std::string&)>& log = {}, const std::string& diag_path = {});
