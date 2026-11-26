add_library(mips STATIC
  src/mips_decoder.cpp
  src/mips_disasm.cpp
)
target_include_directories(mips PUBLIC src)

add_executable(nbarecomp
  src/main.cpp
  src/image.cpp
  src/analysis.cpp
  src/cfg.cpp
  src/codegen.cpp
)
target_link_libraries(nbarecomp PRIVATE mips)
