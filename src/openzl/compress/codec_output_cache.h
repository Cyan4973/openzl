// Copyright (c) Meta Platforms, Inc. and affiliates.

#ifndef OPENZL_COMPRESS_CODEC_OUTPUT_CACHE_H
#define OPENZL_COMPRESS_CODEC_OUTPUT_CACHE_H

#include <stdbool.h> // bool
#include <stddef.h>  // size_t
#include <stdint.h>  // uint32_t, uint64_t, int32_t

#include "openzl/common/map.h"            // ZL_DECLARE_CUSTOM_MAP_TYPE
#include "openzl/shared/portability.h"    // ZL_BEGIN_C_DECLS
#include "openzl/zl_codec_output_cache.h" // ZL_CodecOutputCache
#include "openzl/zl_data.h"               // ZL_Type
#include "openzl/zl_errors.h"             // ZL_Report
#include "openzl/zl_opaque_types.h"       // ZL_CCtx

ZL_BEGIN_C_DECLS

/**
 * Composite key identifying a codec invocation by value:
 *   - a 128-bit hash of the concatenated input stream contents,
 *   - the codec id,
 *   - the output-affecting global parameters (format version, level),
 *   - and the codec's copy-parameters, compared exactly (byte for byte).
 * Only the 128-bit content hash is probabilistic; everything else is exact.
 */
typedef struct {
    uint64_t contentHashHigh;
    uint64_t contentHashLow;
    uint32_t codecID;
    uint32_t formatVersion;
    int32_t compressionLevel;
    uint32_t copyParamsSize;
    const void* copyParams; // borrowed at lookup; cache-arena-owned once stored
} COC_Key;

/** One memoized output stream of a codec invocation. */
typedef struct {
    ZL_Type type;
    size_t eltWidth;
    size_t numElts;
    size_t contentSize;
    const void* content; // cache-arena-owned once stored
} COC_Output;

/** The memoized result of a single codec invocation. */
typedef struct {
    size_t nbOutputs;
    const COC_Output* outputs; // array of nbOutputs, cache-arena-owned
    const void* header; // codec header bytes, cache-arena-owned (or NULL)
    size_t headerSize;
    bool hasHeader; // whether the codec emitted a transform header at all
} COC_Entry;

// Custom hash & equality over COC_Key (see map.h / ZL_DECLARE_CUSTOM_MAP_TYPE).
size_t COC_Map_hash(const COC_Key* key);
bool COC_Map_eq(const COC_Key* lhs, const COC_Key* rhs);

ZL_DECLARE_CUSTOM_MAP_TYPE(COC_Map, COC_Key, COC_Entry);

/** Reason a codec invocation was deliberately not cached (for stats). */
typedef enum {
    COC_SKIP_REFPARAM, // codec used reference parameters
    COC_SKIP_DICT,     // dict-backed codec
    COC_SKIP_OTHER,    // other (e.g. string-typed output not yet supported)
} COC_SkipReason;

/**
 * Look up a memoized result. Returns NULL on miss. The returned pointer stays
 * valid until the next reset/free of the cache. Updates hit/miss counters.
 */
const COC_Entry* COC_find(ZL_CodecOutputCache* cache, const COC_Key* key);

/**
 * Store a copy of (@p key, @p entry) into the cache. All referenced bytes
 * (copy-params, output contents, header) are copied into the cache's arena.
 * @return error if the byte budget would be exceeded (nothing is stored).
 */
ZL_Report COC_insert(
        ZL_CodecOutputCache* cache,
        const COC_Key* key,
        const COC_Entry* entry);

/** Record that a cacheable-looking invocation was skipped, for visibility. */
void COC_recordSkip(ZL_CodecOutputCache* cache, COC_SkipReason reason);

/** The cache attached to @p cctx, or NULL if none. Defined in cctx.c. */
ZL_CodecOutputCache* CCTX_getCodecOutputCache(const ZL_CCtx* cctx);

ZL_END_C_DECLS

#endif // OPENZL_COMPRESS_CODEC_OUTPUT_CACHE_H
