// Copyright (c) Meta Platforms, Inc. and affiliates.

#include "openzl/compress/codec_output_cache.h"

#include <string.h> // memcpy, memcmp, memset

#include "openzl/common/allocation.h"      // Arena, ALLOC_*, ZL_malloc, ZL_free
#include "openzl/common/assertion.h"       // ZL_ASSERT_NN
#include "openzl/common/errors_internal.h" // ZL_ERR, ZL_RESULT_*
#include "openzl/shared/xxhash.h"          // XXH3_*

// Default byte budget: large enough never to trip in normal use, but a guard
// against degenerate cases. Matches OpenZL's arena-size convention (~1-2 GB).
#define COC_DEFAULT_MAX_BYTES ((size_t)2 << 30)
// Ceiling on the number of distinct entries; the byte budget is the real limit.
#define COC_MAX_ENTRIES (1u << 24)

struct ZL_CodecOutputCache_s {
    Arena* arena; // owns the map and every cached blob
    COC_Map map;
    size_t maxBytes;
    size_t curBytes;
    ZL_CodecOutputCache_Stats stats;
};

// ---------------------------------------------------------------------------
// Composite key hash & equality
// ---------------------------------------------------------------------------

size_t COC_Map_hash(const COC_Key* key)
{
    XXH3_state_t hs;
    XXH3_INITSTATE(&hs);
    XXH3_64bits_reset(&hs);
    XXH3_64bits_update(
            &hs, &key->contentHashHigh, sizeof(key->contentHashHigh));
    XXH3_64bits_update(&hs, &key->contentHashLow, sizeof(key->contentHashLow));
    XXH3_64bits_update(&hs, &key->codecID, sizeof(key->codecID));
    XXH3_64bits_update(&hs, &key->formatVersion, sizeof(key->formatVersion));
    XXH3_64bits_update(
            &hs, &key->compressionLevel, sizeof(key->compressionLevel));
    if (key->copyParamsSize != 0) {
        XXH3_64bits_update(&hs, key->copyParams, key->copyParamsSize);
    }
    return (size_t)XXH3_64bits_digest(&hs);
}

bool COC_Map_eq(const COC_Key* lhs, const COC_Key* rhs)
{
    if (lhs->contentHashHigh != rhs->contentHashHigh
        || lhs->contentHashLow != rhs->contentHashLow
        || lhs->codecID != rhs->codecID
        || lhs->formatVersion != rhs->formatVersion
        || lhs->compressionLevel != rhs->compressionLevel
        || lhs->copyParamsSize != rhs->copyParamsSize) {
        return false;
    }
    // Copy-params are compared exactly so two nodes that share a codec but
    // differ only in parameters (e.g. tokenize vs tokenize_sort) never collide.
    if (lhs->copyParamsSize != 0
        && memcmp(lhs->copyParams, rhs->copyParams, lhs->copyParamsSize) != 0) {
        return false;
    }
    return true;
}

// ---------------------------------------------------------------------------
// Lifecycle
// ---------------------------------------------------------------------------

ZL_CodecOutputCache* ZL_CodecOutputCache_create(size_t maxBytes)
{
    ZL_CodecOutputCache* cache = ZL_malloc(sizeof(*cache));
    if (cache == NULL) {
        return NULL;
    }
    memset(cache, 0, sizeof(*cache));
    cache->arena = ALLOC_HeapArena_create();
    if (cache->arena == NULL) {
        ZL_free(cache);
        return NULL;
    }
    cache->maxBytes = (maxBytes != 0) ? maxBytes : COC_DEFAULT_MAX_BYTES;
    cache->map      = COC_Map_createInArena(cache->arena, COC_MAX_ENTRIES);
    return cache;
}

void ZL_CodecOutputCache_free(ZL_CodecOutputCache* cache)
{
    if (cache == NULL) {
        return;
    }
    ALLOC_Arena_freeArena(cache->arena);
    ZL_free(cache);
}

void ZL_CodecOutputCache_reset(ZL_CodecOutputCache* cache)
{
    if (cache == NULL) {
        return;
    }
    // Drop every blob and the map in one shot, then re-arm an empty map.
    ALLOC_Arena_freeAll(cache->arena);
    cache->curBytes = 0;
    memset(&cache->stats, 0, sizeof(cache->stats));
    cache->map = COC_Map_createInArena(cache->arena, COC_MAX_ENTRIES);
}

ZL_CodecOutputCache_Stats ZL_CodecOutputCache_getStats(
        const ZL_CodecOutputCache* cache)
{
    ZL_ASSERT_NN(cache);
    return cache->stats;
}

void COC_recordSkip(ZL_CodecOutputCache* cache, COC_SkipReason reason)
{
    if (cache == NULL) {
        return;
    }
    switch (reason) {
        case COC_SKIP_REFPARAM:
            cache->stats.refParamSkips++;
            break;
        case COC_SKIP_DICT:
            cache->stats.dictSkips++;
            break;
        case COC_SKIP_OTHER:
            cache->stats.otherSkips++;
            break;
    }
}

// ---------------------------------------------------------------------------
// Lookup & insertion
// ---------------------------------------------------------------------------

const COC_Entry* COC_find(ZL_CodecOutputCache* cache, const COC_Key* key)
{
    ZL_ASSERT_NN(cache);
    ZL_ASSERT_NN(key);
    const COC_Map_Entry* entry = COC_Map_find(&cache->map, key);
    if (entry == NULL) {
        cache->stats.misses++;
        return NULL;
    }
    cache->stats.hits++;
    return &entry->val;
}

ZL_Report COC_insert(
        ZL_CodecOutputCache* cache,
        const COC_Key* key,
        const COC_Entry* entry)
{
    ZL_RESULT_DECLARE_SCOPE_REPORT(NULL);
    ZL_ASSERT_NN(cache);
    ZL_ASSERT_NN(key);
    ZL_ASSERT_NN(entry);

    // Account for all bytes we are about to copy into the arena.
    size_t need = key->copyParamsSize + entry->headerSize
            + entry->nbOutputs * sizeof(COC_Output);
    for (size_t i = 0; i < entry->nbOutputs; i++) {
        need += entry->outputs[i].contentSize;
    }
    if (cache->curBytes + need > cache->maxBytes) {
        cache->stats.overflowed++;
        ZL_ERR(allocation,
               "ZL_CodecOutputCache byte budget (%zu) exceeded",
               cache->maxBytes);
    }

    // Copy the copy-params blob into the cache arena.
    COC_Key k = *key;
    if (key->copyParamsSize != 0) {
        void* p = ALLOC_Arena_malloc(cache->arena, key->copyParamsSize);
        ZL_ERR_IF_NULL(p, allocation);
        memcpy(p, key->copyParams, key->copyParamsSize);
        k.copyParams = p;
    } else {
        k.copyParams = NULL;
    }

    // Copy the output descriptors and their contents.
    COC_Entry v = *entry;
    if (entry->nbOutputs != 0) {
        COC_Output* outs = ALLOC_Arena_malloc(
                cache->arena, entry->nbOutputs * sizeof(COC_Output));
        ZL_ERR_IF_NULL(outs, allocation);
        for (size_t i = 0; i < entry->nbOutputs; i++) {
            outs[i] = entry->outputs[i];
            if (entry->outputs[i].contentSize != 0) {
                void* c = ALLOC_Arena_malloc(
                        cache->arena, entry->outputs[i].contentSize);
                ZL_ERR_IF_NULL(c, allocation);
                memcpy(c,
                       entry->outputs[i].content,
                       entry->outputs[i].contentSize);
                outs[i].content = c;
            } else {
                outs[i].content = NULL;
            }
        }
        v.outputs = outs;
    } else {
        v.outputs = NULL;
    }

    // Copy the codec header, if any.
    if (entry->hasHeader && entry->headerSize != 0) {
        void* h = ALLOC_Arena_malloc(cache->arena, entry->headerSize);
        ZL_ERR_IF_NULL(h, allocation);
        memcpy(h, entry->header, entry->headerSize);
        v.header = h;
    } else {
        v.header = NULL;
    }

    COC_Map_Entry me        = { .key = k, .val = v };
    COC_Map_Insert inserted = COC_Map_insert(&cache->map, &me);
    if (inserted.badAlloc) {
        ZL_ERR(allocation, "ZL_CodecOutputCache insertion failed");
    }
    // The caller only inserts after a miss, so the key should be new. If it is
    // somehow already present the freshly-copied blobs simply stay in the arena
    // until reset; this is harmless and never happens on the intended path.
    ZL_ASSERT(inserted.inserted);
    if (inserted.inserted) {
        cache->curBytes += need;
        cache->stats.bytesStored = cache->curBytes;
    }
    return ZL_returnSuccess();
}
