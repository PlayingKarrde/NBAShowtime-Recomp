// Voodoo Banshee register-level model (init, status, VRAM, command FIFO capture).
#include "banshee.h"

#include <stdlib.h>
#include <string.h>

#include "vegas.h"

BansheeStats g_banshee_stats;
uint8_t* g_banshee_vram;

/* BAR0 sub-ranges (byte offsets) */
#define IOREG_END 0x100u        /* init / video registers */
#define CMDAGP_BASE 0x080000u   /* command FIFO / AGP registers */
#define REG2D_BASE 0x100000u
#define REG3D_BASE 0x200000u
#define REG3D_END 0x600000u     /* 3D + texture registers */

/* command FIFO registers (offsets from CMDAGP_BASE) */
#define CMD_BASEADDR0 0x20u /* in 4 KB pages */
#define CMD_BASESIZE0 0x24u /* bits 7:0 = pages - 1, bit 8 = enable */
#define CMD_BUMP0 0x28u
#define CMD_RDPTRL0 0x2Cu
#define CMD_AMIN0 0x34u
#define CMD_AMAX0 0x3Cu
#define CMD_DEPTH0 0x44u
#define CMD_HOLECNT0 0x48u

static struct {
  uint32_t config[64];
  uint32_t io[IOREG_END / 4];
  uint32_t cmd[0x200 / 4];
  uint32_t reg3d[(REG3D_END - REG3D_BASE) / 4];
  uint32_t reg2d[0x400 / 4];
  uint32_t fifo_rdptr;
} b;

void banshee_init(void) {
  memset(&b, 0, sizeof b);
  memset(&g_banshee_stats, 0, sizeof g_banshee_stats);
  if (!g_banshee_vram) g_banshee_vram = calloc(1, BANSHEE_VRAM_SIZE);
  else memset(g_banshee_vram, 0, BANSHEE_VRAM_SIZE);

  /* PCI configuration as left by the BIOS (measured in MAME after boot). */
  b.config[0x00 / 4] = 0x0003121Au;  /* 3dfx Banshee */
  b.config[0x04 / 4] = 0x00000000u;  /* command: Glide enables memory/IO itself */
  b.config[0x08 / 4] = 0x03000002u;  /* VGA display controller, revision 2 */
  b.config[0x10 / 4] = 0x10000000u;  /* BAR0: registers, 32 MB */
  b.config[0x14 / 4] = 0x12000000u;  /* BAR1: linear frame buffer, 32 MB */
  b.config[0x18 / 4] = 0x0C000001u;  /* BAR2: I/O, 256 bytes */

  /* Init/video register values read back on the real board before Glide set them. */
  b.io[0x04 / 4] = 0x01800040u; /* pciInit0 */
  b.io[0x18 / 4] = 0x0C579D29u; /* dramInit0 */
  b.io[0x1C / 4] = 0x00F02200u; /* dramInit1 */
  b.io[0x5C / 4] = 0x02210901u; /* vidProcCfg */
  b.io[0x98 / 4] = 0x00180200u; /* vidScreenSize: 512 x 384 */
}

/* ---- PCI config ---- */
uint32_t banshee_config_read(uint32_t reg) { return reg < 64 ? b.config[reg] : 0; }

void banshee_config_write(uint32_t reg, uint32_t v, uint32_t mask) {
  if (reg >= 64 || reg < 1) return; /* vendor/device read-only */
  uint32_t cur = (b.config[reg] & ~mask) | (v & mask);
  switch (reg) {
    case 2: return;                                  /* class/revision read-only */
    case 4: cur &= 0xFE000000u; break;               /* BAR0: 32 MB memory */
    case 5: cur &= 0xFE000000u; break;               /* BAR1: 32 MB memory */
    case 6: cur = (cur & 0xFFFFFF00u) | 1u; break;   /* BAR2: 256-byte I/O */
    default: break;
  }
  b.config[reg] = cur;
}

/* ---- status ---- */
static uint32_t status_value(void) {
  g_banshee_stats.status_reads++;
  /* Idle: PCI FIFO empty (0x3F free), nothing busy, no swaps pending.
     Bit 6 = vertical retrace, derived from virtual time at 57 Hz. */
  const uint64_t frame = 125000000ull / 57;
  const int vretrace = (g_vegas.cop0.cycles % frame) < frame / 20;
  return 0x0000003Fu | (vretrace ? 0x40u : 0u);
}

/* ---- command FIFO ---- */
static void fifo_update_config(void) {
  g_banshee_stats.fifo_base = (b.cmd[CMD_BASEADDR0 / 4] & 0xFFFFFu) << 12;
  g_banshee_stats.fifo_size = ((b.cmd[CMD_BASESIZE0 / 4] & 0xFFu) + 1u) << 12;
  g_banshee_stats.fifo_enabled = (b.cmd[CMD_BASESIZE0 / 4] & 0x100u) != 0;
}

static int in_fifo(uint32_t off) {
  return g_banshee_stats.fifo_enabled && off >= g_banshee_stats.fifo_base &&
         off < g_banshee_stats.fifo_base + g_banshee_stats.fifo_size;
}

/* A word written into the FIFO region. The packets are consumed instantly for now: the read
   pointer follows the writes, so Glide always sees free space. (Packet parsing feeds the
   renderer later.) */
static void fifo_word(uint32_t off) {
  g_banshee_stats.fifo_words++;
  b.fifo_rdptr = off + 4;
}

static uint32_t cmd_read(uint32_t off) {
  switch (off) {
    case CMD_RDPTRL0: return b.fifo_rdptr;
    case CMD_DEPTH0: return 0;   /* nothing pending */
    case CMD_HOLECNT0: return 0;
    default: return off < sizeof b.cmd ? b.cmd[off / 4] : 0;
  }
}

static void cmd_write(uint32_t off, uint32_t v) {
  if (off >= sizeof b.cmd) return;
  b.cmd[off / 4] = v;
  switch (off) {
    case CMD_BASEADDR0:
    case CMD_BASESIZE0: fifo_update_config(); break;
    case CMD_RDPTRL0: b.fifo_rdptr = v; break;
    case CMD_BUMP0: b.fifo_rdptr += v * 4; g_banshee_stats.fifo_words += v; break;
    default: break;
  }
}

/* ---- BAR decode ---- */
static uint32_t bar(int n) { return b.config[(0x10 / 4) + n] & 0xFE000000u; }

int banshee_mem_handles(uint32_t a) {
  if (!(b.config[1] & 2)) { /* memory decode disabled: still answer at the BIOS-assigned BARs */ }
  return (a - bar(0)) < (32u << 20) || (a - bar(1)) < (32u << 20);
}

static uint32_t sub(uint32_t v, uint32_t off, int size) {
  const uint32_t shift = (off & 3u) * 8;
  if (size == 4) return v;
  return size == 2 ? (v >> shift) & 0xFFFF : (v >> shift) & 0xFF;
}

static uint32_t merge(uint32_t old, uint32_t v, uint32_t off, int size) {
  if (size == 4) return v;
  const uint32_t shift = (off & 3u) * 8;
  const uint32_t mask = (size == 2 ? 0xFFFFu : 0xFFu) << shift;
  return (old & ~mask) | ((v << shift) & mask);
}

static uint32_t reg_read(uint32_t off, int size) {
  const uint32_t a = off & ~3u;
  uint32_t v;
  if (a == 0) v = status_value();
  else if (a < IOREG_END) v = b.io[a / 4];
  else if (a >= CMDAGP_BASE && a < CMDAGP_BASE + 0x200) v = cmd_read(a - CMDAGP_BASE);
  else if (a >= REG2D_BASE && a < REG2D_BASE + sizeof b.reg2d) v = b.reg2d[(a - REG2D_BASE) / 4];
  else if (a >= REG3D_BASE && a < REG3D_END) {
    v = (a == REG3D_BASE) ? status_value() : b.reg3d[(a - REG3D_BASE) / 4];
  } else {
    vegas_log("banshee: read of unknown register +%06X", off);
    v = 0;
  }
  return sub(v, off, size);
}

static void reg_write(uint32_t off, uint32_t v, int size) {
  const uint32_t a = off & ~3u;
  if (a < IOREG_END) {
    b.io[a / 4] = merge(b.io[a / 4], v, off, size);
  } else if (a >= CMDAGP_BASE && a < CMDAGP_BASE + 0x200) {
    cmd_write(a - CMDAGP_BASE, merge(cmd_read(a - CMDAGP_BASE), v, off, size));
  } else if (a >= REG2D_BASE && a < REG2D_BASE + sizeof b.reg2d) {
    b.reg2d[(a - REG2D_BASE) / 4] = merge(b.reg2d[(a - REG2D_BASE) / 4], v, off, size);
    g_banshee_stats.reg2d_writes++;
  } else if (a >= REG3D_BASE && a < REG3D_END) {
    b.reg3d[(a - REG3D_BASE) / 4] = merge(b.reg3d[(a - REG3D_BASE) / 4], v, off, size);
    g_banshee_stats.reg3d_writes++;
  } else {
    vegas_log("banshee: write of unknown register +%06X <- %08X", off, v);
  }
}

uint32_t banshee_mem_read(uint32_t a, int size) {
  if (a - bar(0) < (32u << 20)) return reg_read(a - bar(0), size);
  uint32_t off = (a - bar(1)) & (BANSHEE_VRAM_SIZE - 1); /* +16 MB 3D LFB window mirrors for now */
  uint32_t v = 0;
  memcpy(&v, g_banshee_vram + off, (size_t)size);
  return v;
}

void banshee_mem_write(uint32_t a, uint32_t v, int size) {
  if (a - bar(0) < (32u << 20)) { reg_write(a - bar(0), v, size); return; }
  const uint32_t raw = a - bar(1);
  const uint32_t off = raw & (BANSHEE_VRAM_SIZE - 1);
  if (raw >= BANSHEE_VRAM_SIZE) g_banshee_stats.lfb3d_writes++;
  memcpy(g_banshee_vram + off, &v, (size_t)size);
  if (in_fifo(off)) fifo_word(off);
  else g_banshee_stats.vram_writes++;
}

/* ---- I/O BAR ---- */
#define IO_BASE_PHYS 0x0C000000u /* PCI I/O space window; BAR2 = I/O 0x0000 */
int banshee_io_handles(uint32_t p) { return p >= IO_BASE_PHYS && p < IO_BASE_PHYS + IOREG_END; }
uint32_t banshee_io_read(uint32_t p, int size) { return reg_read(p - IO_BASE_PHYS, size); }
void banshee_io_write(uint32_t p, uint32_t v, int size) { reg_write(p - IO_BASE_PHYS, v, size); }
