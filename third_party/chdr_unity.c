/* libchdr as a single translation unit. The submodule's own unity.c predates the A/V Huffman
   codec, so add it here (its codec table references it). */
#include "libchdr/unity.c"
#include "libchdr/src/libchdr_codec_avhuff.c"
