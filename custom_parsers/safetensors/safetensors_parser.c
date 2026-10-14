// Copyright (c) Meta Platforms, Inc. and affiliates.

#include "custom_parsers/safetensors/safetensors_parser.h"

#include <stdbool.h>
#include <stdlib.h>

#include "custom_parsers/safetensors/safetensors_lexer.h"
#include "openzl/codecs/zl_ace.h"
#include "openzl/codecs/zl_conversion.h"
#include "openzl/codecs/zl_entropy.h"
#include "openzl/codecs/zl_float_deconstruct.h"
#include "openzl/codecs/zl_generic.h"
#include "openzl/codecs/zl_split_by_struct.h"
#include "openzl/codecs/zl_store.h"
#include "openzl/codecs/zl_transformer.h"
#include "openzl/codecs/zl_zstd.h"
#include "openzl/common/assertion.h"
#include "openzl/common/errors_internal.h"
#include "openzl/shared/mem.h"
#include "openzl/shared/utils.h"
#include "openzl/zl_compress.h"
#include "openzl/zl_errors.h"
#include "openzl/zl_graph_api.h"
#include "openzl/zl_segmenter.h"
#include "openzl/zl_version.h"

#define ST_CHUNK_SIZE_MAX_PID 1
#define ST_SEGMENT_SIZES_PID 2
#define ST_SEGMENT_TAGS_PID 3

// A float tensor is atypical when an evenly spaced sample of its elements has
// fewer than 1/ST_DISTINCT_RATIO distinct values (masks, constants, ...).
#define ST_SAMPLE_SIZE 4096
#define ST_MIN_ELTS_FOR_SAMPLING 64
#define ST_DISTINCT_RATIO 8

// A chunk cannot hold more than ZL_runtimeNodeLimit() nodes (20000). Measured
// costs: up to ~39 nodes per segment through the generic graph (int64, f64),
// ~15 through the Transformer, and ~16 per MiB of float data. Merging small
// neighbors and these two caps keep a chunk under ~11000 nodes, leaving room
// for trained successors.
#define ST_SMALL_SEGMENT_SIZE ((size_t)16 << 10)
#define ST_MAX_SEGMENTS_PER_CHUNK 256
#define ST_CHUNK_SIZE_LIMIT ((size_t)64 << 20)

typedef enum {
    ST_Successor_header = 0,
    ST_Successor_raw,
    ST_Successor_int8,
    ST_Successor_int16,
    ST_Successor_int32,
    ST_Successor_int64,
    ST_Successor_f16,
    ST_Successor_bf16,
    ST_Successor_f32,
    ST_Successor_f64,
    ST_Successor_atypical16, // low-cardinality 16-bit floats
    ST_Successor_atypical32, // low-cardinality 32-bit floats
    ST_Successor_count,
} ST_Successor;

typedef struct {
    size_t size;
    uint32_t tag;
    size_t splitAlign; // byte granularity when the segment must be split
} ST_Segment;

typedef struct {
    ZL_Segmenter* sctx;
    ZL_GraphID innerGraph;
    size_t chunkSizeMax;
    size_t* sizes;
    uint32_t* tags;
    size_t capacity;
    size_t nbSegments;
    size_t nbBytes;
} ST_Chunker;

static ST_Successor ST_successorOf(ZL_SafetensorsDtype dtype)
{
    switch (dtype) {
        case ZL_SafetensorsDtype_bool:
        case ZL_SafetensorsDtype_u8:
        case ZL_SafetensorsDtype_i8:
        case ZL_SafetensorsDtype_f8_e4m3:
        case ZL_SafetensorsDtype_f8_e5m2:
            return ST_Successor_int8;
        case ZL_SafetensorsDtype_i16:
        case ZL_SafetensorsDtype_u16:
            return ST_Successor_int16;
        case ZL_SafetensorsDtype_i32:
        case ZL_SafetensorsDtype_u32:
            return ST_Successor_int32;
        case ZL_SafetensorsDtype_i64:
        case ZL_SafetensorsDtype_u64:
            return ST_Successor_int64;
        case ZL_SafetensorsDtype_f16:
            return ST_Successor_f16;
        case ZL_SafetensorsDtype_bf16:
            return ST_Successor_bf16;
        case ZL_SafetensorsDtype_f32:
            return ST_Successor_f32;
        case ZL_SafetensorsDtype_f64:
            return ST_Successor_f64;
        case ZL_SafetensorsDtype_unknown:
        default:
            return ST_Successor_raw;
    }
}

/* ------------------------------------------------------------------------
 * Inner graph: splits a chunk into its segments and routes them by tag.
 * ------------------------------------------------------------------------ */

static ZL_Report
ST_innerGraphFn(ZL_Graph* graph, ZL_Edge* inputs[], size_t nbInputs)
{
    ZL_RESULT_DECLARE_SCOPE_REPORT(graph);
    ZL_ERR_IF_NE(nbInputs, 1, graph_invalidNumInputs);

    const ZL_RefParam sizesParam =
            ZL_Graph_getLocalRefParam(graph, ST_SEGMENT_SIZES_PID);
    const ZL_RefParam tagsParam =
            ZL_Graph_getLocalRefParam(graph, ST_SEGMENT_TAGS_PID);
    ZL_ERR_IF_NE(sizesParam.paramId, ST_SEGMENT_SIZES_PID, graph_invalid);
    ZL_ERR_IF_NE(tagsParam.paramId, ST_SEGMENT_TAGS_PID, graph_invalid);
    const size_t* const sizes  = (const size_t*)sizesParam.paramRef;
    const uint32_t* const tags = (const uint32_t*)tagsParam.paramRef;
    const size_t nbSegments    = sizesParam.paramSize / sizeof(size_t);
    ZL_ERR_IF_NE(
            tagsParam.paramSize / sizeof(uint32_t), nbSegments, graph_invalid);

    const ZL_GraphIDList successors = ZL_Graph_getCustomGraphs(graph);
    ZL_ERR_IF_NE(successors.nbGraphIDs, ST_Successor_count, graph_invalid);

    size_t total = 0;
    for (size_t i = 0; i < nbSegments; ++i) {
        ZL_ERR_IF_GE(tags[i], ST_Successor_count, graph_invalid);
        total += sizes[i];
    }
    ZL_ERR_IF_NE(
            total,
            ZL_Input_contentSize(ZL_Edge_getData(inputs[0])),
            graph_invalid);
    if (nbSegments == 0) {
        return ZL_Edge_setDestination(inputs[0], ZL_GRAPH_STORE);
    }

    ZL_TRY_LET(
            ZL_EdgeList,
            streams,
            ZL_Edge_runSplitNode(inputs[0], sizes, nbSegments));
    ZL_ASSERT_EQ(streams.nbEdges, nbSegments);
    for (size_t i = 0; i < nbSegments; ++i) {
        ZL_ERR_IF_ERR(ZL_Edge_setDestination(
                streams.edges[i], successors.graphids[tags[i]]));
    }
    return ZL_returnSuccess();
}

/* ------------------------------------------------------------------------
 * Parsing: tokens to segments
 * ------------------------------------------------------------------------ */

/// Split granularity: whole rows when they are small enough, else elements.
static size_t ST_splitAlign(const ZL_SafetensorsToken* t, size_t chunkSizeMax)
{
    const size_t eltSize = ZL_Safetensors_dtypeSize(t->dtype);
    if (t->dtype == ZL_SafetensorsDtype_unknown || t->ndims < 2) {
        return eltSize;
    }
    uint64_t rowBytes = eltSize;
    for (uint32_t d = 1; d < t->ndims; ++d) {
        if (t->shape[d] != 0 && rowBytes > chunkSizeMax / 2 / t->shape[d]) {
            return eltSize;
        }
        rowBytes *= t->shape[d];
    }
    return rowBytes ? (size_t)rowBytes : eltSize;
}

static int ST_compareU32(const void* a, const void* b)
{
    const uint32_t x = *(const uint32_t*)a;
    const uint32_t y = *(const uint32_t*)b;
    return (x > y) - (x < y);
}

static bool ST_isAtypical(
        const uint8_t* data,
        size_t nbElts,
        size_t eltSize,
        uint32_t* sample)
{
    if (nbElts < ST_MIN_ELTS_FOR_SAMPLING) {
        return false;
    }
    const size_t nbSamples = ZL_MIN(nbElts, (size_t)ST_SAMPLE_SIZE);
    const size_t stride    = nbElts / nbSamples;
    for (size_t i = 0; i < nbSamples; ++i) {
        const uint8_t* const elt = data + i * stride * eltSize;
        sample[i] = eltSize == 2 ? ZL_readLE16(elt) : ZL_readLE32(elt);
    }
    qsort(sample, nbSamples, sizeof(sample[0]), ST_compareU32);
    size_t nbDistinct = 1;
    for (size_t i = 1; i < nbSamples; ++i) {
        nbDistinct += sample[i] != sample[i - 1];
    }
    return nbDistinct * ST_DISTINCT_RATIO < nbSamples;
}

/// Float tensors that are not floating-point noise go to the Transformer.
static uint32_t ST_tagOf(const ZL_SafetensorsToken* t, uint32_t* sample)
{
    const ST_Successor successor = ST_successorOf(t->dtype);
    if (successor != ST_Successor_bf16 && successor != ST_Successor_f16
        && successor != ST_Successor_f32) {
        return successor;
    }
    const size_t eltSize = ZL_Safetensors_dtypeSize(t->dtype);
    const size_t nbElts  = t->size / eltSize;
    if (!ST_isAtypical((const uint8_t*)t->ptr, nbElts, eltSize, sample)) {
        return successor;
    }
    return eltSize == 2 ? ST_Successor_atypical16 : ST_Successor_atypical32;
}

/// Merges consecutive small segments after the header, so that small segments
/// are never adjacent. A merged run keeps its tag when it is shared, and is
/// never split.
static size_t ST_mergeSmallSegments(ST_Segment* segments, size_t nbSegments)
{
    size_t n = 1;
    for (size_t i = 1; i < nbSegments; ++i) {
        ST_Segment* const run = &segments[n - 1];
        if (n > 1 && run->size < ST_SMALL_SEGMENT_SIZE
            && segments[i].size < ST_SMALL_SEGMENT_SIZE) {
            if (run->tag != segments[i].tag) {
                run->tag = ST_Successor_raw;
            }
            run->size += segments[i].size;
            run->splitAlign = run->size;
            continue;
        }
        segments[n++] = segments[i];
    }
    return n;
}

/// Converts the tokens of the file into segments.
/// @returns the number of segments written to @p segments.
static size_t ST_buildSegments(
        const ZL_SafetensorsToken* tokens,
        size_t nbTokens,
        size_t chunkSizeMax,
        uint32_t* sample,
        ST_Segment* segments)
{
    for (size_t i = 0; i < nbTokens; ++i) {
        const ZL_SafetensorsToken* const t = &tokens[i];
        switch (t->type) {
            case ZL_SafetensorsTokenType_header:
                segments[i] = (ST_Segment){ t->size, ST_Successor_header, 1 };
                break;
            case ZL_SafetensorsTokenType_gap:
                segments[i] = (ST_Segment){ t->size, ST_Successor_raw, 1 };
                break;
            case ZL_SafetensorsTokenType_tensor:
            default:
                segments[i] = (ST_Segment){ t->size,
                                            ST_tagOf(t, sample),
                                            ST_splitAlign(t, chunkSizeMax) };
                break;
        }
    }
    return ST_mergeSmallSegments(segments, nbTokens);
}

/* ------------------------------------------------------------------------
 * Chunking
 * ------------------------------------------------------------------------ */

static ZL_Report ST_flush(ST_Chunker* ch)
{
    ZL_RESULT_DECLARE_SCOPE_REPORT(ch->sctx);
    if (ch->nbBytes == 0) {
        return ZL_returnSuccess();
    }
    const ZL_RefParam refParams[2] = {
        { ST_SEGMENT_SIZES_PID, ch->sizes, ch->nbSegments * sizeof(size_t) },
        { ST_SEGMENT_TAGS_PID, ch->tags, ch->nbSegments * sizeof(uint32_t) },
    };
    const ZL_LocalParams localParams        = { .refParams = { refParams, 2 } };
    const ZL_RuntimeGraphParameters gparams = { .localParams = &localParams };
    ZL_ERR_IF_ERR(ZL_Segmenter_processChunk(
            ch->sctx, &ch->nbBytes, 1, ch->innerGraph, &gparams));
    ch->nbSegments = 0;
    ch->nbBytes    = 0;
    return ZL_returnSuccess();
}

/// A chunk holds at most one piece of each segment: pieces of a split segment
/// are flushed one by one, except the last.
static void ST_append(ST_Chunker* ch, size_t size, uint32_t tag)
{
    ZL_ASSERT_LT(ch->nbSegments, ch->capacity);
    ch->sizes[ch->nbSegments] = size;
    ch->tags[ch->nbSegments]  = tag;
    ++ch->nbSegments;
    ch->nbBytes += size;
}

/// Only the last chunk may be smaller than ZL_MIN_CHUNK_SIZE, which
/// ZL_compressBound() relies on.
static ZL_Report ST_flushIfLargeEnough(ST_Chunker* ch)
{
    if (ch->nbBytes < ZL_MIN_CHUNK_SIZE) {
        return ZL_returnSuccess();
    }
    return ST_flush(ch);
}

/// Adds one segment; when it overflows the chunk, it is split on aligned
/// boundaries, its first piece filling the room left in the current chunk.
static ZL_Report ST_addSegment(ST_Chunker* ch, const ST_Segment* seg)
{
    ZL_RESULT_DECLARE_SCOPE_REPORT(ch->sctx);
    if (ch->nbSegments >= ST_MAX_SEGMENTS_PER_CHUNK) {
        ZL_ERR_IF_ERR(ST_flush(ch));
    }
    if (ch->nbBytes + seg->size > ch->chunkSizeMax) {
        ZL_ERR_IF_ERR(ST_flushIfLargeEnough(ch));
    }
    // Rows are at most half a chunk (ST_splitAlign), and a chunk left open by
    // ST_flushIfLargeEnough() holds less than ZL_MIN_CHUNK_SIZE, at most half a
    // chunk too: a row always fits in the room left, so a chunk never exceeds
    // chunkSizeMax.
    size_t remaining = seg->size;
    while (ch->nbBytes + remaining > ch->chunkSizeMax) {
        const size_t room =
                ch->chunkSizeMax - ZL_MIN(ch->nbBytes, ch->chunkSizeMax);
        const size_t align = seg->splitAlign;
        const size_t piece = ZL_MAX(align, room / align * align);
        if (piece >= remaining) {
            break;
        }
        ST_append(ch, piece, seg->tag);
        ZL_ERR_IF_ERR(ST_flush(ch));
        remaining -= piece;
    }
    ST_append(ch, remaining, seg->tag);
    return ZL_returnSuccess();
}

static ZL_Report
ST_chunkSegments(ST_Chunker* ch, const ST_Segment* segments, size_t nbSegments)
{
    ZL_RESULT_DECLARE_SCOPE_REPORT(ch->sctx);
    for (size_t i = 0; i < nbSegments; ++i) {
        ZL_ERR_IF_ERR(ST_addSegment(ch, &segments[i]));
    }
    return ST_flush(ch);
}

/// Adds to @p why that the input is compressed as generic data instead, to be
/// recorded as a warning, retrievable via ZL_CCtx_getWarnings().
static ZL_Report ST_warnFallback(ZL_Segmenter* sctx, ZL_Report why)
{
    ZL_RESULT_DECLARE_SCOPE_REPORT(sctx);
    ZL_ERR_IF_ERR(
            why, "Input not parsed as safetensors: compressed as generic data");
    return why;
}

/// Sized by the number of tokens: each token becomes one segment, and a chunk
/// holds at most one piece of each segment.
typedef struct {
    ZL_SafetensorsToken* tokens;
    ST_Segment* segments;
    uint32_t* sample;
    size_t* sizes;
    uint32_t* tags;
} ST_Buffers;

/// @returns false if an allocation failed.
static bool
ST_allocBuffers(ZL_Segmenter* sctx, size_t maxTokens, ST_Buffers* buffers)
{
    buffers->tokens = ZL_Segmenter_getScratchSpace(
            sctx, maxTokens * sizeof(*buffers->tokens));
    buffers->segments = ZL_Segmenter_getScratchSpace(
            sctx, maxTokens * sizeof(*buffers->segments));
    buffers->sample = ZL_Segmenter_getScratchSpace(
            sctx, ST_SAMPLE_SIZE * sizeof(*buffers->sample));
    buffers->sizes = ZL_Segmenter_getScratchSpace(
            sctx, maxTokens * sizeof(*buffers->sizes));
    buffers->tags = ZL_Segmenter_getScratchSpace(
            sctx, maxTokens * sizeof(*buffers->tags));
    return buffers->tokens && buffers->segments && buffers->sample
            && buffers->sizes && buffers->tags;
}

/// Lexes the input into @p buffers, allocated for the @p maxTokens tokens it
/// can produce at most.
/// @returns the number of tokens, or why the input cannot be parsed.
static ZL_Report ST_lexInput(
        ZL_Segmenter* sctx,
        const void* src,
        size_t srcSize,
        ST_Buffers* buffers,
        size_t* maxTokens)
{
    ZL_RESULT_DECLARE_SCOPE_REPORT(sctx);
    ZL_ErrorContext* const errCtx = ZL_GET_DEFAULT_ERROR_CONTEXT(sctx);
    // Counting first bounds the allocations by the number of tensors, and lets
    // an invalid header fall back before anything is allocated.
    ZL_TRY_SET(
            size_t,
            *maxTokens,
            ZL_Safetensors_lex(src, srcSize, NULL, 0, errCtx));
    ZL_ERR_IF_NOT(ST_allocBuffers(sctx, *maxTokens, buffers), allocation);
    return ZL_Safetensors_lex(
            src, srcSize, buffers->tokens, *maxTokens, errCtx);
}

/// Compresses the input in plain chunks with the generic graph.
static ZL_Report ST_fallback(ZL_Segmenter* sctx, size_t inputSize, size_t chunk)
{
    ZL_RESULT_DECLARE_SCOPE_REPORT(sctx);
    size_t pos = 0;
    while (pos < inputSize) {
        size_t size = ZL_MIN(chunk, inputSize - pos);
        ZL_ERR_IF_ERR(ZL_Segmenter_processChunk(
                sctx, &size, 1, ZL_GRAPH_COMPRESS_GENERIC, NULL));
        pos += size;
    }
    return ZL_returnSuccess();
}

static size_t ST_chunkSizeMax(ZL_Segmenter* sctx)
{
    const ZL_IntParam param =
            ZL_Segmenter_getLocalIntParam(sctx, ST_CHUNK_SIZE_MAX_PID);
    if (param.paramId != ST_CHUNK_SIZE_MAX_PID || param.paramValue <= 0) {
        return ZL_SAFETENSORS_DEFAULT_CHUNK_SIZE;
    }
    // Room for a chunk to be filled to ZL_MIN_CHUNK_SIZE before row-aligned
    // splitting closes it.
    return ZL_MIN(
            ZL_MAX((size_t)param.paramValue, 2 * (size_t)ZL_MIN_CHUNK_SIZE),
            ST_CHUNK_SIZE_LIMIT);
}

static ZL_Report ST_segmenterFn(ZL_Segmenter* sctx)
{
    ZL_RESULT_DECLARE_SCOPE_REPORT(sctx);
    ZL_ERR_IF_NE(ZL_Segmenter_numInputs(sctx), 1, node_invalid_input);
    const ZL_Input* const input = ZL_Segmenter_getInput(sctx, 0);
    const size_t inputSize      = ZL_Input_contentSize(input);
    const size_t chunkSizeMax   = ST_chunkSizeMax(sctx);
    if (inputSize == 0) {
        size_t size = 0;
        return ZL_Segmenter_processChunk(sctx, &size, 1, ZL_GRAPH_STORE, NULL);
    }
    const unsigned formatVersion =
            (unsigned)ZL_Segmenter_getCParam(sctx, ZL_CParam_formatVersion);
    if (formatVersion < ZL_CHUNK_VERSION_MIN) {
        // A single chunk cannot hold one stream per tensor of a large model.
        ZL_RES_convertToWarning(
                sctx,
                ST_warnFallback(
                        sctx,
                        ZL_REPORT_ERROR(
                                graph_parser_unhandledInput,
                                "format version without chunks")));
        return ST_fallback(sctx, inputSize, SIZE_MAX);
    }

    ST_Buffers buffers    = { 0 };
    size_t maxTokens      = 0;
    const ZL_Report lexed = ST_lexInput(
            sctx, ZL_Input_ptr(input), inputSize, &buffers, &maxTokens);
    if (ZL_isError(lexed)) {
        ZL_RES_convertToWarning(sctx, ST_warnFallback(sctx, lexed));
        return ST_fallback(sctx, inputSize, chunkSizeMax);
    }
    const size_t nbTokens = ZL_validResult(lexed);

    const size_t nbSegments = ST_buildSegments(
            buffers.tokens,
            nbTokens,
            chunkSizeMax,
            buffers.sample,
            buffers.segments);

    const ZL_GraphIDList customGraphs = ZL_Segmenter_getCustomGraphs(sctx);
    ZL_ERR_IF_NE(customGraphs.nbGraphIDs, 1, graph_invalid);
    ST_Chunker chunker = {
        .sctx         = sctx,
        .innerGraph   = customGraphs.graphids[0],
        .chunkSizeMax = chunkSizeMax,
        .sizes        = buffers.sizes,
        .tags         = buffers.tags,
        .capacity     = maxTokens,
    };
    return ST_chunkSegments(&chunker, buffers.segments, nbSegments);
}

/* ------------------------------------------------------------------------
 * Registration
 * ------------------------------------------------------------------------ */

/// Trainable successor for numeric data of width @p interpretNode.
static ZL_GraphID ST_numericSuccessor(
        ZL_Compressor* compressor,
        ZL_NodeID interpretNode,
        ZL_GraphID defaultGraph)
{
    return ZL_Compressor_registerStaticGraph_fromNode1o(
            compressor,
            interpretNode,
            ZL_Compressor_buildACEGraphWithDefault(compressor, defaultGraph));
}

static ZL_GraphID ST_floatSuccessor(
        ZL_Compressor* compressor,
        ZL_NodeID interpretNode,
        ZL_NodeID deconstructNode)
{
    const ZL_GraphID deconstruct = ZL_Compressor_registerStaticGraph_fromNode(
            compressor,
            deconstructNode,
            ZL_GRAPHLIST(ZL_GRAPH_STORE, ZL_GRAPH_FSE));
    return ST_numericSuccessor(compressor, interpretNode, deconstruct);
}

/// float16 keeps 2 mantissa bits next to sign and exponent in its high byte,
/// so it is split by byte. The low byte is noise in native float16 weights,
/// where FSE falls back to raw storage at no cost, but weights converted from
/// bfloat16 leave its 3 lowest bits at zero.
static ZL_GraphID ST_f16Successor(ZL_Compressor* compressor)
{
    const size_t fieldSizes[2]     = { 1, 1 };
    const ZL_GraphID byteGraphs[2] = { ZL_GRAPH_FSE, ZL_GRAPH_FSE };
    return ZL_Compressor_buildACEGraphWithDefault(
            compressor,
            ZL_Compressor_registerSplitByStructGraph(
                    compressor, fieldSizes, byteGraphs, 2));
}

static void ST_buildSuccessors(
        ZL_Compressor* compressor,
        ZL_GraphID* successors)
{
    successors[ST_Successor_header] =
            ZL_Compressor_buildACEGraphWithDefault(compressor, ZL_GRAPH_ZSTD);
    successors[ST_Successor_raw] = ZL_Compressor_buildACEGraphWithDefault(
            compressor, ZL_GRAPH_COMPRESS_GENERIC);
    // Same default as raw, but kept apart so that training specializes typed
    // byte tensors (masks, quantized weights) separately from unknown bytes.
    successors[ST_Successor_int8] = ZL_Compressor_buildACEGraphWithDefault(
            compressor, ZL_GRAPH_COMPRESS_GENERIC);
    successors[ST_Successor_int16] = ST_numericSuccessor(
            compressor, ZL_NODE_INTERPRET_AS_LE16, ZL_GRAPH_COMPRESS_GENERIC);
    successors[ST_Successor_int32] = ST_numericSuccessor(
            compressor, ZL_NODE_INTERPRET_AS_LE32, ZL_GRAPH_COMPRESS_GENERIC);
    successors[ST_Successor_int64] = ST_numericSuccessor(
            compressor, ZL_NODE_INTERPRET_AS_LE64, ZL_GRAPH_COMPRESS_GENERIC);
    successors[ST_Successor_f16]  = ST_f16Successor(compressor);
    successors[ST_Successor_bf16] = ST_floatSuccessor(
            compressor,
            ZL_NODE_INTERPRET_AS_LE16,
            ZL_NODE_BFLOAT16_DECONSTRUCT);
    successors[ST_Successor_f32] = ST_floatSuccessor(
            compressor, ZL_NODE_INTERPRET_AS_LE32, ZL_NODE_FLOAT32_DECONSTRUCT);
    successors[ST_Successor_f64] = ST_numericSuccessor(
            compressor, ZL_NODE_INTERPRET_AS_LE64, ZL_GRAPH_COMPRESS_GENERIC);
    successors[ST_Successor_atypical16] = ST_numericSuccessor(
            compressor,
            ZL_NODE_INTERPRET_AS_LE16,
            ZL_GRAPH_TRANSFORMER_NUMERIC);
    successors[ST_Successor_atypical32] = ST_numericSuccessor(
            compressor,
            ZL_NODE_INTERPRET_AS_LE32,
            ZL_GRAPH_TRANSFORMER_NUMERIC);
}

static ZL_GraphID ST_innerGraph(ZL_Compressor* compressor)
{
    ZL_GraphID base = ZL_Compressor_getGraph(compressor, "Safetensors Parser");
    if (base.gid == ZL_GRAPH_ILLEGAL.gid) {
        const ZL_Type inputType         = ZL_Type_serial;
        const ZL_FunctionGraphDesc desc = {
            .name           = "!Safetensors Parser",
            .graph_f        = ST_innerGraphFn,
            .inputTypeMasks = &inputType,
            .nbInputs       = 1,
        };
        base = ZL_Compressor_registerFunctionGraph(compressor, &desc);
    }
    ZL_GraphID successors[ST_Successor_count];
    ST_buildSuccessors(compressor, successors);
    const ZL_ParameterizedGraphDesc desc = {
        .graph          = base,
        .customGraphs   = successors,
        .nbCustomGraphs = ST_Successor_count,
    };
    return ZL_Compressor_registerParameterizedGraph(compressor, &desc);
}

ZL_RESULT_OF(ZL_GraphID)
ZL_Safetensors_registerGraph(ZL_Compressor* compressor, size_t chunkSizeMax)
{
    ZL_RESULT_DECLARE_SCOPE(ZL_GraphID, compressor);
    ZL_ERR_IF_EQ(chunkSizeMax, 0, parameter_invalid);
    const ZL_GraphID inner = ST_innerGraph(compressor);
    ZL_ERR_IF_EQ(inner.gid, ZL_GRAPH_ILLEGAL.gid, graph_invalid);

    ZL_GraphID base =
            ZL_Compressor_getGraph(compressor, "Safetensors Segmenter");
    if (base.gid == ZL_GRAPH_ILLEGAL.gid) {
        const ZL_Type inputType     = ZL_Type_serial;
        const ZL_SegmenterDesc desc = {
            .name           = "!Safetensors Segmenter",
            .segmenterFn    = ST_segmenterFn,
            .inputTypeMasks = &inputType,
            .numInputs      = 1,
        };
        base = ZL_Compressor_registerSegmenter(compressor, &desc);
    }
    const ZL_IntParam intParam = {
        .paramId    = ST_CHUNK_SIZE_MAX_PID,
        .paramValue = (int)ZL_MIN(chunkSizeMax, (size_t)INT32_MAX),
    };
    const ZL_LocalParams localParams     = { .intParams = { &intParam, 1 } };
    const ZL_ParameterizedGraphDesc desc = {
        .graph          = base,
        .localParams    = &localParams,
        .customGraphs   = &inner,
        .nbCustomGraphs = 1,
    };
    const ZL_GraphID graph =
            ZL_Compressor_registerParameterizedGraph(compressor, &desc);
    ZL_ERR_IF_EQ(graph.gid, ZL_GRAPH_ILLEGAL.gid, graph_invalid);
    return ZL_RESULT_WRAP_VALUE(ZL_GraphID, graph);
}
