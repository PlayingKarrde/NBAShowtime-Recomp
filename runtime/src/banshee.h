// 3dfx Voodoo Banshee (PCI vendor 0x121A, device 0x0003) at register level, enough for the
// game's statically linked Glide 3.10 to initialise it and stream its command FIFO.
// Register offsets follow the Banshee spec; reset values were measured in MAME.
#ifndef NBA_BANSHEE_H
#define NBA_BANSHEE_H

#include <stdint.h>

#include "recomp.h"

#define BANSHEE_VRAM_SIZE (16u << 20)

void banshee_init(void);

/* PCI configuration space (register index = byte offset / 4). */
uint32_t banshee_config_read(uint32_t reg);
void banshee_config_write(uint32_t reg, uint32_t value, uint32_t mask);

/* Memory decode: returns non-zero if `pci_addr` falls in BAR0 (registers) or BAR1 (frame buffer). */
int banshee_mem_handles(uint32_t pci_addr);
uint32_t banshee_mem_read(uint32_t pci_addr, int size);
void banshee_mem_write(uint32_t pci_addr, uint32_t value, int size);

/* I/O BAR (init/video registers mirrored at PCI I/O 0x00-0xFF). */
int banshee_io_handles(uint32_t phys);
uint32_t banshee_io_read(uint32_t phys, int size);
void banshee_io_write(uint32_t phys, uint32_t value, int size);

typedef struct {
  uint64_t fifo_words;       /* command FIFO words written by Glide */
  uint64_t reg3d_writes;     /* direct (non-FIFO) 3D register writes */
  uint64_t reg2d_writes;
  uint64_t vram_writes;      /* frame buffer / texture memory writes outside the FIFO */
  uint64_t lfb3d_writes;     /* writes through the 3D LFB window (BAR1 + 16 MB) */
  uint64_t status_reads;
  uint32_t fifo_base, fifo_size; /* byte offsets in VRAM */
  int fifo_enabled;
} BansheeStats;
extern BansheeStats g_banshee_stats;

extern uint8_t* g_banshee_vram;

#endif
