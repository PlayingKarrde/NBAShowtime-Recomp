// Voodoo Banshee 3D engine state: register file, triangle setup, texture/LFB writes.
// Fed by the command FIFO parser (banshee.c) and by direct register writes. Each triangle is
// handed to the active renderer backend together with a snapshot of the pipeline state.
#ifndef NBA_VOODOO3D_H
#define NBA_VOODOO3D_H

#include <stdint.h>

/* 3D register indices (byte offset / 4) */
enum {
  V3D_status = 0x000 / 4,
  V3D_vertexAx = 0x008 / 4,
  V3D_triangleCMD = 0x080 / 4,
  V3D_fvertexAx = 0x088 / 4,
  V3D_ftriangleCMD = 0x100 / 4,
  V3D_fbzColorPath = 0x104 / 4,
  V3D_fogMode = 0x108 / 4,
  V3D_alphaMode = 0x10C / 4,
  V3D_fbzMode = 0x110 / 4,
  V3D_lfbMode = 0x114 / 4,
  V3D_clipLeftRight = 0x118 / 4,
  V3D_clipLowYHighY = 0x11C / 4,
  V3D_nopCMD = 0x120 / 4,
  V3D_fastfillCMD = 0x124 / 4,
  V3D_swapbufferCMD = 0x128 / 4,
  V3D_fogColor = 0x12C / 4,
  V3D_zaColor = 0x130 / 4,
  V3D_chromaKey = 0x134 / 4,
  V3D_chromaRange = 0x138 / 4,
  V3D_userIntrCMD = 0x13C / 4,
  V3D_stipple = 0x140 / 4,
  V3D_color0 = 0x144 / 4,
  V3D_color1 = 0x148 / 4,
  V3D_fogTable = 0x160 / 4, /* 32 entries */
  V3D_colBufferAddr = 0x1EC / 4,
  V3D_colBufferStride = 0x1F0 / 4,
  V3D_auxBufferAddr = 0x1F4 / 4,
  V3D_auxBufferStride = 0x1F8 / 4,
  V3D_clipLeftRight1 = 0x200 / 4,
  V3D_clipTopBottom1 = 0x204 / 4,
  V3D_swapPending = 0x24C / 4,
  V3D_leftOverlayBuf = 0x250 / 4,
  V3D_rightOverlayBuf = 0x254 / 4,
  V3D_sSetupMode = 0x260 / 4,
  V3D_sVx = 0x264 / 4,
  V3D_sVy = 0x268 / 4,
  V3D_sARGB = 0x26C / 4,
  V3D_sRed = 0x270 / 4,
  V3D_sGreen = 0x274 / 4,
  V3D_sBlue = 0x278 / 4,
  V3D_sAlpha = 0x27C / 4,
  V3D_sVz = 0x280 / 4,
  V3D_sWb = 0x284 / 4,
  V3D_sWtmu0 = 0x288 / 4,
  V3D_sS_W0 = 0x28C / 4,
  V3D_sT_W0 = 0x290 / 4,
  V3D_sWtmu1 = 0x294 / 4,
  V3D_sS_Wtmu1 = 0x298 / 4,
  V3D_sT_Wtmu1 = 0x29C / 4,
  V3D_sDrawTriCMD = 0x2A0 / 4,
  V3D_sBeginTriCMD = 0x2A4 / 4,
  V3D_textureMode = 0x300 / 4,
  V3D_tLOD = 0x304 / 4,
  V3D_tDetail = 0x308 / 4,
  V3D_texBaseAddr = 0x30C / 4,
  V3D_texBaseAddr_1 = 0x310 / 4,
  V3D_texBaseAddr_2 = 0x314 / 4,
  V3D_texBaseAddr_3_8 = 0x318 / 4,
  V3D_trexInit1 = 0x320 / 4,
  V3D_nccTable = 0x324 / 4, /* 2 tables x 12 words */
  V3D_REG_COUNT = 0x100
};

/* Setup-unit vertex (screen space, as Glide sends it). */
typedef struct {
  float x, y;
  float r, g, b, a; /* 0..255 */
  float z;          /* 0..65535 (depth buffer units) */
  float wb, w0, w1; /* 1/w */
  float s0, t0, s1, t1; /* s/w, t/w in texels of the 256-wide LOD 0 */
} V3dVertex;

/* Pipeline state captured with each triangle. */
typedef struct {
  uint32_t fbzColorPath, fogMode, alphaMode, fbzMode;
  uint32_t clipLeftRight, clipLowYHighY;
  uint32_t fogColor, zaColor, chromaKey, chromaRange, stipple, color0, color1;
  uint32_t colBufferAddr, colBufferStride, auxBufferAddr, auxBufferStride;
  uint32_t textureMode, tLOD, tDetail, texBaseAddr, texBaseAddr_1, texBaseAddr_2, texBaseAddr_3_8;
  uint32_t ncc_select_table; /* nccTable index in use (texture palette / NCC) */
  uint8_t fog_table[64][2];  /* fog blend, delta */
} V3dState;

/* Renderer backend. All callbacks are optional. */
typedef struct {
  void (*triangle)(void* user, const V3dState* st, const V3dVertex v[3]);
  void (*fastfill)(void* user, const V3dState* st);
  void (*swap)(void* user, uint32_t display_addr);
  void (*texture_written)(void* user, uint32_t vram_addr, uint32_t bytes);
  void* user;
} V3dBackend;

void v3d_init(uint8_t* vram, uint32_t vram_mask);
void v3d_set_backend(const V3dBackend* be);

/* register write: regnum = byte offset / 4 (0..0xFF), chipmask from the address */
void v3d_reg_write(uint32_t regnum, uint32_t chipmask, uint32_t data);
uint32_t v3d_reg_read(uint32_t regnum);
/* FIFO packet type 3 (setup-unit vertices): body size in words, and execution (returns words used) */
uint32_t v3d_packet3_size(uint32_t cmd);
uint32_t v3d_packet3(uint32_t cmd, const uint32_t* body);
/* texture port (FIFO packet 5, space 3): word offset from the LOD 0 base */
void v3d_texture_write(uint32_t word_offset, uint32_t data);
/* 3D LFB write (FIFO packet 5 space 2 or BAR1 + 16 MB): word offset in LFB space */
void v3d_lfb_write(uint32_t word_offset, uint32_t data, uint32_t mem_mask);

/* palette for 8-bit palettised textures (written through nccTable when bit 31 is set) */
extern uint32_t g_v3d_palette[256];

typedef struct {
  uint64_t triangles, fastfills, swaps, texture_words, lfb_words, culled;
  uint64_t reg_writes[V3D_REG_COUNT];
  uint64_t tex_format_tris[16];
  uint64_t packets[8];
} V3dStats;
extern V3dStats g_v3d_stats;

#endif
