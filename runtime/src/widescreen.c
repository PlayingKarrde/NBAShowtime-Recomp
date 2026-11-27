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

#ifndef RECOMP_ENGINE_DYNAMIC
float recomp_ws_xmin;
#endif

/* The patched values, as the game image has them (saved on the first call). */
static const uint32_t k_addr[] = {0x801FA5D4u, 0x801FA5E0u, 0x801FA5E8u, 0x801FA5F4u,
                                  0x801FA648u, 0x801FA64Cu, 0x801FA65Cu, 0x801F2458u};
enum { N_PATCH = sizeof k_addr / sizeof k_addr[0] };

static float* ram_f(uint32_t vaddr) { return (float*)(recomp_rdram + (vaddr & 0x1FFFFFFFu)); }

static void setf(int i, float v) { memcpy(ram_f(k_addr[i]), &v, 4); }

/* Can be called again between frames (e.g. after a window resize): values are set from the
   originals each time, so repeated changes do not accumulate rounding. */
void widescreen_patch(float aspect) {
  static float orig[N_PATCH];
  static int saved;
  static float cur = -1.0f;
  if (!saved) {
    for (int i = 0; i < N_PATCH; ++i) memcpy(&orig[i], ram_f(k_addr[i]), 4);
    saved = 1;
  }
  if (aspect < 4.0f / 3.0f + 1e-4f) aspect = 4.0f / 3.0f;
  if (aspect == cur) return;
  cur = aspect;
  const float d = aspect == 4.0f / 3.0f ? 0.0f : 192.0f * aspect - 256.0f; /* extra game X units on each side */
  setf(0, orig[0] + d);        /* bbox test centre X */
  setf(1, orig[1] + 2.0f * d); /* bbox test max X */
  setf(2, orig[2] + d);        /* second bbox test centre X */
  setf(3, orig[3] + 2.0f * d); /* second bbox test max X */
  /* 3D polygon clipper 801A2C48 (unshifted screen coordinates): accept/clip x in [-d, 512 + d].
     The left edge is read by patched instructions (tools/recomp main.cpp: insn_override). */
  recomp_ws_xmin = -d;
  setf(4, orig[4] + d); /* all vertices x >= 512: reject */
  setf(5, orig[5] + d); /* trivial accept x < 512 */
  setf(6, orig[6] + d); /* right clip edge */
  setf(7, orig[7] * (d == 0.0f ? 1.0f : aspect * 0.75f)); /* player visibility: horizontal view-space margin */
}
