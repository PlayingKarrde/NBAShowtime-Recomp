// Midway I/O ASIC at physical 0x04800000 (16-bit registers, 2 bytes apart) and a stand-in
// for the DCS2 sound board behind it. Register roles were measured on the real game in MAME
// (tools/mame/trace_mmio.lua); see docs/hardware.md.
#ifndef NBA_IOASIC_H
#define NBA_IOASIC_H

#include <stdint.h>

#define IOASIC_BASE 0x04800000u
#define IOASIC_SIZE 0x20u

/* Input ports (active low). Index 0..3 = registers +0x18, +0x1A, +0x1C, +0x1E. */
enum { IOASIC_IN_A = 0, IOASIC_IN_B = 1, IOASIC_IN_C = 2, IOASIC_IN_DIP = 3 };

void ioasic_init(void);
int ioasic_handles(uint32_t phys);
uint32_t ioasic_read(uint32_t phys, int size);
void ioasic_write(uint32_t phys, uint32_t value, int size);

/* Host side: set an input port word (bits are active low, 0 = pressed / switch on). */
void ioasic_set_input(int port, uint16_t value);
uint16_t ioasic_get_input(int port);

/* NBA Showtime's ports (active low): SYSTEM = coins/starts/service, IN1 = P1/P2 buttons,
   IN2 = P3/P4 buttons (bits 8-11 come from the 49-way joystick adapter), DIP = switches. */
#define IOASIC_PORT_SYSTEM IOASIC_IN_C
#define IOASIC_PORT_IN1 IOASIC_IN_A
#define IOASIC_PORT_IN2 IOASIC_IN_B
/* 49-way joystick position per player (0..3) and axis: 0..6, 3 = centre; X: 0 = left,
   Y: 0 = up. Read by the game through the I/O ASIC select register (selects 0-7). */
void ioasic_set_stick(int player, int x, int y);

/* DCS2 sound board state (for diagnostics and, later, real DSP emulation). */
typedef struct {
  int running;               /* boot monitor finished: the uploaded program was started */
  uint32_t words_received;   /* data words written to the board */
  uint32_t uploads;          /* completed 0x1A upload blocks */
  uint32_t pm_words, dm_words;
  uint16_t pm[0x4000][2];    /* uploaded program memory (24-bit words as two 16-bit halves) */
  uint16_t dm[0x4000];       /* uploaded data memory */
} DcsState;
extern DcsState g_dcs;

#endif
