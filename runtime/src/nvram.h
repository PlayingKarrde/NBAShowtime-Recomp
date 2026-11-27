// Battery-backed NVRAM + real-time clock (timekeeper) at physical 0x04400000.
#ifndef NBA_NVRAM_H
#define NBA_NVRAM_H

#include <stdint.h>

#define NVRAM_BASE 0x04400000u
#define NVRAM_SIZE 0x8000u

/* path: optional host file to load from (and save to with nvram_save). NULL = blank board.
   vegas_init starts with a blank board; the host may call this again before the game runs. */
int nvram_init(const char* path); /* 1 if the file was read */
uint8_t* nvram_mem(void);         /* the raw contents (NVRAM_SIZE bytes) */
void nvram_touch(void);           /* mark changed: nvram_save writes it */
void nvram_save(void);
int nvram_handles(uint32_t phys);
uint32_t nvram_read(uint32_t phys, int size);
void nvram_write(uint32_t phys, uint32_t value, int size);

#endif
