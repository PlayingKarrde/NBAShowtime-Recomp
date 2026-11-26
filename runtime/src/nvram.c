// Battery-backed NVRAM / timekeeper at physical 0x04400000 (32 KB, byte wide). The game keeps
// CMOS data there (audits, adjustments, high scores; see syscalls 59-75). The top 8 bytes are
// the real-time clock (control, seconds, minutes, hours, day, date, month, year; BCD).
#include "nvram.h"

#include <stdio.h>
#include <string.h>
#include <time.h>

#include "vegas.h"

#define RTC_BASE (NVRAM_SIZE - 8)

static struct {
  uint8_t mem[NVRAM_SIZE];
  char path[1024];
  int dirty;
} nv;

static uint8_t bcd(int v) { return (uint8_t)(((v / 10) << 4) | (v % 10)); }

void nvram_init(const char* path) {
  memset(&nv, 0, sizeof nv);
  if (path) {
    snprintf(nv.path, sizeof nv.path, "%s", path);
    FILE* f = fopen(path, "rb");
    if (f) {
      if (fread(nv.mem, 1, NVRAM_SIZE, f) != NVRAM_SIZE) vegas_log("nvram: short file %s", path);
      fclose(f);
    }
  }
}

void nvram_save(void) {
  if (!nv.path[0] || !nv.dirty) return;
  FILE* f = fopen(nv.path, "wb");
  if (!f) { vegas_log("nvram: cannot write %s", nv.path); return; }
  fwrite(nv.mem, 1, NVRAM_SIZE, f);
  fclose(f);
  nv.dirty = 0;
}

int nvram_handles(uint32_t p) { return p >= NVRAM_BASE && p < NVRAM_BASE + NVRAM_SIZE; }

static uint8_t read8(uint32_t off) {
  if (off >= RTC_BASE + 1) {
    /* Live clock (the control register's read latch is not modelled). */
    const time_t now = time(NULL);
    struct tm t;
#ifdef _WIN32
    localtime_s(&t, &now);
#else
    localtime_r(&now, &t);
#endif
    switch (off - RTC_BASE) {
      case 1: return bcd(t.tm_sec);
      case 2: return bcd(t.tm_min);
      case 3: return bcd(t.tm_hour);
      case 4: return (uint8_t)(t.tm_wday + 1);
      case 5: return bcd(t.tm_mday);
      case 6: return bcd(t.tm_mon + 1);
      default: return bcd(t.tm_year % 100);
    }
  }
  return nv.mem[off];
}

uint32_t nvram_read(uint32_t p, int size) {
  const uint32_t off = p - NVRAM_BASE;
  uint32_t v = 0;
  for (int i = 0; i < size && off + (uint32_t)i < NVRAM_SIZE; ++i) v |= (uint32_t)read8(off + (uint32_t)i) << (8 * i);
  return v;
}

void nvram_write(uint32_t p, uint32_t v, int size) {
  const uint32_t off = p - NVRAM_BASE;
  for (int i = 0; i < size && off + (uint32_t)i < NVRAM_SIZE; ++i) nv.mem[off + (uint32_t)i] = (uint8_t)(v >> (8 * i));
  nv.dirty = 1;
}
