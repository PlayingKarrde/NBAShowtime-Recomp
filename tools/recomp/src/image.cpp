#include "image.h"

#include <fstream>
#include <iterator>

bool Image::load(const std::string& path, uint32_t base_addr) {
  std::ifstream f(path, std::ios::binary);
  if (!f) return false;
  bytes.assign(std::istreambuf_iterator<char>(f), std::istreambuf_iterator<char>());
  base = base_addr;
  return !bytes.empty();
}

uint32_t Image::read32(uint32_t addr) const {
  if (!contains(addr, 4)) return 0;
  const uint8_t* p = &bytes[addr - base];
  return uint32_t(p[0]) | uint32_t(p[1]) << 8 | uint32_t(p[2]) << 16 | uint32_t(p[3]) << 24;
}
