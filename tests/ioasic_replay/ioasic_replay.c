// Replays a MAME trace of the I/O ASIC sound ports (tools/mame/trace_mmio.lua with
// TRACE_BASE=04800010) through the runtime's I/O ASIC and the emulated DCS2 sound board
// (ADSP-2104 running the real boot ROM and the uploaded program), and checks that every reply
// the game read from the real board is reproduced.
//
// usage: ioasic_replay <sound_trace.txt> [vegassio.bin]
//   Without the boot ROM, the I/O ASIC's stand-in board is tested instead.
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "../../runtime/src/dcs2.h"
#include "../../runtime/src/ioasic.h"
#include "vegas.h"

VegasMachine g_vegas; /* normally in vegas_machine.c: only the virtual clock is used here */
static void quiet_log(const char* fmt, ...);
void (*vegas_log)(const char* fmt, ...) = quiet_log;
static void quiet_log(const char* fmt, ...) {
  va_list ap;
  va_start(ap, fmt);
  vprintf(fmt, ap);
  va_end(ap);
  printf("\n");
}

#define COUNT_HZ 125000000.0

int main(int argc, char** argv) {
  if (argc < 2) {
    fprintf(stderr, "usage: %s <sound_trace.txt> [vegassio.bin]\n", argv[0]);
    return 2;
  }
  FILE* f = fopen(argv[1], "r");
  if (!f) { perror(argv[1]); return 1; }
  ioasic_init();
  const int real = argc > 2 && dcs2_init(argv[2]) == 0;
  printf("testing %s\n", real ? "emulated DCS2 board (ADSP-2104 + boot ROM)" : "stand-in board");

  char line[256];
  long reads = 0, match = 0, mismatch = 0, writes = 0, late = 0;
  double t0 = -1;
  uint64_t outputs_at_write = 0; /* DSP output-latch writes seen when the host last sent a word */
  int shown = 0;
  while (fgets(line, sizeof line, f)) {
    char t[32], kind[8], pc[32];
    unsigned addr, val;
    if (sscanf(line, "%31s %7s %x %x %31s", t, kind, &addr, &val, pc) != 5) continue;
    const double secs = atof(t + 2); /* "t=1.234" */
    if (t0 < 0) t0 = secs;
    const uint64_t now = (uint64_t)((secs - t0) * COUNT_HZ);
    if (now > g_vegas.cop0.cycles) g_vegas.cop0.cycles = now;
    if (real) dcs2_sync(g_vegas.cop0.cycles);
    if (kind[0] == 'W') {
      /* Like the game, wait for "input empty" before sending the next word (the polling reads
         were left out of the trace). */
      if (real && addr == 0x04800010) {
        int waited = 0;
        while (!(ioasic_read(0x04800014, 2) & 0x80) && waited < 2000) {
          g_vegas.cop0.cycles += 125; /* 1 us */
          dcs2_sync(g_vegas.cop0.cycles);
          waited++;
        }
      }
      ioasic_write(addr, val, 2);
      if (addr == 0x04800010) outputs_at_write = g_dcs2_stats.output_writes;
      writes++;
    } else if (addr == 0x04800016) {
      /* The game only reads the reply after seeing "reply available"; give the DSP up to 2 ms
         of extra time if our timing is behind the real board's. */
      if (real) {
        int waited = 0;
        /* wait for a reply produced after the last command (the latch may still hold an old,
           unacknowledged reply) */
        /* up to 20 ms: replies leave the DSP once per audio frame (~16 ms), so our frame phase
           can lag the real board's */
        while ((!(ioasic_read(0x04800014, 2) & 0x40) || g_dcs2_stats.output_writes == outputs_at_write) &&
               waited < 20000) {
          g_vegas.cop0.cycles += 125; /* 1 us */
          dcs2_sync(g_vegas.cop0.cycles);
          waited++;
        }
        if (waited) late++;
      }
      const uint32_t got = ioasic_read(addr, 2);
      reads++;
      if (got == (val & 0xFFFF)) {
        match++;
      } else {
        mismatch++;
        if (shown++ < 15) printf("mismatch %s: real board %04X, model %04X\n", t, val & 0xFFFF, got);
      }
    }
  }
  fclose(f);
  printf("writes %ld, reply reads %ld: matched %ld, mismatched %ld (%ld needed extra DSP time)\n", writes,
         reads, match, mismatch, late);
  if (real)
    printf("DSP: %llu instructions, %llu resets, program booted: %s, %llu audio words, %llu IRQ1\n",
           (unsigned long long)g_dcs2_stats.instructions, (unsigned long long)g_dcs2_stats.resets,
           g_dcs2_stats.booted_program ? "yes" : "no", (unsigned long long)g_dcs2_stats.samples,
           (unsigned long long)g_dcs2_stats.irq1);
  return mismatch ? 1 : 0;
}
