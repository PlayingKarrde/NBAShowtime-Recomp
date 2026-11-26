// NEC VRC5074 ("Nile 4") system controller at physical 0x1FA00000:
// interrupt controller (16 sources routed to CPU INT0..4 = IP2..IP6) and the 16550 UART.
#ifndef NBA_NILE_H
#define NBA_NILE_H

#include <stdint.h>

#define NILE_BASE 0x1FA00000u
#define NILE_SIZE 0x1000u

/* Interrupt sources (bit numbers), as used by the BIOS dispatch table at 0x8002A310. */
enum {
  NILE_SRC_UART = 4,
  NILE_SRC_PCI_IDE = 11, /* routed to INT0/IP2; matches the IDE interrupt measured in MAME */
};

/* regs_path: optional dump of 0x1FA00000-0x1FA001FF taken at the game's entry point. */
void nile_init(const char* regs_path);
int nile_handles(uint32_t phys);
uint32_t nile_read(uint32_t phys, int size);
void nile_write(uint32_t phys, uint32_t value, int size);

/* PCI window mode (VRC5074 PCIINIT1): non-zero while the window does configuration cycles. */
int nile_pci_is_config(void);

/* Called as virtual time advances: timer reloads raise their interrupt sources. */
void nile_advance(void);

/* Flush a partial UART output line to the log. */
void nile_uart_flush(void);

/* Drive an interrupt source (level). */
void nile_set_source(int source, int asserted);

#endif
