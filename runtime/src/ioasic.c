// Midway I/O ASIC model + DCS2 sound board stand-in (boot monitor protocol, no audio yet).
#include "ioasic.h"

#include <string.h>

#include "vegas.h"

/* Register offsets (byte offsets from IOASIC_BASE), as used by this game/BIOS. */
#define REG_CONTROL 0x00   /* R/W, reads back 0x4000 */
#define REG_SELECT 0x04    /* W: 4-bit select; affects bits 8-11 of REG_INPUT_B */
#define REG_SOUND_OUT 0x10 /* W: data word to the sound board */
#define REG_SOUND_RESET 0x12 /* R/W: 0 = hold sound board in reset, 1 = run */
#define REG_SOUND_STAT 0x14 /* R: 0x80 board ready for data, 0x40 reply available, 0x08 always */
#define REG_SOUND_IN 0x16  /* R: reply from the sound board; W: acknowledge */
#define REG_INPUT_A 0x18
#define REG_INPUT_B 0x1A
#define REG_INPUT_C 0x1C
#define REG_INPUT_DIP 0x1E

DcsState g_dcs;

enum { DCS_RESET, DCS_WAIT_CMD, DCS_ECHO, DCS_UPLOAD_HDR, DCS_UPLOAD_DATA };

static struct {
  uint16_t regs[IOASIC_SIZE / 2];
  uint16_t inputs[4];

  /* sound board */
  int state;
  uint16_t replies[64];
  int reply_head, reply_count;
  uint16_t last_reply;
  uint16_t hdr[3];
  int hdr_n;
  uint32_t upload_left, upload_pos;
  uint16_t upload_sum;
} io;

/* ---- DCS2 boot monitor (protocol decoded from MAME traces) ---- */
static void reply(uint16_t v) {
  if (io.reply_count == 64) return;
  io.replies[(io.reply_head + io.reply_count++) % 64] = v;
}

static void dcs_reset(void) {
  io.reply_count = 0;
  io.state = DCS_WAIT_CMD;
  g_dcs.running = 0;
  reply(0x000A); /* "ready for a command" */
}

static void dcs_upload_word(uint16_t w) {
  const uint16_t start = io.hdr[0], type = io.hdr[2];
  io.upload_sum = (uint16_t)(io.upload_sum + w);
  if (type == 0) { /* program memory: 24-bit words sent as two halves */
    const uint32_t word = start + io.upload_pos / 2;
    if (word < 0x4000) g_dcs.pm[word][io.upload_pos & 1] = w;
    if (io.upload_pos & 1) g_dcs.pm_words++;
  } else {
    const uint32_t word = start + io.upload_pos;
    if (word < 0x4000) g_dcs.dm[word] = w;
    g_dcs.dm_words++;
  }
  io.upload_pos++;
  if (--io.upload_left == 0) {
    g_dcs.uploads++;
    reply(io.upload_sum); /* checksum = 16-bit sum of the data words */
    reply(0x000A);
    io.state = DCS_WAIT_CMD;
  }
}

static void dcs_monitor_command(uint16_t cmd) {
  switch (cmd) {
    case 0x006A: reply(0xD106); reply(0x000A); break; /* version */
    case 0x007A: reply(0x005A); reply(0x000A); break;
    case 0x008A: io.state = DCS_ECHO; break;          /* echo test: reply = ~word */
    case 0x009A: reply(0x6528); reply(0x000A); break; /* ROM checksum */
    case 0x003A: reply(0xCC01); reply(0x000A); break; /* memory tests pass */
    case 0x004A: reply(0xCC02); reply(0x000A); break;
    case 0x00AA: reply(0xCC04); reply(0x000A); break;
    case 0x001A: io.state = DCS_UPLOAD_HDR; io.hdr_n = 0; break;
    case 0x002A: /* start the uploaded program */
      g_dcs.running = 1;
      reply(0x000C);
      break;
    default:
      vegas_log("dcs: unknown boot monitor command %04X", cmd);
      reply(0x000A);
      break;
  }
}

/* Running program: we don't execute the DSP code yet. Accept all data; answer the few
   queries the game makes (values seen from the real board in MAME). */
static void dcs_running_word(uint16_t w) {
  switch (w) {
    case 0x55D4: reply(0x0236); break; /* program version/status query */
    default: break;
  }
}

static void dcs_write(uint16_t w) {
  g_dcs.words_received++;
  switch (io.state) {
    case DCS_RESET: break; /* held in reset: ignored */
    case DCS_ECHO:
      reply((uint16_t)~w);
      reply(0x000A);
      io.state = DCS_WAIT_CMD;
      break;
    case DCS_UPLOAD_HDR:
      io.hdr[io.hdr_n++] = w;
      if (io.hdr_n == 3) {
        const uint32_t words = (uint32_t)(io.hdr[1] - io.hdr[0] + 1);
        io.upload_left = io.hdr[2] == 0 ? words * 2 : words;
        io.upload_pos = 0;
        io.upload_sum = 0;
        io.state = io.upload_left ? DCS_UPLOAD_DATA : DCS_WAIT_CMD;
      }
      break;
    case DCS_UPLOAD_DATA: dcs_upload_word(w); break;
    default:
      if (g_dcs.running) {
        /* After start, 0x1A/0x2A are part of the program's own protocol (bank loads etc.). */
        dcs_running_word(w);
      } else {
        dcs_monitor_command(w);
      }
      break;
  }
}

/* ---- I/O ASIC registers ---- */
void ioasic_init(void) {
  memset(&io, 0, sizeof io);
  memset(&g_dcs, 0, sizeof g_dcs);
  io.regs[REG_CONTROL / 2] = 0x4000;
  io.regs[REG_SOUND_RESET / 2] = 1;
  /* Idle input values read on the real board (nothing pressed; DIP switches as shipped). */
  io.inputs[IOASIC_IN_A] = 0xFFFF;
  io.inputs[IOASIC_IN_B] = 0xF0FF;
  io.inputs[IOASIC_IN_C] = 0xFFFF;
  io.inputs[IOASIC_IN_DIP] = 0xBE7E;
  io.state = DCS_WAIT_CMD;
}

void ioasic_set_input(int port, uint16_t value) {
  if (port >= 0 && port < 4) io.inputs[port] = value;
}

int ioasic_handles(uint32_t p) { return p >= IOASIC_BASE && p < IOASIC_BASE + IOASIC_SIZE; }

static uint16_t read16(uint32_t off) {
  switch (off) {
    case REG_SOUND_STAT:
      return (uint16_t)(0x0008 | 0x0080 /* consumed instantly */ | (io.reply_count ? 0x0040 : 0));
    case REG_SOUND_IN:
      if (io.reply_count) {
        io.last_reply = io.replies[io.reply_head];
        io.reply_head = (io.reply_head + 1) % 64;
        io.reply_count--;
      }
      return io.last_reply;
    case REG_INPUT_A: return io.inputs[IOASIC_IN_A];
    case REG_INPUT_B: {
      /* Bits 8-11 follow the select register: 0 for selects 0-7, 0xF for 8/9, the select
         value itself for 0xA-0xF (measured). */
      const uint16_t sel = io.regs[REG_SELECT / 2] & 0xF;
      const uint16_t field = sel < 8 ? 0 : sel < 0xA ? 0xF : sel;
      return (uint16_t)((io.inputs[IOASIC_IN_B] & ~0x0F00u) | (field << 8));
    }
    case REG_INPUT_C: return io.inputs[IOASIC_IN_C];
    case REG_INPUT_DIP: return io.inputs[IOASIC_IN_DIP];
    default: return io.regs[(off / 2) & 0xF];
  }
}

static void write16(uint32_t off, uint16_t v) {
  switch (off) {
    case REG_SOUND_OUT: dcs_write(v); break;
    case REG_SOUND_RESET: {
      const uint16_t old = io.regs[off / 2];
      io.regs[off / 2] = v;
      if (!(v & 1)) io.state = DCS_RESET;
      else if (!(old & 1)) dcs_reset(); /* released from reset: board boots */
      break;
    }
    case REG_SOUND_IN: break; /* acknowledge */
    default: io.regs[(off / 2) & 0xF] = v; break;
  }
}

int ioasic_trace; /* debug: log register accesses */

uint32_t ioasic_read(uint32_t p, int size) {
  const uint32_t off = p - IOASIC_BASE;
  if (ioasic_trace) {
    uint32_t v = size == 4 ? (read16(off) | ((uint32_t)read16(off + 2) << 16)) : read16(off & ~1u);
    vegas_log("ioasic: read%d +%02X -> %04X", size * 8, off, v);
    return size == 1 ? ((off & 1) ? v >> 8 : v & 0xFF) : v;
  }
  if (size == 4) return read16(off) | ((uint32_t)read16(off + 2) << 16);
  const uint16_t v = read16(off & ~1u);
  return size == 1 ? ((off & 1) ? v >> 8 : v & 0xFF) : v;
}

void ioasic_write(uint32_t p, uint32_t value, int size) {
  const uint32_t off = p - IOASIC_BASE;
  if (ioasic_trace) vegas_log("ioasic: write%d +%02X <- %04X", size * 8, off, value);
  if (size == 4) {
    write16(off, (uint16_t)value);
    write16(off + 2, (uint16_t)(value >> 16));
  } else {
    write16(off & ~1u, (uint16_t)value);
  }
}
