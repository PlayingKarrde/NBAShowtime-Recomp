// Vegas SIO board at physical 0x04000000: control register, interrupt status/enable, and the
// vertical blank interrupt (sub-source 5), routed to VRC5074 source 10 (CPU IP3).
//
// From the BIOS IP3 handler (0x80011484): pending = enable(0x04001000) & status(0x04002000),
// six sub-sources with handlers at 0x8002A2F8. (Register roles confirmed with a MAME trace:
// 0x04001000 is written with 0x22 = sources 1 and 5, 0x04002000 is only read.) The vblank handler (0x800113AC) acknowledges by
// clearing and then setting bit 3 of the control register (0x04000000) and increments the
// 64-bit frame counter that syscalls 56/57 use.
#include "sio.h"

#include <stdio.h>
#include <string.h>

#include "nile.h"
#include "vegas.h"

#define SIO_REG_CONTROL 0
#define SIO_REG_ENABLE 1
#define SIO_REG_STATUS 2
#define SIO_VBLANK_BIT 0x20       /* sub-source 5 */
#define SIO_CTRL_VBLANK_ACK 0x08  /* 0 = clear/hold vblank, 1 = allow */
#define NILE_SRC_SIO 10           /* VRC5074 source 10 -> INT1 -> IP3 */

static struct {
  uint8_t regs[8];
  uint8_t pending;
  uint64_t next_vblank;  /* virtual time (Count units) */
  uint64_t frames;
} sio;

static void update_irq(void) {
  nile_set_source(NILE_SRC_SIO, (sio.pending & sio.regs[SIO_REG_ENABLE]) != 0);
}

void sio_init(const char* regs_path) {
  memset(&sio, 0, sizeof sio);
  sio.regs[SIO_REG_CONTROL] = 0x09;
  if (regs_path) {
    FILE* f = fopen(regs_path, "rb");
    if (f) {
      if (fread(sio.regs, 1, sizeof sio.regs, f) != sizeof sio.regs) vegas_log("sio: short register dump");
      fclose(f);
    }
  }
  sio.regs[SIO_REG_STATUS] = 0; /* status is computed */
  sio.next_vblank = SIO_FRAME_CYCLES;
}

int sio_handles(uint32_t p) { return p >= SIO_BASE && p < SIO_BASE + 0x8000 && (p & 0xFFF) < 8; }

uint32_t sio_read(uint32_t p, int size) {
  (void)size;
  const int reg = (int)((p - SIO_BASE) >> 12);
  if (reg == SIO_REG_STATUS) return sio.pending;
  return sio.regs[reg];
}

void sio_write(uint32_t p, uint32_t v, int size) {
  (void)size;
  const int reg = (int)((p - SIO_BASE) >> 12);
  if (reg == SIO_REG_STATUS) return;
  sio.regs[reg] = (uint8_t)v;
  if (reg == SIO_REG_CONTROL && !(v & SIO_CTRL_VBLANK_ACK)) sio.pending &= (uint8_t)~SIO_VBLANK_BIT;
  update_irq();
}

void sio_advance(uint64_t now) {
  while (now >= sio.next_vblank) {
    sio.next_vblank += SIO_FRAME_CYCLES;
    sio.frames++;
    if (sio.regs[SIO_REG_CONTROL] & SIO_CTRL_VBLANK_ACK) sio.pending |= SIO_VBLANK_BIT;
  }
  update_irq();
}

uint64_t sio_frames(void) { return sio.frames; }
