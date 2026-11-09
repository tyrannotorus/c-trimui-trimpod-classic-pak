/* What liba52 and its FFT need from Rockbox's codeclib.h, for building outside
 * a codec.  The Brick is little-endian aarch64, so the FFT uses its C paths. */
#ifndef PASSTHRUA_CODECLIB_H
#define PASSTHRUA_CODECLIB_H

#include <stdint.h>
#include <string.h>
#include "fft.h"

#define ROCKBOX_LITTLE_ENDIAN 1
#define ICODE_ATTR
#define ICONST_ATTR
#define IBSS_ATTR
#define IDATA_ATTR
#define LIKELY(x)   __builtin_expect(!!(x), 1)
#define UNLIKELY(x) __builtin_expect(!!(x), 0)
#define betoh32(x)  __builtin_bswap32(x)

extern void ff_fft_calc_c(int nbits, FFTComplex *z);

#endif
