/**
 * @file MemoryLoader_simple.c
 * @brief Simple memory loader for libvgm without zlib dependency
 *
 * This is a simplified version of MemoryLoader.c that doesn't require zlib.
 * VGZ decompression is handled separately in vgm_backend.cpp.
 */

#include <stdlib.h>
#include <string.h>
#include <stdio.h>  // For SEEK_SET, SEEK_CUR, SEEK_END

#include "common_def.h"
#include "utils/DataLoader.h"
#include "utils/MemoryLoader.h"

typedef struct _memory_loader {
    const UINT8 *data;
    UINT32 dataLen;
    UINT32 pos;
} MEMORY_LOADER;

static UINT8 MemLoad_dopen(void *context) {
    MEMORY_LOADER *loader = (MEMORY_LOADER *)context;
    loader->pos = 0;
    return 0;
}

static UINT32 MemLoad_dread(void *context, UINT8 *buffer, UINT32 numBytes) {
    MEMORY_LOADER *loader = (MEMORY_LOADER *)context;
    UINT32 bytesLeft = loader->dataLen - loader->pos;
    if (numBytes > bytesLeft)
        numBytes = bytesLeft;
    if (numBytes > 0) {
        memcpy(buffer, loader->data + loader->pos, numBytes);
        loader->pos += numBytes;
    }
    return numBytes;
}

static UINT8 MemLoad_dseek(void *context, UINT32 offset, UINT8 whence) {
    MEMORY_LOADER *loader = (MEMORY_LOADER *)context;
    switch (whence) {
        case SEEK_SET:
            loader->pos = offset;
            break;
        case SEEK_CUR:
            loader->pos += offset;
            break;
        case SEEK_END:
            loader->pos = loader->dataLen + offset;
            break;
        default:
            return 1;
    }
    if (loader->pos > loader->dataLen)
        loader->pos = loader->dataLen;
    return 0;
}

static UINT8 MemLoad_dclose(void *context) {
    (void)context;
    return 0;
}

static INT32 MemLoad_dtell(void *context) {
    MEMORY_LOADER *loader = (MEMORY_LOADER *)context;
    return (INT32)loader->pos;
}

static UINT32 MemLoad_dlength(void *context) {
    MEMORY_LOADER *loader = (MEMORY_LOADER *)context;
    return loader->dataLen;
}

static UINT8 MemLoad_deof(void *context) {
    MEMORY_LOADER *loader = (MEMORY_LOADER *)context;
    return (loader->pos >= loader->dataLen) ? 1 : 0;
}

static void MemLoad_ddeinit(void *context) {
    MEMORY_LOADER *loader = (MEMORY_LOADER *)context;
    free(loader);
}

const DATA_LOADER_CALLBACKS memoryLoader = {
    0x4D454D4C,  // 'MEML'
    "Memory Loader",
    MemLoad_dopen,
    MemLoad_dread,
    MemLoad_dseek,
    MemLoad_dclose,
    MemLoad_dtell,
    MemLoad_dlength,
    MemLoad_deof,
    MemLoad_ddeinit
};

DATA_LOADER *MemoryLoader_Init(const UINT8 *buffer, UINT32 length) {
    DATA_LOADER *dLoad = (DATA_LOADER *)calloc(1, sizeof(DATA_LOADER));
    if (dLoad == NULL)
        return NULL;

    MEMORY_LOADER *memLoad = (MEMORY_LOADER *)calloc(1, sizeof(MEMORY_LOADER));
    if (memLoad == NULL) {
        free(dLoad);
        return NULL;
    }

    memLoad->data = buffer;
    memLoad->dataLen = length;
    memLoad->pos = 0;

    DataLoader_Setup(dLoad, &memoryLoader, memLoad);
    return dLoad;
}
