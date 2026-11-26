# nbalauncher: turns the user's NBA Showtime ROM set into a playable build. It rebuilds the boot
# snapshot from the ROM, recompiles the game to C and compiles it with a bundled Zig toolchain.
cmake_minimum_required(VERSION 3.20)

include(FetchContent)
FetchContent_Declare(imgui
  GIT_REPOSITORY https://github.com/ocornut/imgui.git
  GIT_TAG v1.92.9b
  GIT_SHALLOW TRUE)
FetchContent_MakeAvailable(imgui)

add_library(launcher_imgui STATIC
  ${imgui_SOURCE_DIR}/imgui.cpp
  ${imgui_SOURCE_DIR}/imgui_draw.cpp
  ${imgui_SOURCE_DIR}/imgui_tables.cpp
  ${imgui_SOURCE_DIR}/imgui_widgets.cpp
  ${imgui_SOURCE_DIR}/backends/imgui_impl_sdl3.cpp
  ${imgui_SOURCE_DIR}/backends/imgui_impl_sdlrenderer3.cpp)
target_include_directories(launcher_imgui PUBLIC ${imgui_SOURCE_DIR} ${imgui_SOURCE_DIR}/backends)
target_link_libraries(launcher_imgui PUBLIC SDL3::SDL3)

# Boot data that is not in the ROM: the snapshot recipe and the CPU / chip register state at the
# game's entry point (assets/boot, see tools/recipe/make_recipe.py).
set(BOOT ${CMAKE_SOURCE_DIR}/assets/boot)
set(EMBED_FILES "entry_recipe=${BOOT}/entry_recipe.bin|entry_regs=${BOOT}/entry_regs.json|entry_nile=${BOOT}/entry_nile.bin|entry_sio=${BOOT}/entry_sio.bin")
add_custom_command(
  OUTPUT ${CMAKE_CURRENT_BINARY_DIR}/boot_assets.c
  COMMAND ${CMAKE_COMMAND} -DOUT=${CMAKE_CURRENT_BINARY_DIR}/boot_assets.c "-DFILES=${EMBED_FILES}"
          -P ${CMAKE_CURRENT_SOURCE_DIR}/embed.cmake
  DEPENDS ${CMAKE_CURRENT_SOURCE_DIR}/embed.cmake ${BOOT}/entry_recipe.bin ${BOOT}/entry_regs.json
          ${BOOT}/entry_nile.bin ${BOOT}/entry_sio.bin
  VERBATIM)

add_executable(nbalauncher src/main.cpp src/builder.cpp src/romset.cpp src/util.cpp
               ${CMAKE_CURRENT_BINARY_DIR}/boot_assets.c)
target_link_libraries(nbalauncher PRIVATE launcher_imgui recomp_gen chdr_unity SDL3::SDL3)

# Developer fallbacks, used when the launcher runs from the build tree rather than a packaged
# release: the engine built here (a trace build has none to offer), the headers and glue file
# in the source tree, and the toolchain/ folder.
set(NBA_SDL3_RUNTIME_NAME "$<IF:$<BOOL:${WIN32}>,SDL3.dll,$<TARGET_SONAME_FILE_NAME:SDL3::SDL3>>")
target_compile_definitions(nbalauncher PRIVATE NBA_DEV_SOURCE_ROOT="${CMAKE_SOURCE_DIR}")
set(NBA_HAVE_ENGINE OFF)
if(TARGET nbashowtime AND NOT RECOMP_TRACE_CALLS)
  set(NBA_HAVE_ENGINE ON)
  add_dependencies(nbalauncher nbashowtime)
  target_compile_definitions(nbalauncher PRIVATE
    NBA_DEV_ENGINE_EXE="$<TARGET_FILE:nbashowtime>"
    NBA_DEV_SDL3_LIB="$<TARGET_FILE:SDL3::SDL3>"
    NBA_DEV_SDL3_SONAME="${NBA_SDL3_RUNTIME_NAME}")
else()
  message(STATUS "launcher: no engine in this build tree (RECOMP_TRACE_CALLS is on); "
                 "it will only work from a packaged release")
endif()
if(WIN32)
  set_target_properties(nbalauncher PROPERTIES WIN32_EXECUTABLE ON)
endif()

# ---- Release layout: cmake --install <build> --prefix dist/<platform> (see docs/launcher.md) ----
#   nbalauncher, SDL3            the launcher
#   engine/bin/                  nbashowtime + SDL3, copied into every install
#   engine/include/, module/     what the game library is compiled with
#   toolchain/zig/               the compiler
set(NBA_ZIG_DIR "${CMAKE_SOURCE_DIR}/toolchain/zig" CACHE PATH "Zig toolchain to bundle with the launcher")
set_target_properties(nbalauncher PROPERTIES INSTALL_RPATH "$ORIGIN")
install(TARGETS nbalauncher RUNTIME DESTINATION .)
install(FILES $<TARGET_FILE:SDL3::SDL3> DESTINATION . RENAME ${NBA_SDL3_RUNTIME_NAME})
if(NBA_HAVE_ENGINE)
  install(TARGETS nbashowtime RUNTIME DESTINATION engine/bin)
  install(FILES $<TARGET_FILE:SDL3::SDL3> DESTINATION engine/bin RENAME ${NBA_SDL3_RUNTIME_NAME})
endif()
install(FILES ${CMAKE_SOURCE_DIR}/runtime/include/recomp.h ${CMAKE_SOURCE_DIR}/runtime/include/recomp_version.h
        DESTINATION engine/include)
install(FILES ${CMAKE_SOURCE_DIR}/runtime/module/game_module.c DESTINATION engine/module)
if(EXISTS ${NBA_ZIG_DIR})
  install(DIRECTORY ${NBA_ZIG_DIR}/ DESTINATION toolchain/zig USE_SOURCE_PERMISSIONS)
endif()
