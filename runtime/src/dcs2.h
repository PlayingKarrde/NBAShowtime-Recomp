// Midway DCS2 sound board (ADSP-2104 @ 16 MHz, SDRC memory controller, 4 MB sound DRAM).
// Board facts (memory map, SDRC, latches, control registers) follow MAME's DCS2 2104
// documentation; the implementation is our own.
#ifndef NBA_DCS2_H
#define NBA_DCS2_H

#include <stdint.h>

#define DCS2_CLOCK 16000000u

/* Load the boot ROM (vegassio.bin, 32 KB). Returns 0 on success. */
int dcs2_init(const char* bootrom_path);

/* Host side (I/O ASIC). Times are virtual Count units (125 MHz); the DSP is caught up first. */
void dcs2_sync(uint64_t now);
void dcs2_reset_line(int run, uint64_t now);      /* SOUNDCTL bit 0 */
void dcs2_data_w(uint16_t v, uint64_t now);       /* SOUNDOUT */
uint16_t dcs2_data_r(uint64_t now);               /* SOUNDIN read */
void dcs2_ack_w(uint64_t now);                    /* SOUNDIN write */
uint16_t dcs2_status(uint64_t now);               /* bits 7:6 for SOUNDSTAT, high byte = output control */
int dcs2_present(void);
void dcs2_debug_dump(void); /* print DSP state (diagnostics) */

/* Audio produced by the DSP (interleaved stereo, 16-bit). */
int dcs2_audio_rate(void);
/* Copy up to `max_frames` stereo frames out of the ring buffer; returns frames copied. */
int dcs2_audio_read(int16_t* out, int max_frames);

typedef struct {
  uint64_t instructions;
  uint64_t host_writes, host_reads, resets;
  uint64_t output_writes;  /* words the DSP put in its output latch */
  uint64_t samples;        /* audio words transmitted */
  uint64_t irq1, timer_irqs;
  int booted_program;      /* a software reboot (SYSCONTROL bit 9) happened */
} Dcs2Stats;
extern Dcs2Stats g_dcs2_stats;

#endif
