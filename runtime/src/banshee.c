// Voodoo Banshee register-level model (init, status, VRAM, command FIFO capture).
#include "banshee.h"

#include <stdlib.h>
#include <string.h>

#include "vegas.h"
#include "voodoo3d.h"

BansheeStats g_banshee_stats;
static void fifo_reset(void);
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
  fifo_reset();
  v3d_init(g_banshee_vram, BANSHEE_VRAM_SIZE - 1);

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

uint32_t banshee_io_reg(uint32_t byte_off) { return byte_off < IOREG_END ? b.io[byte_off / 4] : 0; }

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

/* ---- command FIFO parsing ----
   Glide writes packets sequentially into the FIFO region of VRAM and wraps with a JMP packet.
   Complete packets are executed as soon as their last word has been written, so the read
   pointer stays right behind the writer and Glide always sees free space. */
static struct {
  uint32_t parse;   /* next packet header (VRAM byte offset) */
  uint32_t written; /* end of the contiguous run of words written from `parse` */
  int synced;
} fq;

static uint32_t vram32(uint32_t off) {
  uint32_t w;
  memcpy(&w, g_banshee_vram + (off & (BANSHEE_VRAM_SIZE - 1) & ~3u), 4);
  return w;
}

static void reg2d_fifo_write(uint32_t regnum, uint32_t data) {
  b.reg2d[regnum & 0xFF] = data;
  g_banshee_stats.reg2d_writes++;
}

static void fifo_register_write(uint32_t target, uint32_t data) {
  if (target & 0x800u) reg2d_fifo_write(target & 0xFF, data); /* bit 11: 2D register space */
  else v3d_reg_write(target & 0xFF, (target >> 8) & 0xF, data);
}

/* Words a packet needs (header included), or 0 if unknown. */
static uint32_t packet_size(uint32_t cmd) {
  switch (cmd & 7) {
    case 0: return ((cmd >> 3) & 7) == 4 ? 2 : 1;
    case 1: return 1 + (cmd >> 16);
    case 2: return 1 + (uint32_t)__builtin_popcount(cmd >> 3);
    case 3: return 1 + v3d_packet3_size(cmd);
    case 4: return 1 + (uint32_t)__builtin_popcount((cmd >> 15) & 0x3FFF) + ((cmd >> 29) & 7);
    case 5: return 2 + ((cmd >> 3) & 0x7FFFF);
    default: return 0;
  }
}

static uint32_t body_buf[0x10000];

static void execute_packet(uint32_t at, uint32_t cmd, uint32_t size) {
  g_v3d_stats.packets[cmd & 7]++;
  for (uint32_t i = 1; i < size && i <= 0x10000; ++i) body_buf[i - 1] = vram32(at + 4 * i);
  const uint32_t* w = body_buf;
  switch (cmd & 7) {
    case 0:
      if (((cmd >> 3) & 7) == 3) { /* JMP local frame buffer */
        fq.parse = ((cmd >> 6) & 0x7FFFFFu) << 2;
        fq.written = fq.parse;
        return;
      }
      if ((cmd >> 3) & 7) vegas_log("banshee: unsupported FIFO jump type %u", (cmd >> 3) & 7);
      break;
    case 1: {
      uint32_t target = (cmd >> 3) & 0xFFF;
      const uint32_t inc = (cmd >> 15) & 1;
      for (uint32_t i = 0; i < (cmd >> 16); ++i, target += inc) fifo_register_write(target, w[i]);
      break;
    }
    case 2: {
      uint32_t n = 0;
      for (int bit = 3; bit < 32; ++bit)
        if (cmd & (1u << bit)) reg2d_fifo_write(2 + (uint32_t)(bit - 3), w[n++]); /* from clip0Min */
      break;
    }
    case 3: v3d_packet3(cmd, w); break;
    case 4: {
      uint32_t target = (cmd >> 3) & 0xFFF, n = 0;
      for (int bit = 15; bit <= 28; ++bit, ++target)
        if (cmd & (1u << bit)) fifo_register_write(target, w[n++]);
      break;
    }
    case 5: {
      const uint32_t count = (cmd >> 3) & 0x7FFFF;
      uint32_t target = (w[0] & 0x3FFFFFFFu) / 4;
      switch (cmd >> 30) {
        case 0:
          for (uint32_t i = 0; i < count; ++i, ++target)
            memcpy(g_banshee_vram + ((target * 4) & (BANSHEE_VRAM_SIZE - 1)), &w[1 + i], 4);
          g_banshee_stats.vram_writes += count;
          break;
        case 2: for (uint32_t i = 0; i < count; ++i) v3d_lfb_write(target + i, w[1 + i], 0xFFFFFFFFu); break;
        case 3: for (uint32_t i = 0; i < count; ++i) v3d_texture_write(target + i, w[1 + i]); break;
        default: vegas_log("banshee: unsupported FIFO packet 5 space %u", cmd >> 30); break;
      }
      break;
    }
    default: break;
  }
  fq.parse = at + size * 4;
}

static void fifo_run(void) {
  for (int guard = 0; guard < 100000; ++guard) {
    if (fq.written - fq.parse < 4 || fq.written <= fq.parse) return;
    const uint32_t cmd = vram32(fq.parse);
    const uint32_t size = packet_size(cmd);
    if (!size) {
      vegas_log("banshee: unknown FIFO packet %08X at %06X", cmd, fq.parse);
      fq.parse += 4;
      continue;
    }
    if (fq.written - fq.parse < size * 4) return; /* wait for the rest */
    execute_packet(fq.parse, cmd, size);
  }
}

static void fifo_reset(void) { memset(&fq, 0, sizeof fq); }

static void fifo_word(uint32_t off) {
  g_banshee_stats.fifo_words++;
  if (!fq.synced) { fq.parse = off; fq.written = off; fq.synced = 1; }
  if (off == fq.written) fq.written = off + 4;
  else if (off == fq.parse) fq.written = off + 4; /* writer restarted at the parse point */
  else { /* out of order: resynchronise on this word */
    vegas_log("banshee: FIFO write at %06X, expected %06X (parse %06X)", off, fq.written, fq.parse);
    fq.parse = off;
    fq.written = off + 4;
  }
  fifo_run();
  b.fifo_rdptr = fq.parse;
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
    case CMD_RDPTRL0: b.fifo_rdptr = v; fq.parse = fq.written = v; fq.synced = 1; break;
    case CMD_BUMP0: vegas_log("banshee: FIFO bump mode not supported (%u)", v); break;
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
    const uint32_t w = (a - REG3D_BASE) / 4;
    b.reg3d[w] = merge(b.reg3d[w], v, off, size);
    g_banshee_stats.reg3d_writes++;
    uint32_t data = b.reg3d[w];
    if (w & (1u << 18)) data = __builtin_bswap32(data);               /* byte-swizzled alias */
    if (w & (1u << 19)) vegas_log("banshee: remapped 3D register write +%06X", off); /* rare */
    v3d_reg_write(w & 0xFF, (w >> 8) & 0xF, data);
  } else if (a >= 0x600000u && a < 0x800000u) {
    v3d_texture_write((a - 0x600000u) / 4, v); /* TMU 0 texture download window */
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
