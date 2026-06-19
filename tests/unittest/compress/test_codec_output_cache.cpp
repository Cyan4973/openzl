// Copyright (c) Meta Platforms, Inc. and affiliates.

#include <cstring>

#include "openzl/compress/codec_output_cache.h"

#include <gtest/gtest.h>

namespace {

COC_Key mkKey(
        uint64_t hi,
        uint64_t lo,
        uint32_t cid,
        uint32_t fmt,
        int32_t lvl,
        const void* cp,
        uint32_t cpsz)
{
    COC_Key k;
    memset(&k, 0, sizeof(k));
    k.contentHashHigh  = hi;
    k.contentHashLow   = lo;
    k.codecID          = cid;
    k.formatVersion    = fmt;
    k.compressionLevel = lvl;
    k.copyParams       = cp;
    k.copyParamsSize   = cpsz;
    return k;
}

COC_Output mkOut(const void* content, size_t n)
{
    COC_Output o;
    memset(&o, 0, sizeof(o));
    o.type        = ZL_Type_serial;
    o.eltWidth    = 1;
    o.numElts     = n;
    o.contentSize = n;
    o.content     = content;
    return o;
}

COC_Entry mkEntry(const COC_Output* out, const void* hdr, size_t hsz)
{
    COC_Entry e;
    memset(&e, 0, sizeof(e));
    e.nbOutputs  = out ? 1 : 0;
    e.outputs    = out;
    e.header     = hdr;
    e.headerSize = hsz;
    e.hasHeader  = (hdr != nullptr);
    return e;
}

// RAII wrapper so a failed assertion doesn't leak the cache.
struct Cache {
    ZL_CodecOutputCache* c;
    explicit Cache(size_t maxBytes = 0)
            : c(ZL_CodecOutputCache_create(maxBytes))
    {
    }
    ~Cache()
    {
        ZL_CodecOutputCache_free(c);
    }
    operator ZL_CodecOutputCache*() const
    {
        return c;
    }
};

} // namespace

TEST(CodecOutputCacheTest, CreateEmpty)
{
    Cache c;
    ASSERT_NE(c.c, nullptr);
    auto s = ZL_CodecOutputCache_getStats(c);
    EXPECT_EQ(s.hits, 0u);
    EXPECT_EQ(s.misses, 0u);
    EXPECT_EQ(s.bytesStored, 0u);
}

TEST(CodecOutputCacheTest, InsertThenHit)
{
    Cache c;
    COC_Key k = mkKey(1, 2, 10, 16, 3, "PA", 2);
    EXPECT_EQ(COC_find(c, &k), nullptr); // miss first

    const char payload[] = "hello-output";
    const char header[]  = "HDR";
    COC_Output o         = mkOut(payload, sizeof(payload));
    COC_Entry e          = mkEntry(&o, header, sizeof(header));
    ASSERT_FALSE(ZL_isError(COC_insert(c, &k, &e)));

    const COC_Entry* g = COC_find(c, &k);
    ASSERT_NE(g, nullptr);
    ASSERT_EQ(g->nbOutputs, 1u);
    EXPECT_EQ(g->outputs[0].type, ZL_Type_serial);
    EXPECT_EQ(g->outputs[0].contentSize, sizeof(payload));
    EXPECT_EQ(memcmp(g->outputs[0].content, payload, sizeof(payload)), 0);
    EXPECT_TRUE(g->hasHeader);
    EXPECT_EQ(g->headerSize, sizeof(header));
    EXPECT_EQ(memcmp(g->header, header, sizeof(header)), 0);
}

// The cache must own its copies: mutating the source buffers afterwards must
// not affect the stored entry.
TEST(CodecOutputCacheTest, OwnsItsCopies)
{
    Cache c;
    char payload[] = "hello-output";
    char params[]  = "PA";
    COC_Key k      = mkKey(1, 2, 10, 16, 3, params, sizeof(params));
    COC_Output o   = mkOut(payload, sizeof(payload));
    COC_Entry e    = mkEntry(&o, nullptr, 0);
    ASSERT_FALSE(ZL_isError(COC_insert(c, &k, &e)));

    memset(payload, 'X', sizeof(payload));
    memset(params, 'X', sizeof(params));

    // Look up with a fresh key holding the original param bytes.
    const char origParams[] = "PA";
    COC_Key k2         = mkKey(1, 2, 10, 16, 3, origParams, sizeof(origParams));
    const COC_Entry* g = COC_find(c, &k2);
    ASSERT_NE(g, nullptr);
    const char want[] = "hello-output";
    EXPECT_EQ(memcmp(g->outputs[0].content, want, sizeof(want)), 0);
}

// Same content + codec + globals, different copy-params (e.g. tokenize vs
// tokenize_sort) must never collide.
TEST(CodecOutputCacheTest, ParamsDistinguishNodes)
{
    Cache c;
    COC_Key sorted   = mkKey(1, 2, 10, 16, 3, "S1", 2);
    COC_Key unsorted = mkKey(1, 2, 10, 16, 3, "S0", 2);

    EXPECT_FALSE(COC_Map_eq(&sorted, &unsorted));
    EXPECT_NE(COC_Map_hash(&sorted), COC_Map_hash(&unsorted));

    const char a[] = "A";
    const char b[] = "B";
    COC_Output oa  = mkOut(a, sizeof(a));
    COC_Output ob  = mkOut(b, sizeof(b));
    COC_Entry ea   = mkEntry(&oa, nullptr, 0);
    COC_Entry eb   = mkEntry(&ob, nullptr, 0);
    ASSERT_FALSE(ZL_isError(COC_insert(c, &sorted, &ea)));
    ASSERT_FALSE(ZL_isError(COC_insert(c, &unsorted, &eb)));

    const COC_Entry* gs = COC_find(c, &sorted);
    const COC_Entry* gu = COC_find(c, &unsorted);
    ASSERT_NE(gs, nullptr);
    ASSERT_NE(gu, nullptr);
    EXPECT_EQ(memcmp(gs->outputs[0].content, a, sizeof(a)), 0);
    EXPECT_EQ(memcmp(gu->outputs[0].content, b, sizeof(b)), 0);
}

TEST(CodecOutputCacheTest, EveryScalarIngredientIsKeyed)
{
    Cache c;
    COC_Key base = mkKey(1, 2, 10, 16, 3, "PA", 2);
    COC_Output o = mkOut("x", 1);
    COC_Entry e  = mkEntry(&o, nullptr, 0);
    ASSERT_FALSE(ZL_isError(COC_insert(c, &base, &e)));

    COC_Key hiDiff  = mkKey(99, 2, 10, 16, 3, "PA", 2);
    COC_Key loDiff  = mkKey(1, 99, 10, 16, 3, "PA", 2);
    COC_Key cidDiff = mkKey(1, 2, 11, 16, 3, "PA", 2);
    COC_Key fmtDiff = mkKey(1, 2, 10, 15, 3, "PA", 2);
    COC_Key lvlDiff = mkKey(1, 2, 10, 16, 9, "PA", 2);
    EXPECT_EQ(COC_find(c, &hiDiff), nullptr);
    EXPECT_EQ(COC_find(c, &loDiff), nullptr);
    EXPECT_EQ(COC_find(c, &cidDiff), nullptr);
    EXPECT_EQ(COC_find(c, &fmtDiff), nullptr);
    EXPECT_EQ(COC_find(c, &lvlDiff), nullptr);

    // ... but an equal key (fresh param buffer, same bytes) hits.
    char pa[]    = "PA";
    COC_Key same = mkKey(1, 2, 10, 16, 3, pa, 2);
    EXPECT_NE(COC_find(c, &same), nullptr);
}

TEST(CodecOutputCacheTest, SkipCounters)
{
    Cache c;
    COC_recordSkip(c, COC_SKIP_REFPARAM);
    COC_recordSkip(c, COC_SKIP_REFPARAM);
    COC_recordSkip(c, COC_SKIP_DICT);
    COC_recordSkip(c, COC_SKIP_OTHER);
    auto s = ZL_CodecOutputCache_getStats(c);
    EXPECT_EQ(s.refParamSkips, 2u);
    EXPECT_EQ(s.dictSkips, 1u);
    EXPECT_EQ(s.otherSkips, 1u);
}

TEST(CodecOutputCacheTest, Reset)
{
    Cache c;
    COC_Key k    = mkKey(1, 2, 10, 16, 3, "PA", 2);
    COC_Output o = mkOut("x", 1);
    COC_Entry e  = mkEntry(&o, nullptr, 0);
    ASSERT_FALSE(ZL_isError(COC_insert(c, &k, &e)));
    ASSERT_NE(COC_find(c, &k), nullptr);

    ZL_CodecOutputCache_reset(c);
    auto s = ZL_CodecOutputCache_getStats(c);
    EXPECT_EQ(s.hits, 0u);
    EXPECT_EQ(s.misses, 0u);
    EXPECT_EQ(s.bytesStored, 0u);
    EXPECT_EQ(COC_find(c, &k), nullptr);

    // Still usable after reset.
    ASSERT_FALSE(ZL_isError(COC_insert(c, &k, &e)));
    EXPECT_NE(COC_find(c, &k), nullptr);
}

TEST(CodecOutputCacheTest, OverflowIsAnErrorAndStoresNothing)
{
    Cache c(8); // 8-byte budget
    COC_Key k      = mkKey(1, 2, 10, 16, 3, "PA", 2);
    const char p[] = "way too large for the budget";
    COC_Output o   = mkOut(p, sizeof(p));
    COC_Entry e    = mkEntry(&o, nullptr, 0);
    EXPECT_TRUE(ZL_isError(COC_insert(c, &k, &e)));
    auto s = ZL_CodecOutputCache_getStats(c);
    EXPECT_EQ(s.overflowed, 1u);
    EXPECT_EQ(s.bytesStored, 0u);
    EXPECT_EQ(COC_find(c, &k), nullptr);
}

TEST(CodecOutputCacheTest, ZeroOutputEntry)
{
    Cache c;
    COC_Key k   = mkKey(7, 7, 1, 16, 1, nullptr, 0);
    COC_Entry e = mkEntry(nullptr, nullptr, 0);
    ASSERT_FALSE(ZL_isError(COC_insert(c, &k, &e)));
    const COC_Entry* g = COC_find(c, &k);
    ASSERT_NE(g, nullptr);
    EXPECT_EQ(g->nbOutputs, 0u);
    EXPECT_FALSE(g->hasHeader);
}

TEST(CodecOutputCacheTest, FreeNullIsSafe)
{
    ZL_CodecOutputCache_free(nullptr);
    ZL_CodecOutputCache_reset(nullptr);
}
