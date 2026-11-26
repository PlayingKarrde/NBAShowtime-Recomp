// PCI bus behind the VRC5074: configuration cycles (slot by IDSEL bit) and memory cycles.
#ifndef NBA_PCI_H
#define NBA_PCI_H

#include <stdint.h>

#define PCI_WINDOW_BASE 0x10000000u
#define PCI_WINDOW_SIZE 0x08000000u

void pci_init(void);
int pci_handles(uint32_t phys);
uint32_t pci_read(uint32_t phys, int size);
void pci_write(uint32_t phys, uint32_t value, int size);

#endif
