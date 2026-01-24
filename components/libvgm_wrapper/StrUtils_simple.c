/**
 * @file StrUtils_simple.c
 * @brief Simple string conversion stub for ESP-IDF (no iconv)
 *
 * This provides minimal character conversion functionality.
 * For VGM GD3 tags, it handles UTF-16LE to UTF-8 conversion
 * and passes UTF-8 strings through unchanged.
 */

#include <stdint.h>
#include <stdlib.h>
#include <string.h>

// libvgm headers
#include "stdtype.h"
#include "utils/StrUtils.h"

struct _codepage_conversion {
    char cpFrom[32];
    char cpTo[32];
};

/**
 * Initialize code page conversion
 * Returns 0 on success
 */
UINT8 CPConv_Init(CPCONV** retCPC, const char* cpFrom, const char* cpTo) {
    CPCONV* cpc = (CPCONV*)malloc(sizeof(CPCONV));
    if (cpc == NULL)
        return 0xFF;

    strncpy(cpc->cpFrom, cpFrom ? cpFrom : "", sizeof(cpc->cpFrom) - 1);
    cpc->cpFrom[sizeof(cpc->cpFrom) - 1] = '\0';
    strncpy(cpc->cpTo, cpTo ? cpTo : "", sizeof(cpc->cpTo) - 1);
    cpc->cpTo[sizeof(cpc->cpTo) - 1] = '\0';

    *retCPC = cpc;
    return 0x00;
}

/**
 * Deinitialize code page conversion
 */
void CPConv_Deinit(CPCONV* cpc) {
    if (cpc != NULL)
        free(cpc);
}

/**
 * Convert UTF-16LE to UTF-8
 */
static size_t utf16le_to_utf8(const UINT8* src, size_t srcLen, char* dst, size_t dstLen) {
    size_t dstPos = 0;
    size_t srcPos = 0;

    while (srcPos + 1 < srcLen && dstPos < dstLen - 1) {
        // Read UTF-16LE character
        UINT16 ch = src[srcPos] | (src[srcPos + 1] << 8);
        srcPos += 2;

        if (ch == 0) {
            break;  // Null terminator
        } else if (ch < 0x80) {
            // ASCII (1 byte UTF-8)
            dst[dstPos++] = (char)ch;
        } else if (ch < 0x800) {
            // 2-byte UTF-8
            if (dstPos + 2 > dstLen - 1) break;
            dst[dstPos++] = 0xC0 | (ch >> 6);
            dst[dstPos++] = 0x80 | (ch & 0x3F);
        } else {
            // 3-byte UTF-8 (covers BMP)
            if (dstPos + 3 > dstLen - 1) break;
            dst[dstPos++] = 0xE0 | (ch >> 12);
            dst[dstPos++] = 0x80 | ((ch >> 6) & 0x3F);
            dst[dstPos++] = 0x80 | (ch & 0x3F);
        }
    }

    dst[dstPos] = '\0';
    return dstPos;
}

/**
 * Convert string between code pages
 * For ESP-IDF, we support:
 * - UTF-16LE -> UTF-8
 * - Pass-through for same encoding or unknown
 */
UINT8 CPConv_StrConvert(CPCONV* cpc, size_t* outSize, char** outStr,
                        size_t inSize, const char* inStr) {
    if (cpc == NULL || outSize == NULL || outStr == NULL)
        return 0xFF;

    if (inStr == NULL || inSize == 0) {
        *outSize = 0;
        *outStr = NULL;
        return 0x00;
    }

    // Allocate output buffer (worst case: 3 bytes per UTF-16 character + null)
    size_t maxOutSize = inSize * 3 + 1;
    char* out = (char*)malloc(maxOutSize);
    if (out == NULL) {
        *outSize = 0;
        *outStr = NULL;
        return 0xFF;
    }

    // Check if converting from UTF-16
    int isUtf16 = (strstr(cpc->cpFrom, "UTF-16") != NULL ||
                   strstr(cpc->cpFrom, "UCS-2") != NULL);

    if (isUtf16) {
        // Convert UTF-16LE to UTF-8
        size_t converted = utf16le_to_utf8((const UINT8*)inStr, inSize, out, maxOutSize);
        *outSize = converted;
    } else {
        // Pass-through (assume UTF-8 or compatible)
        size_t copyLen = inSize;
        if (copyLen >= maxOutSize) copyLen = maxOutSize - 1;
        memcpy(out, inStr, copyLen);
        out[copyLen] = '\0';
        *outSize = copyLen;
    }

    *outStr = out;
    return 0x00;
}
