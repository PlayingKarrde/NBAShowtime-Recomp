// Vegas SIO board: control, interrupt status/enable, vertical blank interrupt.
#ifndef NBA_SIO_H
#define NBA_SIO_H

#include <stdint.h>

#define SIO_BASE 0x04000000u
/* 57 Hz video (MAME: refresh 57.0) in virtual Count units (125 MHz). */
#define SIO_FRAME_CYCLES (125000000ull / 57)

void sio_init(const char* regs_path);
int sio_handles(uint32_t phys);
uint32_t sio_read(uint32_t phys, int size);
void sio_write(uint32_t phys, uint32_t value, int size);
void sio_advance(uint64_t now_cycles);
uint64_t sio_frames(void);

#endif
