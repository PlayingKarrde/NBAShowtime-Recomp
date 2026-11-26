// IDE disk model: ATA task file, PIO, IDENTIFY, bus-master DMA (READ/WRITE DMA).
// Backed by the game's CHD (MAME compressed hard disk, read with libchdr) or a raw disk image;
// writes go to an in-memory copy-on-write overlay.
#define _FILE_OFFSET_BITS 64
#include "ide.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <libchdr/chd.h>

#include "nile.h"
#include "vegas.h"

#define SECTOR 512u
#define OVERLAY_PAGE 1024u /* sectors per overlay page */

/* ATA status bits */
#define ST_BSY 0x80
#define ST_DRDY 0x40
#define ST_DSC 0x10
#define ST_DRQ 0x08
#define ST_ERR 0x01

/* Bus-master bits */
#define BM_CMD_START 0x01
#define BM_CMD_TO_MEMORY 0x08 /* 1 = device -> memory (read) */
#define BM_ST_ACTIVE 0x01
#define BM_ST_ERROR 0x02
#define BM_ST_IRQ 0x04

IdeStats g_ide_stats;

static struct {
  FILE* img;        /* raw image, or */
  chd_file* chd;    /* CHD: one decoded hunk is kept in hunk_buf */
  uint8_t* hunk_buf;
  uint32_t hunk_bytes;
  int64_t hunk_cur;
  uint64_t sectors;
  uint8_t** overlay; /* overlay[page][sector-in-page * 512] (NULL if untouched) */
  uint64_t overlay_pages;

  /* task file */
  uint8_t features, error, count, lba0, lba1, lba2, devhead, status, devctl;
  uint8_t command; /* last command (0 = none) */
  int irq;

  /* PIO: data moves in blocks (1 sector, or the SET MULTIPLE count for READ/WRITE MULTIPLE) */
  uint8_t buf[256 * SECTOR];
  uint32_t buf_pos, buf_len;
  uint32_t pio_sectors_left; /* including the block in the buffer */
  uint32_t pio_block;        /* sectors per block for the current command */
  uint32_t blk_sectors;      /* sectors in the buffer */
  uint64_t pio_lba;
  int pio_write;
  uint32_t multiple;         /* SET MULTIPLE MODE block size */

  /* DMA */
  int dma_pending; /* a DMA command waits for the bus master to start */
  uint8_t bm_cmd, bm_status, bm_dev[2];
  uint32_t prd;
} ide;

static void set_irq(int on) {
  ide.irq = on;
  nile_set_source(NILE_SRC_PCI_IDE, on && !(ide.devctl & 0x02));
}

static uint64_t cur_lba(void) {
  return ((uint64_t)(ide.devhead & 0x0F) << 24) | ((uint64_t)ide.lba2 << 16) |
         ((uint64_t)ide.lba1 << 8) | ide.lba0;
}
static void set_lba(uint64_t lba) {
  ide.lba0 = (uint8_t)lba;
  ide.lba1 = (uint8_t)(lba >> 8);
  ide.lba2 = (uint8_t)(lba >> 16);
  ide.devhead = (uint8_t)((ide.devhead & 0xF0) | ((lba >> 24) & 0x0F));
}
static uint32_t cur_count(void) { return ide.count ? ide.count : 256; }

int ide_init(const char* path) {
  memset(&ide, 0, sizeof ide);
  /* The BIOS sets the READ/WRITE MULTIPLE block size (SET MULTIPLE, count = word 0x8003C92C)
     during boot, before the game snapshot is taken: start with that drive state. */
  ide.multiple = 16;
  const size_t n = strlen(path);
  if (n > 4 && (!strcmp(path + n - 4, ".chd") || !strcmp(path + n - 4, ".CHD"))) {
    chd_error err = chd_open(path, CHD_OPEN_READ, NULL, &ide.chd);
    if (err != CHDERR_NONE) {
      vegas_log("ide: cannot open %s: %s", path, chd_error_string(err));
      return -1;
    }
    const chd_header* h = chd_get_header(ide.chd);
    ide.hunk_bytes = h->hunkbytes;
    ide.hunk_buf = malloc(ide.hunk_bytes);
    ide.hunk_cur = -1;
    ide.sectors = h->logicalbytes / SECTOR;
    chd_set_cache_budget(ide.chd, 16u << 20); /* recently decoded hunks */
  } else {
    ide.img = fopen(path, "rb");
    if (!ide.img) {
      vegas_log("ide: cannot open disk image %s", path);
      return -1;
    }
    fseeko(ide.img, 0, SEEK_END);
    ide.sectors = (uint64_t)ftello(ide.img) / SECTOR;
  }
  ide.overlay_pages = (ide.sectors + OVERLAY_PAGE - 1) / OVERLAY_PAGE;
  ide.overlay = calloc(ide.overlay_pages, sizeof *ide.overlay);
  ide.status = ST_DRDY | ST_DSC;
  ide.devhead = 0xA0;
  return 0;
}

static void read_sector(uint64_t lba, uint8_t* out) {
  if (lba >= ide.sectors) { memset(out, 0, SECTOR); return; }
  uint8_t* page = ide.overlay[lba / OVERLAY_PAGE];
  if (page && page[OVERLAY_PAGE * SECTOR + lba % OVERLAY_PAGE]) { /* dirty flag bytes after data */
    memcpy(out, page + (lba % OVERLAY_PAGE) * SECTOR, SECTOR);
    return;
  }
  if (ide.chd) {
    const uint64_t byte = lba * SECTOR;
    const int64_t hunk = (int64_t)(byte / ide.hunk_bytes);
    if (hunk != ide.hunk_cur) {
      if (chd_read(ide.chd, (uint32_t)hunk, ide.hunk_buf) != CHDERR_NONE) {
        vegas_log("ide: CHD read error at hunk %lld", (long long)hunk);
        memset(out, 0, SECTOR);
        ide.hunk_cur = -1;
        return;
      }
      ide.hunk_cur = hunk;
    }
    memcpy(out, ide.hunk_buf + byte % ide.hunk_bytes, SECTOR);
  } else {
    fseeko(ide.img, (off_t)(lba * SECTOR), SEEK_SET);
    if (fread(out, 1, SECTOR, ide.img) != SECTOR) memset(out, 0, SECTOR);
  }
  g_ide_stats.sectors_read++;
}

int ide_discard_writes; /* debug: drop all writes */

static void write_sector(uint64_t lba, const uint8_t* in) {
  if (lba >= ide.sectors || ide_discard_writes) return;
  uint8_t** page = &ide.overlay[lba / OVERLAY_PAGE];
  if (!*page) *page = calloc(1, OVERLAY_PAGE * SECTOR + OVERLAY_PAGE);
  memcpy(*page + (lba % OVERLAY_PAGE) * SECTOR, in, SECTOR);
  (*page)[OVERLAY_PAGE * SECTOR + lba % OVERLAY_PAGE] = 1;
  g_ide_stats.sectors_written++;
}

static void put_string(uint16_t* id, int word, int words, const char* s) {
  for (int i = 0; i < words; ++i) {
    char a = *s ? *s++ : ' ';
    char b = *s ? *s++ : ' ';
    id[word + i] = (uint16_t)(((uint8_t)a << 8) | (uint8_t)b);
  }
}

static void identify(void) {
  uint16_t id[256];
  memset(id, 0, sizeof id);
  const uint32_t cyl = 6256, heads = 16, spt = 63; /* CHD metadata */
  id[0] = 0x0040;
  id[1] = (uint16_t)cyl;
  id[3] = (uint16_t)heads;
  id[6] = (uint16_t)spt;
  put_string(id, 10, 10, "NBARECOMP0001");
  put_string(id, 23, 4, "1.0");
  put_string(id, 27, 20, "NBA RECOMP VIRTUAL DISK");
  id[47] = 0x8010;
  id[49] = 0x0300; /* LBA + DMA */
  id[53] = 0x0007;
  id[54] = (uint16_t)cyl;
  id[55] = (uint16_t)heads;
  id[56] = (uint16_t)spt;
  uint32_t chs = cyl * heads * spt;
  id[57] = (uint16_t)chs;
  id[58] = (uint16_t)(chs >> 16);
  id[60] = (uint16_t)ide.sectors;
  id[61] = (uint16_t)(ide.sectors >> 16);
  id[63] = 0x0407; /* MW DMA 0-2 supported, mode 2 selected */
  id[64] = 0x0003;
  id[80] = 0x001E;
  id[88] = 0x0407;
  memcpy(ide.buf, id, SECTOR);
  ide.buf_pos = 0;
  ide.buf_len = SECTOR;
  ide.pio_sectors_left = 0;
  ide.pio_write = 0;
  ide.status = ST_DRDY | ST_DSC | ST_DRQ;
  set_irq(1);
}

static void finish_command(int err) {
  ide.status = (uint8_t)(ST_DRDY | ST_DSC | (err ? ST_ERR : 0));
  ide.error = err ? 0x04 : 0x00; /* ABRT */
  set_irq(1);
}

/* PIO (ATA protocol): reads raise INTRQ when each block is ready; writes start with DRQ and no
   interrupt, then interrupt after each block (DRQ set again while sectors remain) and at the end. */
static void pio_block(void) {
  ide.blk_sectors = ide.pio_sectors_left < ide.pio_block ? ide.pio_sectors_left : ide.pio_block;
  if (!ide.pio_write)
    for (uint32_t i = 0; i < ide.blk_sectors; ++i) read_sector(ide.pio_lba + i, ide.buf + i * SECTOR);
  ide.buf_pos = 0;
  ide.buf_len = ide.blk_sectors * SECTOR;
  ide.status = ST_DRDY | ST_DSC | ST_DRQ;
}

static void pio_start(int write, uint32_t block) {
  ide.pio_lba = cur_lba();
  ide.pio_sectors_left = cur_count();
  ide.pio_write = write;
  ide.pio_block = block ? (block > 256 ? 256 : block) : 1;
  pio_block();
  if (!write) set_irq(1);
}

/* the buffered block has been transferred */
static void pio_block_done(void) {
  if (ide.pio_write)
    for (uint32_t i = 0; i < ide.blk_sectors; ++i) write_sector(ide.pio_lba + i, ide.buf + i * SECTOR);
  ide.pio_lba += ide.blk_sectors;
  ide.pio_sectors_left -= ide.blk_sectors;
  set_lba(ide.pio_lba);
  if (ide.pio_sectors_left) {
    pio_block();
    set_irq(1);
  } else {
    ide.status = ST_DRDY | ST_DSC;
    if (ide.pio_write) set_irq(1);
  }
}

/* Bus-master DMA transfer for the pending READ/WRITE DMA command. */
static void run_dma(RecompContext* ctx) {
  const int to_memory = ide.command == 0xC8 || ide.command == 0xC9;
  uint64_t lba = cur_lba();
  uint32_t remaining = cur_count() * SECTOR;
  uint8_t sector[SECTOR];
  uint32_t sector_pos = SECTOR; /* force a load on first byte */
  uint32_t prd = ide.prd;
  int guard = 0;
  g_ide_stats.dma_transfers++;

  while (remaining && guard++ < 4096) {
    const uint32_t base = mem_r32(ctx, 0x80000000u | (prd & 0x1FFFFFFFu));
    const uint32_t ctl = mem_r32(ctx, 0x80000000u | ((prd + 4) & 0x1FFFFFFFu));
    uint32_t len = ctl & 0xFFFF;
    if (!len) len = 0x10000;
    uint32_t addr = base & 0x1FFFFFFEu;
    while (len && remaining) {
      if (to_memory) {
        if (sector_pos == SECTOR) { read_sector(lba++, sector); sector_pos = 0; }
        uint32_t n = SECTOR - sector_pos;
        if (n > len) n = len;
        if (addr + n <= RECOMP_RAM_SIZE) memcpy(recomp_rdram + addr, sector + sector_pos, n);
        sector_pos += n; addr += n; len -= n; remaining -= n;
      } else {
        if (sector_pos == SECTOR) sector_pos = 0;
        uint32_t n = SECTOR - sector_pos;
        if (n > len) n = len;
        if (addr + n <= RECOMP_RAM_SIZE) memcpy(sector + sector_pos, recomp_rdram + addr, n);
        sector_pos += n; addr += n; len -= n; remaining -= n;
        if (sector_pos == SECTOR) write_sector(lba++, sector);
      }
    }
    if (ctl & 0x80000000u) break; /* end of table */
    prd += 8;
  }
  set_lba(lba);
  ide.count = 0;
  ide.dma_pending = 0;
  ide.bm_status = (uint8_t)((ide.bm_status & ~BM_ST_ACTIVE) | BM_ST_IRQ);
  finish_command(remaining != 0);
}

int ide_trace; /* debug: log commands */

static void execute(RecompContext* ctx, uint8_t cmd) {
  if (ide_trace) vegas_log("ide: cmd %02X lba %llu count %u", cmd, (unsigned long long)cur_lba(), cur_count());
  ide.command = cmd;
  ide.error = 0;
  g_ide_stats.commands++;
  switch (cmd) {
    case 0xEC: identify(); break;
    case 0x20: case 0x21: pio_start(0, 1); break;            /* READ SECTORS */
    case 0xC4: pio_start(0, ide.multiple); break;            /* READ MULTIPLE */
    case 0x30: case 0x31: pio_start(1, 1); break;            /* WRITE SECTORS */
    case 0xC5: pio_start(1, ide.multiple); break;            /* WRITE MULTIPLE */
    case 0xC6:                                               /* SET MULTIPLE MODE */
      ide.multiple = ide.count;
      finish_command(0);
      break;
    case 0xC8: case 0xC9: case 0xCA: case 0xCB: /* READ/WRITE DMA */
      ide.dma_pending = 1;
      ide.status = ST_DRDY | ST_DSC | ST_DRQ;
      if (ide.bm_cmd & BM_CMD_START) run_dma(ctx);
      break;
    case 0x10: case 0x91: case 0xEF: case 0xE0: case 0xE1: case 0xE2: case 0xE3:
    case 0xE5: case 0xE6: case 0xE7: case 0x40: case 0x70:
      finish_command(0); /* recalibrate, init params, set features, set multiple, power, verify, seek */
      break;
    default:
      vegas_log("ide: unsupported command %02X", cmd);
      finish_command(1);
      break;
  }
}

static uint32_t data_read(int size) {
  uint32_t v = 0;
  for (int i = 0; i < size && ide.buf_pos < ide.buf_len; ++i) v |= (uint32_t)ide.buf[ide.buf_pos++] << (8 * i);
  if (ide.buf_pos >= ide.buf_len && (ide.status & ST_DRQ) && !ide.pio_write) {
    if (ide.pio_sectors_left) pio_block_done();
    else ide.status = ST_DRDY | ST_DSC; /* IDENTIFY */
  }
  return v;
}

static void data_write(uint32_t v, int size) {
  if (!ide.pio_write || !(ide.status & ST_DRQ)) return;
  for (int i = 0; i < size && ide.buf_pos < ide.buf_len; ++i) ide.buf[ide.buf_pos++] = (uint8_t)(v >> (8 * i));
  if (ide.buf_pos >= ide.buf_len) pio_block_done();
}

int ide_handles(uint32_t p) { return p >= IDE_TASKFILE && p < IDE_BUSMASTER + 8; }

uint32_t ide_read(uint32_t p, int size) {
  switch (p) {
    case IDE_TASKFILE + 0: return data_read(size);
    case IDE_TASKFILE + 1: return ide.error;
    case IDE_TASKFILE + 2: return ide.count;
    case IDE_TASKFILE + 3: return ide.lba0;
    case IDE_TASKFILE + 4: return ide.lba1;
    case IDE_TASKFILE + 5: return ide.lba2;
    case IDE_TASKFILE + 6: return ide.devhead;
    case IDE_TASKFILE + 7: set_irq(0); return ide.status; /* status read acknowledges INTRQ */
    case IDE_CONTROL: return ide.status;                   /* alternate status: no acknowledge */
    case IDE_BUSMASTER + 0: return ide.bm_cmd;
    case IDE_BUSMASTER + 1: return ide.bm_dev[0];
    case IDE_BUSMASTER + 2: return ide.bm_status;
    case IDE_BUSMASTER + 3: return ide.bm_dev[1];
    case IDE_BUSMASTER + 4: return size == 4 ? ide.prd : (ide.prd & 0xFF);
    case IDE_BUSMASTER + 5: return (ide.prd >> 8) & 0xFF;
    case IDE_BUSMASTER + 6: return (ide.prd >> 16) & 0xFF;
    case IDE_BUSMASTER + 7: return (ide.prd >> 24) & 0xFF;
    default:
      vegas_log("ide: read%d of unknown register %08X", size * 8, p);
      return 0;
  }
}

void ide_write(RecompContext* ctx, uint32_t p, uint32_t v, int size) {
  switch (p) {
    case IDE_TASKFILE + 0: data_write(v, size); break;
    case IDE_TASKFILE + 1: ide.features = (uint8_t)v; break;
    case IDE_TASKFILE + 2: ide.count = (uint8_t)v; break;
    case IDE_TASKFILE + 3: ide.lba0 = (uint8_t)v; break;
    case IDE_TASKFILE + 4: ide.lba1 = (uint8_t)v; break;
    case IDE_TASKFILE + 5: ide.lba2 = (uint8_t)v; break;
    case IDE_TASKFILE + 6: ide.devhead = (uint8_t)v; break;
    case IDE_TASKFILE + 7: set_irq(0); execute(ctx, (uint8_t)v); break;
    case IDE_CONTROL:
      if ((v & 0x04) && !(ide.devctl & 0x04)) { /* SRST: soft reset */
        ide.status = ST_DRDY | ST_DSC; ide.error = 0x01; ide.dma_pending = 0; set_irq(0);
      }
      ide.devctl = (uint8_t)v;
      set_irq(ide.irq);
      break;
    case IDE_BUSMASTER + 0: {
      const uint8_t old = ide.bm_cmd;
      ide.bm_cmd = (uint8_t)v;
      if ((v & BM_CMD_START) && !(old & BM_CMD_START)) {
        ide.bm_status |= BM_ST_ACTIVE;
        if (ide.dma_pending) run_dma(ctx);
      } else if (!(v & BM_CMD_START)) {
        ide.bm_status &= ~BM_ST_ACTIVE;
      }
      break;
    }
    case IDE_BUSMASTER + 1: ide.bm_dev[0] = (uint8_t)v; break;
    case IDE_BUSMASTER + 2: /* bits 1,2 are write-1-to-clear; 5,6 are plain */
      ide.bm_status = (uint8_t)((ide.bm_status & ~(v & 0x06)) & ~0x60) | (uint8_t)(v & 0x60);
      break;
    case IDE_BUSMASTER + 3: ide.bm_dev[1] = (uint8_t)v; break;
    case IDE_BUSMASTER + 4:
      if (size == 4) ide.prd = v & ~3u;
      else ide.prd = (ide.prd & ~0xFFu) | (v & 0xFC);
      break;
    case IDE_BUSMASTER + 5: ide.prd = (ide.prd & ~0xFF00u) | ((v & 0xFF) << 8); break;
    case IDE_BUSMASTER + 6: ide.prd = (ide.prd & ~0xFF0000u) | ((v & 0xFF) << 16); break;
    case IDE_BUSMASTER + 7: ide.prd = (ide.prd & ~0xFF000000u) | ((v & 0xFF) << 24); break;
    default:
      vegas_log("ide: write%d of unknown register %08X <- %08X", size * 8, p, v);
      break;
  }
}
