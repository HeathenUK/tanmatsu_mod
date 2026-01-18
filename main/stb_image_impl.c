// stb_image implementation file
// This file compiles the stb_image single-header library

#include "esp_heap_caps.h"
#include <stdlib.h>

// Use PSRAM for stb_image allocations to avoid stack overflow
#define STBI_MALLOC(sz)        heap_caps_malloc(sz, MALLOC_CAP_SPIRAM)
#define STBI_REALLOC(p,newsz)  heap_caps_realloc(p, newsz, MALLOC_CAP_SPIRAM)
#define STBI_FREE(p)           heap_caps_free(p)

#define STB_IMAGE_IMPLEMENTATION
#define STBI_ONLY_PNG          // Only need PNG support for F-key icons
#define STBI_NO_STDIO          // Use memory loading, not file I/O (we'll read files ourselves)
#define STBI_NO_FAILURE_STRINGS // Reduce binary size
#define STBI_NO_LINEAR         // Don't need linear light conversion
#define STBI_NO_HDR            // Don't need HDR support

#include "stb_image.h"
