add_library(mips STATIC
  src/mips_decoder.cpp
  src/mips_disasm.cpp
)
target_include_directories(mips PUBLIC src)

# The recompiler proper, also linked into the launcher.
add_library(recomp_gen STATIC
  src/image.cpp
  src/analysis.cpp
  src/cfg.cpp
  src/codegen.cpp
  src/vegas_gen.cpp
)
target_include_directories(recomp_gen PUBLIC src PRIVATE ${CMAKE_SOURCE_DIR}/runtime/include)
target_link_libraries(recomp_gen PUBLIC mips)

add_executable(nbarecomp src/main.cpp)
target_link_libraries(nbarecomp PRIVATE recomp_gen)
