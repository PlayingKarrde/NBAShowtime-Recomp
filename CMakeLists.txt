cmake_minimum_required(VERSION 3.20)
project(NBAShowtimeRecomp LANGUAGES C CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_C_STANDARD 11)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

if(NOT CMAKE_BUILD_TYPE)
  set(CMAKE_BUILD_TYPE RelWithDebInfo)
endif()

if(MSVC)
  add_compile_options(/W4)
else()
  add_compile_options(-Wall -Wextra)
endif()

add_subdirectory(tools/recomp)

# libchdr (third_party/libchdr, a git submodule) as one unity build: the game reads its CHD with
# it, and the launcher also uses its bundled miniz for zip archives.
set(CHDR_DIR ${CMAKE_SOURCE_DIR}/third_party/libchdr)
add_library(chdr_unity STATIC ${CMAKE_SOURCE_DIR}/third_party/chdr_unity.c)
target_include_directories(chdr_unity PUBLIC ${CHDR_DIR}/include ${CHDR_DIR}/deps/miniz-3.1.2
                           PRIVATE ${CHDR_DIR}/deps/lzma-26.02/include ${CHDR_DIR}/deps/zstd-1.5.7)
target_compile_definitions(chdr_unity PUBLIC MINIZ_ARCHIVE_APIS MINIZ_STDIO)
if(NOT MSVC)
  target_compile_options(chdr_unity PRIVATE -w)
endif()

find_package(SDL3 CONFIG QUIET)

# ---- Engine, game library and tests ----
# The engine (nbashowtime: runtime + SDL3 host) contains nothing from the ROM. The recompiled game
# is a separate library (nbagame.so / .dll / .dylib) built from generated/ - by the launcher on
# players' machines, or here for development - and loaded at startup. See docs/launcher.md.
option(RECOMP_TRACE_CALLS "Record recompiled function entries in a ring buffer (debugging; links the game statically)" OFF)
file(GLOB GENERATED_SOURCES CONFIGURE_DEPENDS ${CMAKE_SOURCE_DIR}/generated/*.c)
set(RUNTIME_SOURCES runtime/src/recomp_runtime.c runtime/src/vegas_machine.c runtime/src/ide.c runtime/src/nile.c
    runtime/src/ioasic.c runtime/src/pci.c runtime/src/banshee.c runtime/src/voodoo3d.c runtime/src/raster_sw.c
    runtime/src/voodoo_tex.c runtime/src/widescreen.c runtime/src/sio.c runtime/src/nvram.c runtime/src/proc.c
    runtime/src/adsp2100.c runtime/src/dcs2.c runtime/src/fpu.c)
set(HOST_SOURCES src/host/main.c src/host/renderer_gl.c src/host/input.c)
# Guest float semantics: no FMA contraction (R5000 madd.s is unfused), no strict aliasing games.
set(GAME_CODE_OPTIONS -ffp-contract=off -fno-strict-aliasing -Wno-unused-label -Wno-unused-parameter
    -Wno-tautological-compare) # NaN tests (x != x)

if(SDL3_FOUND AND NOT RECOMP_TRACE_CALLS)
  add_library(recomp_runtime_engine STATIC ${RUNTIME_SOURCES})
  target_include_directories(recomp_runtime_engine PUBLIC runtime/include)
  target_compile_definitions(recomp_runtime_engine PUBLIC RECOMP_ENGINE_DYNAMIC)
  target_compile_options(recomp_runtime_engine PRIVATE -ffp-contract=off)
  target_link_libraries(recomp_runtime_engine PUBLIC chdr_unity)

  add_executable(nbashowtime ${HOST_SOURCES} src/host/game_loader.c)
  target_link_libraries(nbashowtime PRIVATE recomp_runtime_engine SDL3::SDL3 m)
  set_target_properties(nbashowtime PROPERTIES ENABLE_EXPORTS ON) # symbol names in stall reports
  if(NOT WIN32 AND NOT APPLE)
    # Installed next to its own copy of SDL3 by the launcher.
    set_target_properties(nbashowtime PROPERTIES BUILD_RPATH "$ORIGIN" INSTALL_RPATH "$ORIGIN")
  endif()
elseif(NOT SDL3_FOUND)
  message(STATUS "SDL3 not found: the nbashowtime executable is not built")
endif()

if(GENERATED_SOURCES)
  # Static build of the recompiled game, for the smoke test and trace builds.
  add_library(game_recomp STATIC ${GENERATED_SOURCES})
  target_include_directories(game_recomp PUBLIC runtime/include generated)
  target_compile_options(game_recomp PRIVATE ${GAME_CODE_OPTIONS})

  add_library(recomp_runtime STATIC ${RUNTIME_SOURCES})
  target_include_directories(recomp_runtime PUBLIC runtime/include generated)
  target_compile_options(recomp_runtime PRIVATE -ffp-contract=off)
  target_link_libraries(recomp_runtime PUBLIC chdr_unity)

  # Standalone: only the I/O ASIC model, no generated code.
  add_executable(ioasic_replay tests/ioasic_replay/ioasic_replay.c runtime/src/ioasic.c
                 runtime/src/dcs2.c runtime/src/adsp2100.c)
  target_include_directories(ioasic_replay PRIVATE runtime/include generated)

  if(RECOMP_TRACE_CALLS)
    target_compile_definitions(game_recomp PUBLIC RECOMP_TRACE_CALLS)
    target_compile_definitions(recomp_runtime PUBLIC RECOMP_TRACE_CALLS)
  endif()

  add_executable(smoke tests/smoke/smoke.c)
  target_link_libraries(smoke PRIVATE recomp_runtime game_recomp recomp_runtime m)
  set_target_properties(smoke PROPERTIES ENABLE_EXPORTS ON) # symbol names in timeout backtraces

  if(NOT RECOMP_TRACE_CALLS)
    # The game library, as the launcher builds it.
    add_library(nbagame MODULE ${GENERATED_SOURCES} runtime/module/game_module.c)
    target_include_directories(nbagame PRIVATE runtime/include generated)
    target_compile_definitions(nbagame PRIVATE RECOMP_GAME_MODULE)
    target_compile_options(nbagame PRIVATE ${GAME_CODE_OPTIONS})
    set_target_properties(nbagame PROPERTIES PREFIX "" C_VISIBILITY_PRESET hidden)
    if(APPLE)
      set_target_properties(nbagame PROPERTIES SUFFIX ".dylib")
    endif()
    target_link_libraries(nbagame PRIVATE m)
    if(TARGET nbashowtime)
      add_dependencies(nbashowtime nbagame) # so `cmake --build` gives a runnable pair
    endif()
  elseif(SDL3_FOUND)
    # Trace builds: one executable with the game linked in (the call ring lives in the runtime).
    add_executable(nbashowtime ${HOST_SOURCES})
    target_compile_definitions(nbashowtime PRIVATE NBA_STATIC_GAME)
    target_link_libraries(nbashowtime PRIVATE recomp_runtime game_recomp recomp_runtime SDL3::SDL3 m)
    set_target_properties(nbashowtime PROPERTIES ENABLE_EXPORTS ON)
  endif()
endif()

# ---- The launcher: builds the game library from the player's ROM (SDL3 + Dear ImGui) ----
option(NBA_BUILD_LAUNCHER "Build the launcher (fetches Dear ImGui)" ON)
if(NBA_BUILD_LAUNCHER AND SDL3_FOUND)
  add_subdirectory(launcher)
endif()
