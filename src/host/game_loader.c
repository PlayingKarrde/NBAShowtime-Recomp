#include "game_loader.h"

#include <stdio.h>

#include <SDL3/SDL.h>

#include "recomp.h"

#if defined(_WIN32)
#define GAME_LIB "nbagame.dll"
#elif defined(__APPLE__)
#define GAME_LIB "nbagame.dylib"
#else
#define GAME_LIB "nbagame.so"
#endif

void game_library_default_path(char* out, size_t n) {
  const char* base = SDL_GetBasePath();
  snprintf(out, n, "%s%s", base ? base : "", GAME_LIB);
}

int game_library_load(const char* path, char* why, size_t n) {
  SDL_SharedObject* so = SDL_LoadObject(path);
  if (!so) {
    snprintf(why, n, "The game files are missing (%s).\n\nRun the launcher to build them from your ROM.", path);
    return -1;
  }
  RecompGameInitFn init = (RecompGameInitFn)SDL_LoadFunction(so, RECOMP_GAME_INIT_SYMBOL);
  if (!init) {
    snprintf(why, n, "%s is not an NBA Showtime game library.", path);
    SDL_UnloadObject(so);
    return -1;
  }
  static RecompHostApi api = {
      RECOMP_GAME_ABI, sizeof(RecompHostApi),
#define FIELD(ret, name, args) name,
      RECOMP_HOST_FUNCS(FIELD)
#undef FIELD
  };
  const RecompGameExports* ex = init(&api);
  if (!ex || ex->abi_version != RECOMP_GAME_ABI || ex->codegen_revision != RECOMP_CODEGEN_REVISION ||
      !ex->func_table) {
    snprintf(why, n,
             "This version of NBA Showtime needs its game files rebuilt.\n\n"
             "Run the launcher that came with this version and build into the same folder. "
             "(game files: interface %u, revision %u; this version expects %u, %u)",
             ex ? ex->abi_version : 0, ex ? ex->codegen_revision : 0, RECOMP_GAME_ABI,
             RECOMP_CODEGEN_REVISION);
    SDL_UnloadObject(so);
    return -2;
  }
  recomp_game = ex; /* the library stays loaded for the life of the process */
  return 0;
}
