// Copyright (c) Meta Platforms, Inc. and affiliates.

#include "custom_parsers/gguf/gguf_parser.h"

#include <stdbool.h>
#include <stdlib.h>
#include <string.h>

#include "custom_parsers/gguf/gguf_lexer.h"
#include "custom_parsers/shared_components/tensor_chunker.h"
#include "custom_parsers/shared_components/tensor_graphs.h"
#include "openzl/codecs/zl_ace.h"
#include "openzl/codecs/zl_entropy.h"
#include "openzl/codecs/zl_split_by_struct.h"
#include "openzl/codecs/zl_store.h"
#include "openzl/codecs/zl_zstd.h"
#include "openzl/common/assertion.h"
#include "openzl/shared/bits.h"
#include "openzl/shared/data_stats.h"
#include "openzl/shared/mem.h"
#include "openzl/shared/utils.h"
#include "openzl/zl_compress.h"
#include "openzl/zl_errors.h"
#include "openzl/zl_selector.h"

/// A quantized type split into fields goes to successor
/// GG_FIRST_BLOCK_SUCCESSOR + its ggml type. ggml never reuses a type value, so
/// these indices keep their meaning in serialized compressors. They start at a
/// fixed index, so that tensor classes can grow too; unused indices go to the
/// raw successor.
#define GG_FIRST_BLOCK_SUCCESSOR 32
#define GG_NB_SUCCESSORS (GG_FIRST_BLOCK_SUCCESSOR + ZL_GGUFType_limit)
ZL_STATIC_ASSERT(
        ZL_TensorClass_count <= GG_FIRST_BLOCK_SUCCESSOR,
        "tensor classes overlap the successors of quantized types");

#define GG_MAX_FIELDS 5

typedef enum {
    GG_q,   ///< packed quantized values or sub-block scales
    GG_f16, ///< a float16 scale
    GG_f8,  ///< 8-bit float scales (E8M0, E4M3)
} GG_FieldKind;

/// Fields of a quantized block in storage order.
typedef struct {
    uint32_t type;
    size_t nbFields;
    size_t sizes[GG_MAX_FIELDS];
    GG_FieldKind kinds[GG_MAX_FIELDS];
} GG_BlockLayout;

// Fields in the order of the block structs of ggml/src/ggml-common.h, e.g.
// block_q2_K = { scales, qs, d, dmin } and block_q6_K = { ql, qh, scales, d }.
// block_iq2_s.qs holds grid indices then signs, and block_iq3_xxs.qs grid
// indices then scales and signs: both are split here.
// block_iq1_m has no float16 field: its scale is spread over its sub-block
// scales.
static const GG_BlockLayout kBlockLayouts[] = {
    { ZL_GGUFType_q4_0, 2, { 2, 16 }, { GG_f16, GG_q } },
    { ZL_GGUFType_q4_1, 3, { 2, 2, 16 }, { GG_f16, GG_f16, GG_q } },
    { ZL_GGUFType_q5_0, 3, { 2, 4, 16 }, { GG_f16, GG_q, GG_q } },
    { ZL_GGUFType_q5_1, 4, { 2, 2, 4, 16 }, { GG_f16, GG_f16, GG_q, GG_q } },
    { ZL_GGUFType_q8_0, 2, { 2, 32 }, { GG_f16, GG_q } },
    { ZL_GGUFType_q2_k, 4, { 16, 64, 2, 2 }, { GG_q, GG_q, GG_f16, GG_f16 } },
    { ZL_GGUFType_q3_k, 4, { 32, 64, 12, 2 }, { GG_q, GG_q, GG_q, GG_f16 } },
    { ZL_GGUFType_q4_k, 4, { 2, 2, 12, 128 }, { GG_f16, GG_f16, GG_q, GG_q } },
    { ZL_GGUFType_q5_k,
      5,
      { 2, 2, 12, 32, 128 },
      { GG_f16, GG_f16, GG_q, GG_q, GG_q } },
    { ZL_GGUFType_q6_k, 4, { 128, 64, 16, 2 }, { GG_q, GG_q, GG_q, GG_f16 } },
    { ZL_GGUFType_iq2_xxs, 2, { 2, 64 }, { GG_f16, GG_q } },
    { ZL_GGUFType_iq2_xs, 3, { 2, 64, 8 }, { GG_f16, GG_q, GG_q } },
    { ZL_GGUFType_iq3_xxs, 3, { 2, 64, 32 }, { GG_f16, GG_q, GG_q } },
    { ZL_GGUFType_iq1_s, 3, { 2, 32, 16 }, { GG_f16, GG_q, GG_q } },
    { ZL_GGUFType_iq4_nl, 2, { 2, 16 }, { GG_f16, GG_q } },
    { ZL_GGUFType_iq3_s,
      5,
      { 2, 64, 8, 32, 4 },
      { GG_f16, GG_q, GG_q, GG_q, GG_q } },
    { ZL_GGUFType_iq2_s,
      5,
      { 2, 32, 32, 8, 8 },
      { GG_f16, GG_q, GG_q, GG_q, GG_q } },
    { ZL_GGUFType_iq4_xs, 4, { 2, 2, 4, 128 }, { GG_f16, GG_q, GG_q, GG_q } },
    { ZL_GGUFType_iq1_m, 3, { 32, 16, 8 }, { GG_q, GG_q, GG_q } },
    { ZL_GGUFType_tq1_0, 3, { 48, 4, 2 }, { GG_q, GG_q, GG_f16 } },
    { ZL_GGUFType_tq2_0, 2, { 64, 2 }, { GG_q, GG_f16 } },
    { ZL_GGUFType_mxfp4, 2, { 1, 16 }, { GG_f8, GG_q } },
    { ZL_GGUFType_nvfp4, 2, { 4, 32 }, { GG_f8, GG_q } },
    { ZL_GGUFType_q1_0, 2, { 2, 16 }, { GG_f16, GG_q } },
    { ZL_GGUFType_q2_0, 2, { 2, 16 }, { GG_f16, GG_q } },
};
#define GG_NB_BLOCK_LAYOUTS (sizeof(kBlockLayouts) / sizeof(kBlockLayouts[0]))

static uint32_t GG_tagOf(uint32_t type)
{
    switch (type) {
        case ZL_GGUFType_f32:
            return ZL_TensorClass_f32;
        case ZL_GGUFType_f16:
            return ZL_TensorClass_f16;
        case ZL_GGUFType_bf16:
            return ZL_TensorClass_bf16;
        case ZL_GGUFType_f64:
            return ZL_TensorClass_f64;
        case ZL_GGUFType_i8:
            return ZL_TensorClass_int8;
        case ZL_GGUFType_i16:
            return ZL_TensorClass_int16;
        case ZL_GGUFType_i32:
            return ZL_TensorClass_int32;
        case ZL_GGUFType_i64:
            return ZL_TensorClass_int64;
        default:
            break;
    }
    for (size_t i = 0; i < GG_NB_BLOCK_LAYOUTS; ++i) {
        if (kBlockLayouts[i].type == type) {
            return GG_FIRST_BLOCK_SUCCESSOR + type;
        }
    }
    size_t blockElts, blockBytes;
    if (!ZL_GGUF_typeLayout(type, &blockElts, &blockBytes)) {
        return ZL_TensorClass_raw;
    }
    // Other known types are typed bytes, kept apart from raw data so that
    // training specializes them. Q8_1 and Q8_K are among them: llama.cpp only
    // uses them for activations, not stored weights.
    return ZL_TensorClass_int8;
}

/* ------------------------------------------------------------------------
 * Description of the tokens
 * ------------------------------------------------------------------------ */

/// Split granularity: whole rows when they are small enough, else blocks.
static size_t GG_splitAlign(const ZL_GGUFToken* t, size_t chunkSizeMax)
{
    size_t blockElts = 1, blockBytes = 1;
    ZL_GGUF_typeLayout(t->ggmlType, &blockElts, &blockBytes);
    if (t->ndims < 2) {
        return blockBytes;
    }
    const uint64_t rowBlocks = t->ne[0] / blockElts;
    if (rowBlocks == 0 || rowBlocks > chunkSizeMax / 2 / blockBytes) {
        return blockBytes;
    }
    return (size_t)rowBlocks * blockBytes;
}

static ZL_Report GG_lex(
        const void* src,
        size_t srcSize,
        void* tokens,
        size_t tokensCapacity,
        ZL_ErrorContext* errCtx)
{
    return ZL_GGUF_lex(src, srcSize, tokens, tokensCapacity, errCtx);
}

static ZL_TensorInfo GG_describe(const void* token, size_t chunkSizeMax)
{
    const ZL_GGUFToken* const t = (const ZL_GGUFToken*)token;
    switch (t->type) {
        case ZL_GGUFTokenType_header:
            return (ZL_TensorInfo){ t->ptr, t->size, ZL_TensorClass_header, 1 };
        case ZL_GGUFTokenType_tensor:
            return (ZL_TensorInfo){ t->ptr,
                                    t->size,
                                    GG_tagOf(t->ggmlType),
                                    GG_splitAlign(t, chunkSizeMax) };
        case ZL_GGUFTokenType_gap:
        default:
            return (ZL_TensorInfo){ t->ptr, t->size, ZL_TensorClass_raw, 1 };
    }
}

static const ZL_TensorFormat kGGUFFormat = {
    .name             = "GGUF",
    .defaultChunkSize = ZL_GGUF_DEFAULT_CHUNK_SIZE,
    .tokenSize        = sizeof(ZL_GGUFToken),
    .lex              = GG_lex,
    .describe         = GG_describe,
};

static ZL_Report GG_segmenterFn(ZL_Segmenter* sctx)
{
    return ZL_TensorChunker_segment(sctx, &kGGUFFormat);
}

/* ------------------------------------------------------------------------
 * Registration
 * ------------------------------------------------------------------------ */

enum { GG_scale16_huffman, GG_scale16_bytes, GG_scale16_zstd, GG_SCALE16_NB };

/// The entropy graphs code large inputs in independent chunks, each with its
/// own header (encode_entropy_binding.c): the estimates follow the same chunks.
#define GG_FSE_CHUNK_SIZE ((size_t)1 << 15)
#define GG_FSE_CHUNKED_SIZE 100000
#define GG_HUFFMAN_CHUNK_ELTS ((size_t)1 << 18)
#define GG_HUFFMAN_CHUNKED_ELTS 400000
/// Up to this number of symbols, Huffman code lengths are computed rather than
/// taken from the entropy, which they exceed the most on few symbols.
#define GG_EXACT_HUFFMAN_SYMBOLS 256

/// Scratch space of the estimates. GG_scale16Stats() spends on it a time
/// following the number of distinct scales rather than the 65536 possible ones.
typedef struct {
    unsigned count[65536];
    unsigned counts[65536]; // count of each distinct scale
    uint16_t scales[65536]; // distinct scales
    // internal nodes of a Huffman tree
    uint64_t nodes[GG_EXACT_HUFFMAN_SYMBOLS];
    unsigned byteCounts[2][256]; // count of each value of each byte
} GG_Scale16Workspace;

/// FSE codes each byte of the scales with a header of about 10 bits per
/// symbol.
static double GG_bytesSizeEstimate(
        const uint8_t* src,
        size_t nbElts,
        GG_Scale16Workspace* wksp)
{
    const size_t chunkSize =
            nbElts >= GG_FSE_CHUNKED_SIZE ? GG_FSE_CHUNK_SIZE : nbElts;
    double size = 0;
    for (size_t start = 0; start < nbElts; start += chunkSize) {
        const size_t n               = ZL_MIN(chunkSize, nbElts - start);
        unsigned (*const count)[256] = wksp->byteCounts;
        memset(wksp->byteCounts, 0, sizeof(wksp->byteCounts));
        for (size_t i = start; i < start + n; ++i) {
            count[0][src[2 * i]]++;
            count[1][src[2 * i + 1]]++;
        }
        for (size_t b = 0; b < 2; ++b) {
            size_t cardinality = 0;
            for (size_t v = 0; v < 256; ++v) {
                cardinality += count[b][v] != 0;
            }
            const double laneSize =
                    ZL_calculateEntropy(count[b], 255, n) * (double)n / 8
                    + 1.25 * (double)cardinality;
            size += ZL_MIN(laneSize, (double)n);
        }
    }
    return size;
}

typedef struct {
    double huffmanSize;
    size_t repeats;       // scales equal to the previous one
    double chanceRepeats; // repeats expected from the histogram alone
} GG_Scale16Stats;

static int GG_compareCounts(const void* a, const void* b)
{
    const unsigned x = *(const unsigned*)a;
    const unsigned y = *(const unsigned*)b;
    return (x > y) - (x < y);
}

/// @returns the size in bits of the Huffman code of the @p nbSymbols counts,
/// which it sorts: the sum of the weights of the internal nodes of the tree,
/// built in @p nodes in increasing weight order.
static double
GG_huffmanBits(unsigned* counts, size_t nbSymbols, uint64_t* nodes)
{
    qsort(counts, nbSymbols, sizeof(*counts), GG_compareCounts);
    size_t leaf = 0, node = 0;
    uint64_t bits = 0;
    for (size_t n = 0; n + 1 < nbSymbols; ++n) {
        uint64_t weight = 0;
        for (int child = 0; child < 2; ++child) {
            if (node < n && (leaf == nbSymbols || nodes[node] < counts[leaf])) {
                weight += nodes[node++];
            } else {
                weight += counts[leaf++];
            }
        }
        nodes[n] = weight;
        bits += weight;
    }
    return (double)bits;
}

/// The 16-bit Huffman graph bitpacks or stores a chunk whose Huffman code it
/// expects to gain too little, with a header estimate of max(100, maxScale / 4)
/// bytes, over 8 KiB on signed scales (encode_entropy_binding.c).
/// @returns the size of a chunk of @p n scales, whose Huffman code would take
/// @p huffmanSize bytes.
static double GG_huffmanChunkSize(
        size_t n,
        double entropy,
        unsigned maxScale,
        size_t minGain,
        double huffmanSize)
{
    const size_t storeSize = 2 * n;
    const size_t estimate  = (size_t)(ZL_MAX(1, entropy) * (double)n + 7) / 8
            + ZL_MAX(100, maxScale / 4);
    const size_t bitpackSize =
            (n * (size_t)ZL_nextPow2(maxScale + 1) + 7) / 8 + 2;
    if (bitpackSize <= estimate && bitpackSize + minGain <= storeSize) {
        return (double)bitpackSize;
    }
    if (entropy > 15 || estimate + minGain > storeSize) {
        return (double)storeSize;
    }
    return ZL_MIN(huffmanSize, (double)storeSize);
}

static GG_Scale16Stats
GG_scale16Stats(const uint8_t* src, size_t nbElts, GG_Scale16Workspace* wksp)
{
    GG_Scale16Stats stats = { 0 };
    const size_t chunkElts =
            nbElts >= GG_HUFFMAN_CHUNKED_ELTS ? GG_HUFFMAN_CHUNK_ELTS : nbElts;
    // The minimum gain of the entropy graphs, shared among the chunks
    const size_t minGain = ZL_MAX(32, 2 * nbElts / 100);
    memset(wksp->count, 0, sizeof(wksp->count));
    uint16_t prev = (uint16_t)~ZL_readLE16(src);
    for (size_t start = 0; start < nbElts; start += chunkElts) {
        const size_t n     = ZL_MIN(chunkElts, nbElts - start);
        size_t cardinality = 0;
        unsigned maxScale  = 0;
        for (size_t i = start; i < start + n; ++i) {
            const uint16_t v = ZL_readLE16(src + 2 * i);
            if (wksp->count[v]++ == 0) {
                wksp->scales[cardinality++] = v;
            }
            maxScale = ZL_MAX(maxScale, v);
            stats.repeats += v == prev;
            prev = v;
        }
        double sumSquares = 0;
        for (size_t k = 0; k < cardinality; ++k) {
            const unsigned c             = wksp->count[wksp->scales[k]];
            wksp->counts[k]              = c;
            wksp->count[wksp->scales[k]] = 0;
            sumSquares += (double)c * (double)c;
        }
        // 16-bit Huffman spends at least a bit per symbol, and about 1.5 byte
        // of header per symbol. Its whole code lengths cost more than the
        // entropy on few symbols, e.g. 1.67 bits instead of 1.58 on 3 equally
        // frequent ones.
        const double entropy =
                ZL_calculateEntropy(wksp->counts, cardinality - 1, n);
        const double bits = cardinality <= GG_EXACT_HUFFMAN_SYMBOLS
                ? GG_huffmanBits(wksp->counts, cardinality, wksp->nodes)
                : entropy * (double)n;
        const double huffmanSize =
                ZL_MAX(bits, (double)n) / 8 + 1.5 * (double)cardinality;
        stats.huffmanSize += GG_huffmanChunkSize(
                n,
                entropy,
                maxScale,
                (size_t)((uint64_t)minGain * n / nbElts),
                huffmanSize);
        stats.chanceRepeats += sumSquares / (double)n;
    }
    return stats;
}

static double
GG_trySize(const ZL_Selector* selector, const ZL_Input* input, ZL_GraphID graph)
{
    const ZL_GraphReport report = ZL_Selector_tryGraph(selector, input, graph);
    return ZL_isError(report.finalCompressedSize)
            ? (double)ZL_Input_contentSize(input)
            : (double)ZL_validResult(report.finalCompressedSize);
}

/// zstd is first tried on 3 slices of the scales, which show repeats within a
/// row, e.g. scales shared by a row or repeating every row.
#define GG_SAMPLE_SLICE_SIZE ((size_t)16 << 10)

static double GG_zstdSampleSize(
        const ZL_Selector* selector,
        const ZL_Input* input,
        ZL_GraphID zstd)
{
    const size_t size = ZL_Input_contentSize(input);
    if (size <= 3 * GG_SAMPLE_SLICE_SIZE) {
        return GG_trySize(selector, input, zstd);
    }
    const uint8_t* const src = (const uint8_t*)ZL_Input_ptr(input);
    uint8_t* const sample    = (uint8_t*)ZL_Selector_getScratchSpace(
            selector, 3 * GG_SAMPLE_SLICE_SIZE);
    if (sample == NULL) {
        return (double)size;
    }
    const size_t middle = (size / 2 - GG_SAMPLE_SLICE_SIZE / 2) & ~(size_t)1;
    memcpy(sample, src, GG_SAMPLE_SLICE_SIZE);
    memcpy(sample + GG_SAMPLE_SLICE_SIZE, src + middle, GG_SAMPLE_SLICE_SIZE);
    memcpy(sample + 2 * GG_SAMPLE_SLICE_SIZE,
           src + size - GG_SAMPLE_SLICE_SIZE,
           GG_SAMPLE_SLICE_SIZE);
    ZL_TypedRef* const sampleRef =
            ZL_TypedRef_createSerial(sample, 3 * GG_SAMPLE_SLICE_SIZE);
    if (sampleRef == NULL) {
        return (double)size;
    }
    const double sampleSize = GG_trySize(selector, sampleRef, zstd);
    ZL_TypedRef_free(sampleRef);
    return sampleSize * (double)size / (double)(3 * GG_SAMPLE_SLICE_SIZE);
}

/// No codec wins on all float16 scales: 16-bit Huffman suits most tensors,
/// coding their bytes separately suits scales taking many values for their
/// number, and zstd catches repeated scales. The entropy coders are estimated
/// from histograms, which costs a fraction of trying them.
static ZL_GraphID GG_selectScale16(
        const ZL_Selector* selector,
        const ZL_Input* input,
        const ZL_GraphID* candidates,
        size_t nbCandidates)
{
    // A serialized compressor can replace the candidates.
    if (nbCandidates != GG_SCALE16_NB) {
        return ZL_GRAPH_ILLEGAL;
    }
    // Only 16-bit scales are estimated, and zstd takes any other input.
    if (ZL_Input_eltWidth(input) != 2) {
        return candidates[GG_scale16_zstd];
    }
    const uint8_t* const src = (const uint8_t*)ZL_Input_ptr(input);
    const size_t nbElts      = ZL_Input_contentSize(input) / 2;
    GG_Scale16Workspace* const wksp =
            (GG_Scale16Workspace*)ZL_Selector_getScratchSpace(
                    selector, sizeof(GG_Scale16Workspace));
    if (wksp == NULL || nbElts < 2) {
        return candidates[GG_scale16_huffman];
    }
    const GG_Scale16Stats stats = GG_scale16Stats(src, nbElts, wksp);
    // Huffman stores constant scales as a constant.
    if (stats.repeats == nbElts - 1) {
        return candidates[GG_scale16_huffman];
    }
    double sizes[GG_SCALE16_NB];
    sizes[GG_scale16_huffman] = stats.huffmanSize;
    sizes[GG_scale16_bytes]   = GG_bytesSizeEstimate(src, nbElts, wksp);
    // zstd only wins on repeated scales. A sample shows repeats within a row,
    // but can be more repetitive than the whole scales, and misses repeats
    // paying off only over the whole scales, e.g. scales duplicated by a
    // conversion from larger groups, which repeat their predecessor more often
    // than chance: zstd is then tried on the whole scales.
    const ZL_GraphID zstd = candidates[GG_scale16_zstd];
    const bool repeated =
            (double)stats.repeats > stats.chanceRepeats + (double)nbElts / 8;
    sizes[GG_scale16_zstd] = repeated
            ? GG_trySize(selector, input, zstd)
            : GG_zstdSampleSize(selector, input, zstd);
    if (!repeated
        && sizes[GG_scale16_zstd]
                < ZL_MIN(sizes[GG_scale16_huffman], sizes[GG_scale16_bytes])
        && ZL_Input_contentSize(input) > 3 * GG_SAMPLE_SLICE_SIZE) {
        sizes[GG_scale16_zstd] = GG_trySize(selector, input, zstd);
    }
    size_t best = 0;
    for (size_t i = 1; i < GG_SCALE16_NB; ++i) {
        if (sizes[i] < sizes[best]) {
            best = i;
        }
    }
    if (sizes[best] >= 2 * (double)nbElts) {
        return ZL_GRAPH_STORE;
    }
    return candidates[best];
}

static ZL_RESULT_OF(ZL_GraphID)
        GG_registerScale16Graph(ZL_Compressor* compressor)
{
    // Serialized compressors refer to selectors by name, so this one is
    // anchored, and registered once per compressor.
    const ZL_GraphID existing =
            ZL_Compressor_getGraph(compressor, "Tensor Scale16");
    if (existing.gid != ZL_GRAPH_ILLEGAL.gid) {
        return ZL_RESULT_WRAP_VALUE(ZL_GraphID, existing);
    }
    const size_t byteSizes[2]                  = { 1, 1 };
    const ZL_GraphID byteGraphs[2]             = { ZL_GRAPH_FSE, ZL_GRAPH_FSE };
    const ZL_GraphID candidates[GG_SCALE16_NB] = {
        [GG_scale16_huffman] = ZL_GRAPH_HUFFMAN,
        [GG_scale16_bytes]   = ZL_Compressor_registerSplitByStructGraph(
                compressor, byteSizes, byteGraphs, 2),
        // Keeps most of the gain of higher levels on repeated scales, at a
        // fraction of their cost.
        [GG_scale16_zstd] =
                ZL_Compressor_registerZstdGraph_withLevel(compressor, 3),
    };
    const ZL_SelectorDesc desc = {
        .selector_f     = GG_selectScale16,
        .inStreamType   = ZL_Type_serial | ZL_Type_struct,
        .customGraphs   = candidates,
        .nbCustomGraphs = GG_SCALE16_NB,
        .name           = "!Tensor Scale16",
    };
    return ZL_Compressor_registerSelectorGraph2(compressor, &desc);
}

/// Quantized values and sub-block scales are nearly uniform bytes, where the
/// Huffman stage of zstd beats FSE; 8-bit float scales take few values, where
/// FSE beats Huffman.
static ZL_GraphID GG_fieldGraph(GG_FieldKind kind, ZL_GraphID scale16)
{
    switch (kind) {
        case GG_f16:
            return scale16;
        case GG_f8:
            return ZL_GRAPH_FSE;
        case GG_q:
        default:
            return ZL_GRAPH_ZSTD;
    }
}

static ZL_GraphID GG_blockSuccessor(
        ZL_Compressor* compressor,
        const GG_BlockLayout* layout,
        ZL_GraphID scale16)
{
    ZL_GraphID fieldGraphs[GG_MAX_FIELDS];
    for (size_t i = 0; i < layout->nbFields; ++i) {
        fieldGraphs[i] = GG_fieldGraph(layout->kinds[i], scale16);
    }
    return ZL_Compressor_buildACEGraphWithDefault(
            compressor,
            ZL_Compressor_registerSplitByStructGraph(
                    compressor, layout->sizes, fieldGraphs, layout->nbFields));
}

ZL_RESULT_OF(ZL_GraphID)
ZL_GGUF_registerGraph(ZL_Compressor* compressor, size_t chunkSizeMax)
{
    ZL_RESULT_DECLARE_SCOPE(ZL_GraphID, compressor);
    ZL_TRY_LET(ZL_GraphID, scale16, GG_registerScale16Graph(compressor));
    ZL_GraphID successors[GG_NB_SUCCESSORS];
    ZL_TensorGraphs_buildSuccessors(compressor, successors);
    for (size_t i = ZL_TensorClass_count; i < GG_NB_SUCCESSORS; ++i) {
        successors[i] = successors[ZL_TensorClass_raw];
    }
    for (size_t i = 0; i < GG_NB_BLOCK_LAYOUTS; ++i) {
        ZL_ASSERT_LT(kBlockLayouts[i].type, ZL_GGUFType_limit);
        successors[GG_FIRST_BLOCK_SUCCESSOR + kBlockLayouts[i].type] =
                GG_blockSuccessor(compressor, &kBlockLayouts[i], scale16);
    }
    return ZL_TensorChunker_registerSegmenter(
            compressor,
            "!GGUF Segmenter",
            GG_segmenterFn,
            chunkSizeMax,
            "!GGUF Parser",
            successors,
            GG_NB_SUCCESSORS);
}
