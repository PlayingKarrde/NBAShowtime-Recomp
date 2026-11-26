// Entry point of the game library (nbagame.so / .dll / .dylib). The launcher compiles this file
// with the recompiled code (generated/*.c) and -DRECOMP_GAME_MODULE; the engine loads the result
// and calls recomp_game_init. See runtime/include/recomp.h.
#ifndef RECOMP_GAME_MODULE
#error "compile with -DRECOMP_GAME_MODULE"
#endif
#include "recomp.h"

#ifdef _WIN32
#define RECOMP_EXPORT __declspec(dllexport)
#else
#define RECOMP_EXPORT __attribute__((visibility("default")))
#endif

/* Shared state lives here, next to the code that uses it on every memory access and loop. */
uint8_t* recomp_rdram;
int32_t recomp_budget;
float recomp_ws_xmin;
RecompHostApi recomp_host;

extern const unsigned recomp_codegen_revision; /* func_table.c */

RECOMP_EXPORT const RecompGameExports* recomp_game_init(const RecompHostApi* host) {
  static RecompGameExports ex;
  ex.abi_version = RECOMP_GAME_ABI;
  ex.codegen_revision = recomp_codegen_revision;
  /* Only take the table from an engine that agrees on its layout; otherwise just report our
     versions so the engine can explain. */
  if (host && host->abi_version == RECOMP_GAME_ABI && host->size == sizeof(RecompHostApi)) {
    recomp_host = *host;
    ex.func_table = recomp_func_table;
    ex.func_count = recomp_func_count;
    ex.rdram = &recomp_rdram;
    ex.budget = &recomp_budget;
    ex.ws_xmin = &recomp_ws_xmin;
  }
  return &ex;
}
