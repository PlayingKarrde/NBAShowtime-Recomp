// Replays a MAME trace of the I/O ASIC sound ports (tools/mame/trace_mmio.lua with
// TRACE_BASE=04800010) through the runtime's I/O ASIC + DCS2 stand-in, and checks that every
// reply the game read from the real sound board is produced by the model.
//
// usage: ioasic_replay <sound_trace.txt>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "../../runtime/src/ioasic.h"
#include "vegas.h"

static void quiet_log(const char* fmt, ...);
void (*vegas_log)(const char* fmt, ...) = quiet_log; /* normally provided by vegas_machine.c */

static void quiet_log(const char* fmt, ...) {
  va_list ap;
  va_start(ap, fmt);
  vprintf(fmt, ap);
  va_end(ap);
  printf("\n");
}

int main(int argc, char** argv) {
  if (argc < 2) {
    fprintf(stderr, "usage: %s <sound_trace.txt>\n", argv[0]);
    return 2;
  }
  FILE* f = fopen(argv[1], "r");
  if (!f) { perror(argv[1]); return 1; }
  ioasic_init();

  char line[256];
  long reads = 0, match = 0, mismatch_monitor = 0, mismatch_running = 0, writes = 0;
  int shown = 0;
  while (fgets(line, sizeof line, f)) {
    char t[32], kind[8], pc[32];
    unsigned addr, val;
    if (sscanf(line, "%31s %7s %x %x %31s", t, kind, &addr, &val, pc) != 5) continue;
    if (kind[0] == 'W') {
      ioasic_write(addr, val, 2);
      writes++;
    } else if (addr == 0x04800016) {
      /* The game only reads the reply port after seeing "reply available". */
      const uint32_t got = ioasic_read(addr, 2);
      reads++;
      if (got == (val & 0xFFFF)) {
        match++;
      } else {
        if (g_dcs.running) mismatch_running++;
        else mismatch_monitor++;
        if (shown++ < 15)
          printf("mismatch %s: real board %04X, model %04X (%s)\n", t, val & 0xFFFF, got,
                 g_dcs.running ? "running program" : "boot monitor");
      }
    }
  }
  fclose(f);
  printf("writes %ld, reply reads %ld, matched %ld, mismatched %ld in boot monitor, %ld while running\n",
         writes, reads, match, mismatch_monitor, mismatch_running);
  printf("uploads %u (%u PM + %u DM words), program %s\n", g_dcs.uploads, g_dcs.pm_words, g_dcs.dm_words,
         g_dcs.running ? "running" : "not started");
  return mismatch_monitor ? 1 : 0;
}
