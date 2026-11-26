// Widescreen ("Hor+") support: the game keeps its focal length and the renderer shows more of
// the game's X range around the centre (256 +/- 192 * aspect). The game itself rejects objects
// whose bounding boxes project outside its 512x384 screen; these patches widen those tests.
//
// Bounding-box visibility (found by tracing the vertex writer back from Glide):
//   801A0D18 projects the 8 box corners with centre (0x801FA5D4, 0x801FA5D8) and builds
//   outcodes against x in [0, 0x801FA5E0 = 512], y in [0, 0x801FA5DC = 384] (lower bounds are
//   a literal 0). 801A1008/801A1114 do the same with centre 0x801FA5E8 and max 0x801FA5F4.
// Shifting such a centre by +D and the X maximum by +2D accepts x in [-D, 512 + D] in the
// original coordinates. The drawing projection (801A12A8, centre 0x801FA5F8) is untouched.
// The per-triangle clipper (801A2C48) rejects, trivially accepts and clips against x = 0 and
// x = 512; its 512s are data (patched here) and its 0s are code (patched in the recompiler).
//
// Players are also culled as a whole before any of that (80135A40): in view space, a player is
// hidden when |x| > z * k, with k ending in a factor of 1.05 (0x801F2458, read only there) that
// fits the 4:3 view. Hor+ keeps the focal length, so the visible half-width at any depth grows by
// aspect / (4/3); scaling that factor by the same amount keeps players at the edges drawn.
#include "widescreen.h"

#include <string.h>

#include "recomp.h"

float recomp_ws_xmin;

static void mulf(uint32_t vaddr, float k) {
  uint8_t* p = recomp_rdram + (vaddr & 0x1FFFFFFFu);
  float f;
  memcpy(&f, p, 4);
  f *= k;
  memcpy(p, &f, 4);
}

static void addf(uint32_t vaddr, float delta) {
  uint8_t* p = recomp_rdram + (vaddr & 0x1FFFFFFFu);
  float f;
  memcpy(&f, p, 4);
  f += delta;
  memcpy(p, &f, 4);
}

void widescreen_patch(float aspect) {
  if (aspect <= 4.0f / 3.0f + 1e-4f) return;
  const float d = 192.0f * aspect - 256.0f; /* extra game X units on each side */
  addf(0x801FA5D4u, d);        /* bbox test centre X */
  addf(0x801FA5E0u, 2.0f * d); /* bbox test max X */
  addf(0x801FA5E8u, d);        /* second bbox test centre X */
  addf(0x801FA5F4u, 2.0f * d); /* second bbox test max X */
  /* 3D polygon clipper 801A2C48 (unshifted screen coordinates): accept/clip x in [-d, 512 + d].
     The left edge is read by patched instructions (tools/recomp main.cpp: insn_override). */
  recomp_ws_xmin = -d;
  addf(0x801FA648u, d); /* all vertices x >= 512: reject */
  addf(0x801FA64Cu, d); /* trivial accept x < 512 */
  addf(0x801FA65Cu, d); /* right clip edge */
  mulf(0x801F2458u, aspect * 0.75f); /* player visibility: horizontal view-space margin */
}
