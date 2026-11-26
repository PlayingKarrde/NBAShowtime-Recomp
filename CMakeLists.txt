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
file(GLOB GENERATED_SOURCES CONFIGURE_DEPENDS ${CMAKE_SOURCE_DIR}/generated/*.c)
if(GENERATED_SOURCES)
  add_library(game_recomp STATIC ${GENERATED_SOURCES})
  target_include_directories(game_recomp PUBLIC runtime/include generated)
  # Guest float semantics: no FMA contraction (R5000 madd.s is unfused), no strict aliasing games.
  target_compile_options(game_recomp PRIVATE -ffp-contract=off -fno-strict-aliasing -Wno-unused-label -Wno-unused-parameter)

  add_library(recomp_runtime STATIC runtime/src/recomp_runtime.c)
  target_include_directories(recomp_runtime PUBLIC runtime/include generated)
  target_compile_options(recomp_runtime PRIVATE -ffp-contract=off)

  add_executable(smoke tests/smoke/smoke.c)
  target_link_libraries(smoke PRIVATE recomp_runtime game_recomp recomp_runtime m)
endif()
