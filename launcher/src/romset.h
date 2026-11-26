// Finds the NBA Showtime ROM set in what the user picked: the distributed zip (nbashowt.zip +
// nbashowt/nbashowt.chd), a MAME rom folder, or loose files. Files are recognised by content
// (CRC32 / CHD SHA-1), not by name.
#pragma once
#include <cstdint>
#include <functional>
#include <string>
#include <vector>

#include "util.h"

struct RomPart {
  bool found = false;
  std::string where;  // human-readable location
};

struct ChdSource {
  fs::path file;        // a plain .chd file, or
  fs::path zip;         // a .chd entry inside this zip
  uint32_t zip_index = 0;
  uint64_t size = 0;
};

struct RomSet {
  RomPart bios, sound, disk;
  std::vector<uint8_t> bios_data, sound_data;  // small enough to keep in memory
  ChdSource chd;
  std::vector<std::string> notes;  // files that look right but are the wrong version, etc.
  bool complete() const { return bios.found && sound.found && disk.found; }
};

// Expected contents (MAME set "nbashowt", NBA Showtime NBA on NBC ver 2.0, Apr 25 1999).
constexpr uint32_t kBiosCrc = 0xff5d620d, kBiosSize = 524288;   // showtime_mar15_1999.u27
constexpr uint32_t kSoundCrc = 0xd1470e23, kSoundSize = 32768;  // vegassio.bin
extern const uint8_t kChdSha1[20];  // f7c56bc3dcbebc434de58034986179ae01127f87

RomSet scan_romset(const fs::path& input);

// Copies (or unpacks) the CHD to dest, reporting bytes done / total. Skips the copy when dest
// already holds the right CHD of the same size.
bool install_chd(const ChdSource& src, const fs::path& dest,
                 const std::function<void(uint64_t, uint64_t)>& progress, std::string& err);

// Reads the SHA-1 stored in a CHD v5 header (the whole-image hash chdman prints as "SHA1").
bool chd_header_sha1(const uint8_t* header, size_t n, uint8_t sha1[20]);
