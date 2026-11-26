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

# ---- Recompiled game + runtime (needs `nbarecomp generate` to have produced generated/) ----
option(RECOMP_TRACE_CALLS "Record recompiled function entries in a ring buffer (debugging)" OFF)
file(GLOB GENERATED_SOURCES CONFIGURE_DEPENDS ${CMAKE_SOURCE_DIR}/generated/*.c)
if(GENERATED_SOURCES)
  add_library(game_recomp STATIC ${GENERATED_SOURCES})
  target_include_directories(game_recomp PUBLIC runtime/include generated)
  # Guest float semantics: no FMA contraction (R5000 madd.s is unfused), no strict aliasing games.
  target_compile_options(game_recomp PRIVATE -ffp-contract=off -fno-strict-aliasing -Wno-unused-label -Wno-unused-parameter)

  add_library(recomp_runtime STATIC runtime/src/recomp_runtime.c runtime/src/vegas_machine.c runtime/src/ide.c runtime/src/nile.c runtime/src/ioasic.c runtime/src/pci.c runtime/src/banshee.c runtime/src/voodoo3d.c runtime/src/raster_sw.c runtime/src/voodoo_tex.c runtime/src/widescreen.c runtime/src/sio.c runtime/src/nvram.c runtime/src/proc.c runtime/src/adsp2100.c runtime/src/dcs2.c runtime/src/fpu.c)
  target_include_directories(recomp_runtime PUBLIC runtime/include generated)
  target_compile_options(recomp_runtime PRIVATE -ffp-contract=off)

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

  # The game itself: SDL3 window + OpenGL renderer + audio (needs SDL3 development files).
  find_package(SDL3 CONFIG QUIET)
  if(SDL3_FOUND)
    add_executable(nbashowtime src/host/main.c src/host/renderer_gl.c src/host/input.c)
    target_include_directories(nbashowtime PRIVATE runtime/include generated)
    target_link_libraries(nbashowtime PRIVATE recomp_runtime game_recomp recomp_runtime SDL3::SDL3 m)
    set_target_properties(nbashowtime PROPERTIES ENABLE_EXPORTS ON) # symbol names in stall reports
  else()
    message(STATUS "SDL3 not found: the nbashowtime executable is not built")
  endif()
endif()
