// IDE hard disk on the Vegas PCI IDE controller (task file + bus-master DMA).
#ifndef NBA_IDE_H
#define NBA_IDE_H

#include <stdint.h>

#include "recomp.h"

/* Physical addresses (kseg1 0xAC000100 -> physical 0x0C000100), as programmed by the BIOS. */
#define IDE_TASKFILE 0x0C000100u /* +0 data .. +7 status/command */
#define IDE_CONTROL 0x0C00010Au  /* alternate status / device control */
#define IDE_BUSMASTER 0x0C000120u /* +0 command, +2 status, +4 PRD table address */

int ide_init(const char* image_path);
int ide_handles(uint32_t phys);
uint32_t ide_read(uint32_t phys, int size);
void ide_write(RecompContext* ctx, uint32_t phys, uint32_t value, int size);

/* Statistics for diagnostics. */
typedef struct {
  uint64_t sectors_read, sectors_written, commands, dma_transfers;
} IdeStats;
extern IdeStats g_ide_stats;

#endif
