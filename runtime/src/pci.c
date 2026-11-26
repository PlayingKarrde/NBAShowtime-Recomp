// PCI bus behind the VRC5074 window at physical 0x10000000-0x17FFFFFF.
// In configuration mode (VRC5074 PCIINIT1 type 5) one address bit selects the slot (IDSEL);
// in memory mode the address is decoded against device BARs.
#include "pci.h"

#include "banshee.h"
#include "nile.h"
#include "vegas.h"

/* IDSEL bits used by the BIOS/Glide probe: 0x10400000, 0x10800000, 0x11000000, 0x12000000, 0x14000000 */
enum { SLOT_NONE = -1, SLOT_BANSHEE = 24, SLOT_CMD646 = 26 };

/* CMD646 IDE controller config space as read on the real board (offsets 0x00-0x6C). */
static const uint32_t cmd646_config[28] = {
    0x06461095, 0x00000107, 0x01018F05, 0x00000000, 0x0C000101, 0x0C000109, 0x0C000111,
    0x0C000119, 0x0C000121, 0x00000000, 0x00000000, 0x00000000, 0x00000000, 0x00000000,
    0x00000000, 0x0000010E, 0x00000000, 0x0000008A, 0xDF58B4F0, 0x000055D3, 0x0BC00454,
    0x000055D4, 0x0BC300F0, 0x000055D4, 0x00000000, 0x00000000, 0x00000000, 0x00000000};
static uint32_t cmd646_regs[64];

void pci_init(void) {
  for (int i = 0; i < 64; ++i) cmd646_regs[i] = i < 28 ? cmd646_config[i] : 0;
}

int pci_handles(uint32_t p) { return p >= PCI_WINDOW_BASE && p < PCI_WINDOW_BASE + PCI_WINDOW_SIZE; }

static int config_slot(uint32_t p) {
  const uint32_t idsel = (p - PCI_WINDOW_BASE) & 0x07C00000u;
  for (int bit = 22; bit <= 26; ++bit)
    if (idsel == (1u << bit)) return bit;
  return SLOT_NONE;
}

static uint32_t sub(uint32_t v, uint32_t p, int size) {
  const uint32_t shift = (p & 3u) * 8;
  if (size == 4) return v;
  return size == 2 ? (v >> shift) & 0xFFFF : (v >> shift) & 0xFF;
}

uint32_t pci_read(uint32_t p, int size) {
  if (nile_pci_is_config()) {
    const uint32_t reg = (p & 0xFFu) >> 2;
    switch (config_slot(p)) {
      case SLOT_BANSHEE: return sub(banshee_config_read(reg), p, size);
      case SLOT_CMD646: return sub(cmd646_regs[reg], p, size);
      default: return size == 4 ? 0xFFFFFFFFu : size == 2 ? 0xFFFFu : 0xFFu; /* empty slot */
    }
  }
  const uint32_t a = p; /* memory cycle: PCI address = physical (window maps 1:1) */
  if (banshee_mem_handles(a)) return banshee_mem_read(a, size);
  vegas_log("pci: memory read%d of unmapped %08X", size * 8, a);
  return 0xFFFFFFFFu;
}

void pci_write(uint32_t p, uint32_t v, int size) {
  if (nile_pci_is_config()) {
    const uint32_t reg = (p & 0xFFu) >> 2;
    const uint32_t shift = (p & 3u) * 8;
    const uint32_t mask = size == 4 ? 0xFFFFFFFFu : ((size == 2 ? 0xFFFFu : 0xFFu) << shift);
    const uint32_t val = size == 4 ? v : (v << shift);
    switch (config_slot(p)) {
      case SLOT_BANSHEE: banshee_config_write(reg, val, mask); break;
      case SLOT_CMD646:
        if (reg == 1) cmd646_regs[reg] = (cmd646_regs[reg] & ~mask) | (val & mask);
        break;
      default: break;
    }
    return;
  }
  if (banshee_mem_handles(p)) { banshee_mem_write(p, v, size); return; }
  vegas_log("pci: memory write%d of unmapped %08X <- %08X", size * 8, p, v);
}
