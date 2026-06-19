// Copyright (c) Meta Platforms, Inc. and affiliates.

#ifndef OPENZL_ZL_CODEC_OUTPUT_CACHE_H
#define OPENZL_ZL_CODEC_OUTPUT_CACHE_H

#include <stddef.h>                 // size_t
#include "openzl/zl_errors.h"       // ZL_Report
#include "openzl/zl_opaque_types.h" // ZL_CCtx

#if defined(__cplusplus)
extern "C" {
#endif

/**
 * ZL_CodecOutputCache  (advanced, experimental)
 * ---------------------------------------------
 * Memoizes the *output* of individual codec invocations, keyed by the content
 * of the input(s) plus the codec identity, its parameters, and the
 * output-affecting global parameters (format version, level). When the exact
 * same (input, codec, params) combination is compressed again, the cached
 * output is replayed instead of re-running the codec, while the surrounding
 * frame is still regenerated normally -- so the produced frame is byte-for-byte
 * identical to a run without the cache.
 *
 * This is purely a speed optimization for workloads that repeatedly compress
 * related inputs through overlapping graphs (e.g. exhaustive graph search /
 * bruteforce evaluation, or the automatic graph selector trying many candidate
 * graphs on the same input). It is OFF by default: a CCtx ignores the cache
 * until one is attached with ZL_CCtx_setCodecOutputCache().
 *
 * Correctness invariant: a codec output is a pure, deterministic function of
 * its input bytes, codec id, (copy) parameters and the format/level. Codec
 * invocations whose output could depend on anything else are NOT cached:
 *   - codecs using reference parameters (refParams), whose referenced content
 *     cannot be proven identical from the cache key, and
 *   - dict-backed codecs.
 * Such invocations always run normally; enabling the cache can therefore only
 * ever make compression faster, never change its output.
 *
 * Threading: a cache is mutable and single-writer. It may be reused across
 * several CCtx *sequentially within one thread* (e.g. across evaluation
 * batches), but must NOT be shared by CCtx running concurrently.
 *
 * Memory: all cached bytes are owned by the cache and released in one shot by
 * ZL_CodecOutputCache_free() (or ZL_CodecOutputCache_reset()). A byte budget
 * bounds the cache; exceeding it makes the offending compression fail with an
 * error rather than silently degrading.
 */
typedef struct ZL_CodecOutputCache_s ZL_CodecOutputCache;

/** Counters describing cache activity. All monotonically increasing except
 *  @p bytesStored, which tracks the bytes currently held. */
typedef struct {
    size_t hits;          // codec invocations served from the cache
    size_t misses;        // cacheable invocations that ran and were stored
    size_t refParamSkips; // invocations not cached: codec used refParams
    size_t dictSkips;     // invocations not cached: dict-backed codec
    size_t otherSkips;    // invocations not cached: other (e.g. string output)
    size_t bytesStored;   // payload bytes currently held
    size_t overflowed;    // nb of times the byte budget was exceeded
} ZL_CodecOutputCache_Stats;

/**
 * Create an empty cache.
 * @param maxBytes byte budget for cached payloads; 0 selects a default (2 GB).
 * @return the cache, or NULL on allocation failure.
 */
ZL_CodecOutputCache* ZL_CodecOutputCache_create(size_t maxBytes);

/** Release the cache and all the memory it owns. NULL is accepted. */
void ZL_CodecOutputCache_free(ZL_CodecOutputCache* cache);

/** Drop all cached entries, keeping the (empty) cache reusable. */
void ZL_CodecOutputCache_reset(ZL_CodecOutputCache* cache);

/** Snapshot of the cache counters. */
ZL_CodecOutputCache_Stats ZL_CodecOutputCache_getStats(
        const ZL_CodecOutputCache* cache);

/**
 * Attach (or, with @p cache == NULL, detach) a cache to a compression context.
 * The cache is borrowed: it is not owned by the CCtx and must outlive every
 * compression that uses it. Off by default.
 */
ZL_Report ZL_CCtx_setCodecOutputCache(
        ZL_CCtx* cctx,
        ZL_CodecOutputCache* cache);

#if defined(__cplusplus)
}
#endif

#endif // OPENZL_ZL_CODEC_OUTPUT_CACHE_H
