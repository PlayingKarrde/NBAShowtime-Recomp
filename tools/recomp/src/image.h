#pragma once
#include <cstdint>
#include <string>
#include <vector>

// A flat memory image of the game executable as loaded in RAM.
struct Image {
  uint32_t base = 0;
  std::vector<uint8_t> bytes;

  bool load(const std::string& path, uint32_t base_addr);
  uint32_t end() const { return base + static_cast<uint32_t>(bytes.size()); }
  bool contains(uint32_t addr, uint32_t size = 1) const {
    return addr >= base && addr - base + size <= bytes.size();
  }
  uint32_t read32(uint32_t addr) const;  // little-endian
};
