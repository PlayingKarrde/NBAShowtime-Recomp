#include "romset.h"

#include <cstring>

#include "miniz.h"

const uint8_t kChdSha1[20] = {0xf7, 0xc5, 0x6b, 0xc3, 0xdc, 0xbe, 0xbc, 0x43, 0x4d, 0xe5,
                              0x80, 0x34, 0x98, 0x61, 0x79, 0xae, 0x01, 0x12, 0x7f, 0x87};

namespace {

constexpr size_t kChdV5HeaderSize = 124;

uint32_t be32(const uint8_t* p) { return (uint32_t(p[0]) << 24) | (p[1] << 16) | (p[2] << 8) | p[3]; }

bool ends_with(std::string s, const char* ext) {
  for (auto& c : s) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
  const size_t n = std::strlen(ext);
  return s.size() >= n && s.compare(s.size() - n, n, ext) == 0;
}

struct Scanner {
  RomSet& rs;

  void offer_small(const std::vector<uint8_t>& data, const std::string& where) {
    const uint32_t crc = static_cast<uint32_t>(mz_crc32(MZ_CRC32_INIT, data.data(), data.size()));
    if (!rs.bios.found && data.size() == kBiosSize && crc == kBiosCrc) {
      rs.bios = {true, where};
      rs.bios_data = data;
    } else if (!rs.sound.found && data.size() == kSoundSize && crc == kSoundCrc) {
      rs.sound = {true, where};
      rs.sound_data = data;
    }
  }

  void offer_chd_header(const uint8_t* hdr, size_t n, const ChdSource& src, const std::string& where) {
    if (rs.disk.found) return;
    uint8_t sha[20];
    if (!chd_header_sha1(hdr, n, sha)) {
      rs.notes.push_back(where + ": not a CHD v5 file");
      return;
    }
    if (std::memcmp(sha, kChdSha1, 20) != 0) {
      rs.notes.push_back(where + ": a different disk (this needs NBA Showtime ver 2.0, nbashowt.chd)");
      return;
    }
    rs.disk = {true, where};
    rs.chd = src;
  }

  // Looks at every entry of an open zip. Nested zips (the BIOS set inside the bundle) are read
  // into memory and scanned too.
  void scan_zip(mz_zip_archive* zip, const std::string& where, const fs::path& outer_file, int depth) {
    const mz_uint n = mz_zip_reader_get_num_files(zip);
    for (mz_uint i = 0; i < n; ++i) {
      mz_zip_archive_file_stat st;
      if (!mz_zip_reader_file_stat(zip, i, &st) || st.m_is_directory) continue;
      const std::string name = st.m_filename;
      const std::string w = where + " > " + name;
      if ((st.m_uncomp_size == kBiosSize && st.m_crc32 == kBiosCrc) ||
          (st.m_uncomp_size == kSoundSize && st.m_crc32 == kSoundCrc)) {
        std::vector<uint8_t> data(static_cast<size_t>(st.m_uncomp_size));
        if (mz_zip_reader_extract_to_mem(zip, i, data.data(), data.size(), 0)) offer_small(data, w);
      } else if (ends_with(name, ".zip") && depth < 2 && st.m_uncomp_size < (64u << 20)) {
        std::vector<uint8_t> data(static_cast<size_t>(st.m_uncomp_size));
        if (!mz_zip_reader_extract_to_mem(zip, i, data.data(), data.size(), 0)) continue;
        mz_zip_archive inner{};
        if (mz_zip_reader_init_mem(&inner, data.data(), data.size(), 0)) {
          scan_zip(&inner, w, {}, depth + 1);
          mz_zip_reader_end(&inner);
        }
      } else if (ends_with(name, ".chd")) {
        if (outer_file.empty()) {
          rs.notes.push_back(w + ": a CHD inside a nested zip is not supported; unpack it first");
          continue;
        }
        uint8_t hdr[kChdV5HeaderSize] = {};
        mz_zip_reader_extract_iter_state* it = mz_zip_reader_extract_iter_new(zip, i, 0);
        if (!it) continue;
        const size_t got = mz_zip_reader_extract_iter_read(it, hdr, sizeof hdr);
        mz_zip_reader_extract_iter_free(it);
        ChdSource src;
        src.zip = outer_file;
        src.zip_index = i;
        src.size = st.m_uncomp_size;
        offer_chd_header(hdr, got, src, w);
      }
    }
  }

  void scan_file(const fs::path& p) {
    std::error_code ec;
    const uint64_t size = fs::file_size(p, ec);
    if (ec) return;
    const std::string name = u8(p);
    if (ends_with(name, ".zip")) {
      FILE* f = open_file(p, "rb");
      if (!f) return;
      mz_zip_archive zip{};
      if (mz_zip_reader_init_cfile(&zip, f, 0, 0)) {
        scan_zip(&zip, name, p, 0);
        mz_zip_reader_end(&zip);
      }
      std::fclose(f);
    } else if (ends_with(name, ".chd")) {
      uint8_t hdr[kChdV5HeaderSize] = {};
      FILE* f = open_file(p, "rb");
      if (!f) return;
      const size_t got = std::fread(hdr, 1, sizeof hdr, f);
      std::fclose(f);
      ChdSource src;
      src.file = p;
      src.size = size;
      offer_chd_header(hdr, got, src, name);
    } else if (size == kBiosSize || size == kSoundSize) {
      std::vector<uint8_t> data;
      if (read_file(p, data)) offer_small(data, name);
    }
  }
};

}  // namespace

bool chd_header_sha1(const uint8_t* h, size_t n, uint8_t sha1[20]) {
  if (n < kChdV5HeaderSize || std::memcmp(h, "MComprHD", 8) != 0 || be32(h + 12) != 5) return false;
  std::memcpy(sha1, h + 84, 20);  // v5: rawsha1 @64, sha1 @84, parentsha1 @104
  return true;
}

RomSet scan_romset(const fs::path& input) {
  RomSet rs;
  Scanner sc{rs};
  std::error_code ec;
  if (fs::is_directory(input, ec)) {
    // A MAME rom folder (nbashowt.zip + nbashowt/nbashowt.chd) or any folder holding the files.
    fs::recursive_directory_iterator it(input, fs::directory_options::skip_permission_denied, ec), end;
    for (int seen = 0; !ec && it != end && seen < 20000; it.increment(ec), ++seen) {
      if (it.depth() > 3) { it.disable_recursion_pending(); continue; }
      if (it->is_regular_file(ec)) sc.scan_file(it->path());
      if (rs.complete()) break;
    }
  } else {
    sc.scan_file(input);
  }
  return rs;
}

bool install_chd(const ChdSource& src, const fs::path& dest,
                 const std::function<void(uint64_t, uint64_t)>& progress, std::string& err) {
  std::error_code ec;
  // Already there (an earlier build)?
  if (fs::exists(dest, ec) && fs::file_size(dest, ec) == src.size) {
    uint8_t hdr[kChdV5HeaderSize], sha[20];
    FILE* f = open_file(dest, "rb");
    const size_t got = f ? std::fread(hdr, 1, sizeof hdr, f) : 0;
    if (f) std::fclose(f);
    if (chd_header_sha1(hdr, got, sha) && std::memcmp(sha, kChdSha1, 20) == 0) {
      progress(src.size, src.size);
      return true;
    }
  }
  const fs::path tmp = fs::path(dest).concat(".part");
  FILE* out = open_file(tmp, "wb");
  if (!out) { err = "cannot write " + u8(tmp); return false; }
  std::vector<uint8_t> buf(4u << 20);
  uint64_t done = 0;
  bool ok = true;
  if (!src.file.empty()) {
    FILE* in = open_file(src.file, "rb");
    if (!in) { std::fclose(out); err = "cannot read " + u8(src.file); return false; }
    size_t n;
    while ((n = std::fread(buf.data(), 1, buf.size(), in)) > 0) {
      if (std::fwrite(buf.data(), 1, n, out) != n) { ok = false; err = "write failed (disk full?)"; break; }
      done += n;
      progress(done, src.size);
    }
    std::fclose(in);
  } else {
    FILE* zf = open_file(src.zip, "rb");
    mz_zip_archive zip{};
    if (!zf || !mz_zip_reader_init_cfile(&zip, zf, 0, 0)) {
      if (zf) std::fclose(zf);
      std::fclose(out);
      err = "cannot open " + u8(src.zip);
      return false;
    }
    mz_zip_reader_extract_iter_state* it = mz_zip_reader_extract_iter_new(&zip, src.zip_index, 0);
    if (!it) { ok = false; err = "cannot unpack the CHD from " + u8(src.zip); }
    while (ok) {
      const size_t n = mz_zip_reader_extract_iter_read(it, buf.data(), buf.size());
      if (n == 0) break;
      if (std::fwrite(buf.data(), 1, n, out) != n) { ok = false; err = "write failed (disk full?)"; break; }
      done += n;
      progress(done, src.size);
    }
    if (it && mz_zip_reader_extract_iter_free(it) != MZ_TRUE && ok) {  // also checks the CRC
      ok = false;
      err = "the CHD in " + u8(src.zip) + " is damaged (zip CRC mismatch)";
    }
    mz_zip_reader_end(&zip);
    std::fclose(zf);
  }
  if (std::fclose(out) != 0 && ok) { ok = false; err = "write failed (disk full?)"; }
  if (ok && done != src.size) { ok = false; err = "the CHD was cut short"; }
  if (!ok) { fs::remove(tmp, ec); return false; }
  fs::rename(tmp, dest, ec);
  if (ec) { err = "cannot rename " + u8(tmp) + ": " + ec.message(); return false; }
  return true;
}
