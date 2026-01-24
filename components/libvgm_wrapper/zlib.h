// zlib.h shim - redirect to miniz for ESP-IDF compatibility
// miniz provides a subset of zlib API sufficient for libvgm's gzip decompression

#ifndef ZLIB_SHIM_H
#define ZLIB_SHIM_H

#include "miniz_full.h"

// miniz uses different names for some zlib constants/types
// Map zlib names to miniz names where needed

#ifndef z_const
#define z_const const
#endif

#ifndef Z_NULL
#define Z_NULL 0
#endif

// z_stream is typedef'd as mz_stream in miniz
typedef mz_stream z_stream;

// Bytef is defined in miniz as unsigned char
#ifndef Bytef
typedef unsigned char Bytef;
#endif

// Map function names (miniz already provides these with mz_ prefix and z_ aliases)
#ifndef inflateInit2
#define inflateInit2 mz_inflateInit2
#endif

#ifndef inflate
#define inflate mz_inflate
#endif

#ifndef inflateEnd
#define inflateEnd mz_inflateEnd
#endif

// Return codes (miniz defines Z_OK etc as aliases to MZ_OK etc)
#ifndef Z_OK
#define Z_OK MZ_OK
#endif

#ifndef Z_STREAM_END
#define Z_STREAM_END MZ_STREAM_END
#endif

#ifndef Z_SYNC_FLUSH
#define Z_SYNC_FLUSH MZ_SYNC_FLUSH
#endif

#endif // ZLIB_SHIM_H
