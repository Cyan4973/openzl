// Copyright (c) Meta Platforms, Inc. and affiliates.

#include "openzl/compress/enc_interface.h" // ZL_Encoder definition
#include "openzl/common/allocation.h"      // ZL_malloc, ZL_free
#include "openzl/common/assertion.h"       // ZL_ASSERT, ZL_REQUIRE
#include "openzl/common/errors_internal.h" // ZL_TRY_LET
#include "openzl/common/introspection.h" // WAYPOINT, ZL_CompressIntrospectionHooks
#include "openzl/common/limits.h"
#include "openzl/common/operation_context.h"
#include "openzl/common/stream.h" // STREAM_nbIntMetadata, STREAM_getIntMetadataByIndex
#include "openzl/compress/cctx.h"   // CCTX_*
#include "openzl/compress/cgraph.h" // CGRAPH_getDictObj
#include "openzl/compress/cnode.h"
#include "openzl/compress/codec_output_cache.h" // ZL_CodecOutputCache, COC_*
#include "openzl/compress/localparams.h"
#include "openzl/compress/trStates.h"   // TRS_getState
#include "openzl/dict/dict_constants.h" // ZL_DICT_INDEX_NONE
#include "openzl/shared/xxhash.h"       // XXH3_128bits
#include "openzl/zl_common_types.h"     // ZL_TernaryParam_disable
#include "openzl/zl_compressor.h"
#include "openzl/zl_data.h"
#include "openzl/zl_output.h" // ZL_Output_ptr, ZL_Output_commit, ZL_Output_type
#include "openzl/zl_version.h"

ZL_Report ENC_initEICtx(
        ZL_Encoder* eictx,
        ZL_CCtx* cctx,
        Arena* wkspArena,
        const RTNodeID* rtnodeid,
        const CNode* cnode,
        const ZL_LocalParams* lparams,
        CachedStates* cachedStates)
{
    ZL_ASSERT_NN(eictx);
    ZL_ASSERT_NN(wkspArena);
    ZL_ASSERT_NN(rtnodeid);
    *eictx = (ZL_Encoder){ .cctx         = cctx,
                           .rtnodeid     = *rtnodeid,
                           .cnode        = cnode,
                           .wkspArena    = wkspArena,
                           .lparams      = lparams,
                           .cachedStates = cachedStates };
    return ZL_returnSuccess();
}

void ENC_destroyEICtx(ZL_Encoder* ei)
{
    ZL_ASSERT_NN(ei);
    ALLOC_Arena_freeAll(ei->wkspArena);
}

int ZL_Encoder_getCParam(const ZL_Encoder* eic, ZL_CParam gparam)
{
    ZL_ASSERT_NN(eic);
    return CCTX_getAppliedGParam(eic->cctx, gparam);
}

ZL_LocalIntParams ZL_Encoder_getLocalIntParams(const ZL_Encoder* eic)
{
    ZL_ASSERT_NN(eic);
    return LP_getLocalIntParams(eic->lparams);
}

ZL_IntParam ZL_Encoder_getLocalIntParam(const ZL_Encoder* eic, int intParamId)
{
    ZL_ASSERT_NN(eic);
    return LP_getLocalIntParam(eic->lparams, intParamId);
}

ZL_RefParam ZL_Encoder_getLocalParam(const ZL_Encoder* eic, int refParamId)
{
    ZL_ASSERT_NN(eic);
    return LP_getLocalRefParam(eic->lparams, refParamId);
}

ZL_CopyParam ZL_Encoder_getLocalCopyParam(
        const ZL_Encoder* eic,
        int copyParamId)
{
    ZL_ASSERT_NN(eic);
    ZL_LocalCopyParams const lgp = eic->lparams->copyParams;
    for (size_t n = 0; n < lgp.nbCopyParams; n++) {
        if (lgp.copyParams[n].paramId == copyParamId) {
            return lgp.copyParams[n];
        }
    }
    return (ZL_CopyParam){ .paramId   = ZL_LP_INVALID_PARAMID,
                           .paramPtr  = NULL,
                           .paramSize = 0 };
}

const ZL_LocalParams* ZL_Encoder_getLocalParams(const ZL_Encoder* eic)
{
    ZL_ASSERT_NN(eic);
    return eic->lparams;
}

const void* ZL_Encoder_getMaterializedDict(const ZL_Encoder* eictx)
{
    ZL_ASSERT_NN(eictx);
    if (eictx->cnode == NULL)
        return NULL;
    if ((unsigned)CCTX_getAppliedGParam(eictx->cctx, ZL_CParam_formatVersion)
        < ZL_MATERIALIZED_DICT_VERSION_MIN) {
        ZL_ASSERT_EQ(CNODE_getDictIndex(eictx->cnode), ZL_DICT_INDEX_NONE);
        return NULL;
    }
    uint32_t offset = CNODE_getDictIndex(eictx->cnode);
    if (offset == ZL_DICT_INDEX_NONE)
        return NULL;
    return CGRAPH_getDictObj(CCTX_getCGraph(eictx->cctx), offset);
}

const void* ZL_Encoder_getMParam(const ZL_Encoder* eictx)
{
    ZL_ASSERT_NN(eictx);
    if (eictx->cnode == NULL)
        return NULL;
    return CNODE_getMParamObj(eictx->cnode);
}

const void* ENC_getPrivateParam(const ZL_Encoder* eictx)
{
    return eictx->privateParam;
}

// ZL_Encoder_sendCodecHeader():
// Note : this operation can fail,
// in which case, the operation failure is marked,
// and the orchestrator later get to detect the issue and react adequately.
void ZL_Encoder_sendCodecHeader(
        ZL_Encoder* eictx,
        const void* trh,
        size_t trhSize)
{
    ZL_RESULT_DECLARE_SCOPE_REPORT(eictx);
    ZL_DLOG(SEQ, "ZL_Encoder_sendCodecHeader (%zu bytes)", trhSize);
    CWAYPOINT(on_ZL_Encoder_sendCodecHeader, eictx, trh, trhSize);
    ZL_ASSERT_NN(eictx);
    if (trhSize)
        ZL_ASSERT_NN(trh);
    if (eictx->hasSentTrHeader) {
        eictx->sendTransformHeaderError = ZL_REPORT_ERROR(
                transform_executionFailure, "Transform header sent twice");
        return;
    }
    eictx->hasSentTrHeader = 1;
    ZL_Report const r      = CCTX_sendTrHeader(
            eictx->cctx, eictx->rtnodeid, (ZL_RBuffer){ trh, trhSize });
    if (ZL_isError(r))
        eictx->sendTransformHeaderError = r;
}

ZL_Report ZL_Encoder_createAllOutBuffers(
        ZL_Encoder* eic,
        void* buffStarts[],
        const size_t buffSizes[],
        size_t nbBuffs)
{
    ZL_RESULT_DECLARE_SCOPE_REPORT(eic);

    /* General idea :
     *
     * 1) Access the definition of the node in the immutable cgraph,
     *    which is tracked from the RT_node within the RT_graph,
     *    itself tracked within the Encoder Interface Context (EICtx),
     *    in order to access the definition(s) of possible output stream Types.
     * 2) Stream type must be "ZL_Type_serial" when invoking this function.
     * 3) What matters is to know the nb of output streams declared
     * 4) Ensure that this nb matches @nbBuffs
     * 5) Loop over @buffSizes[], generate a buffer for each one.
     *    Return the pointers in @buffStarts.
     * 6) return success
     *    or return early if there was an issue (such as failed allocation).
     */

    /* TODO(@cyan) :
     * Retrieve the nb of output streams
     * as defined at transform's registration time,
     * then compare it to `nbBuffs`, ensure it's equal,
     * consider how to bubble up an error when it's not.
     **/
    ZL_ASSERT_NN(eic);

    // Triggering that assert means that
    // the user has been invoking this function twice
    // or has started creating some streams with ZL_Encoder_createTypedStream()
    // and then called ZL_Encoder_createAllOutBuffers() afterwards.
    // Both of these cases are in direct violation of the API contract.
    // Hence it's technically UB, though this is less stupid than previous case.
    ZL_ASSERT_EQ(
            RTGM_getNbOutStreams(CCTX_getRTGraph(eic->cctx), eic->rtnodeid),
            0,
            "Method ZL_Encoder_createAllOutBuffers() "
            "can only be invoked once ");

    for (int n = 0; n < (int)nbBuffs; n++) {
        ZL_Output* const data =
                ZL_Encoder_createTypedStream(eic, n, buffSizes[n], 1);
        ZL_ERR_IF_NULL(data, allocation);
        buffStarts[n] = ZL_Output_ptr(data);
        if (buffSizes[n] > 0 && buffStarts[n] == NULL)
            ZL_ERR(allocation);
    }
    return ZL_returnSuccess();
}

ZL_Output* ZL_Encoder_createTypedStream(
        ZL_Encoder* eic,
        int outStreamIndex,
        size_t eltsCapacity,
        size_t eltWidth)
{
    ZL_ASSERT_NN(eic);
    ZL_Data* ret = CCTX_getNewStream(
            eic->cctx, eic->rtnodeid, outStreamIndex, eltWidth, eltsCapacity);
    CWAYPOINT(
            on_ZL_Encoder_createTypedStream,
            eic,
            outStreamIndex,
            eltsCapacity,
            eltWidth,
            ZL_codemodDataAsOutput(ret));
    return ZL_codemodDataAsOutput(ret);
}

ZL_Output* ZL_Encoder_createStringStream(
        ZL_Encoder* eic,
        int outcomeIndex,
        size_t nbStringsMax,
        size_t sumStringLenMax)
{
    ZL_Output* const stringS =
            ZL_Encoder_createTypedStream(eic, outcomeIndex, sumStringLenMax, 1);
    if (stringS == NULL)
        return NULL;
    if (ZL_Output_type(stringS) != ZL_Type_string)
        return NULL;
    uint32_t* const stringLenArr =
            ZL_Output_reserveStringLens(stringS, nbStringsMax);
    if (stringLenArr == NULL)
        return NULL;
    return stringS;
}

// -------------------------------------------------
// Non-public methods
// -------------------------------------------------

ZL_Output* ENC_refTypedStream(
        ZL_Encoder* eictx,
        int outcomeIndex,
        size_t eltWidth,
        size_t nbElts,
        ZL_Input const* ref,
        size_t offsetBytes)
{
    ZL_ASSERT_NN(eictx);
    return ZL_codemodDataAsOutput(CCTX_refContentIntoNewStream(
            eictx->cctx,
            eictx->rtnodeid,
            outcomeIndex,
            eltWidth,
            nbElts,
            ZL_codemodInputAsData(ref),
            offsetBytes));
}

// ── codec-output cache helpers ─────────────────────────────────────────────

// Per-input contribution to the codec-output cache key: a 128-bit hash of one
// stream's (type, eltWidth, numElts, content, int-metadata). For a single-input
// codec this IS the key's content hash, so a producer can memoize it and a
// consumer can reuse it (STREAM_get/setKeyHash) instead of re-hashing identical
// bytes on every cache probe.
static XXH128_hash_t ENC_hashStreamForKey(const ZL_Data* d)
{
    XXH3_state_t hs;
    XXH3_INITSTATE(&hs);
    XXH3_128bits_reset(&hs);
    ZL_Type const t = ZL_Data_type(d);
    size_t const ew = ZL_Data_eltWidth(d);
    size_t const ne = ZL_Data_numElts(d);
    size_t const cs = ZL_Data_contentSize(d);
    XXH3_128bits_update(&hs, &t, sizeof(t));
    XXH3_128bits_update(&hs, &ew, sizeof(ew));
    XXH3_128bits_update(&hs, &ne, sizeof(ne));
    if (cs != 0) {
        XXH3_128bits_update(&hs, ZL_Data_rPtr(d), cs);
    }
    // Codec output can depend on the input's int-metadata, so it is part of
    // the key.
    size_t const nbMeta = STREAM_nbIntMetadata(d);
    XXH3_128bits_update(&hs, &nbMeta, sizeof(nbMeta));
    for (size_t m = 0; m < nbMeta; m++) {
        int mId, mValue;
        STREAM_getIntMetadataByIndex(d, m, &mId, &mValue);
        XXH3_128bits_update(&hs, &mId, sizeof(mId));
        XXH3_128bits_update(&hs, &mValue, sizeof(mValue));
    }
    return XXH3_128bits_digest(&hs);
}

// Build the cache key for this codec invocation. Returns false (recording the
// reason) when the invocation must not be cached: dict-backed codecs and
// codecs reading reference parameters, whose output cannot be proven a pure
// function of the key. The copy-params blob is serialized into the encoder's
// scratch arena (valid until ENC_destroyEICtx, i.e. through snapshot/insert).
static bool ENC_cocBuildKey(
        ZL_Encoder* eictx,
        const InternalTransform_Desc* trDesc,
        const ZL_Data* inStreams[],
        size_t nbInStreams,
        ZL_CodecOutputCache* cache,
        COC_Key* key)
{
    if (eictx->cnode != NULL
        && CNODE_getDictIndex(eictx->cnode) != ZL_DICT_INDEX_NONE) {
        COC_recordSkip(cache, COC_SKIP_DICT);
        return false;
    }
    const ZL_LocalParams* const lp = eictx->lparams;
    if (lp != NULL && lp->refParams.nbRefParams != 0) {
        COC_recordSkip(cache, COC_SKIP_REFPARAM);
        return false;
    }

    // 128-bit hash of the concatenated input contents (+ type/width/count).
    // Fast path: a single-input codec's content hash is exactly one stream's
    // per-input hash (ENC_hashStreamForKey). If the producer already memoized
    // it (a cache replay, or a stream stamped earlier), reuse it instead of
    // re-hashing identical bytes -- the dominant cost on cache-hit-heavy
    // sweeps.
    XXH128_hash_t h;
    uint64_t stampedHigh, stampedLow;
    if (nbInStreams == 1
        && STREAM_getKeyHash(inStreams[0], &stampedHigh, &stampedLow)) {
        h.high64 = stampedHigh;
        h.low64  = stampedLow;
    } else if (nbInStreams == 1) {
        h = ENC_hashStreamForKey(inStreams[0]);
    } else {
        // Multi-input: hash all inputs into one state (not reconstructable from
        // per-stream memos, so no fast path). Byte-identical to the original.
        XXH3_state_t hs;
        XXH3_INITSTATE(&hs);
        XXH3_128bits_reset(&hs);
        for (size_t i = 0; i < nbInStreams; i++) {
            const ZL_Data* const d = inStreams[i];
            ZL_Type const t        = ZL_Data_type(d);
            size_t const ew        = ZL_Data_eltWidth(d);
            size_t const ne        = ZL_Data_numElts(d);
            size_t const cs        = ZL_Data_contentSize(d);
            XXH3_128bits_update(&hs, &t, sizeof(t));
            XXH3_128bits_update(&hs, &ew, sizeof(ew));
            XXH3_128bits_update(&hs, &ne, sizeof(ne));
            if (cs != 0) {
                XXH3_128bits_update(&hs, ZL_Data_rPtr(d), cs);
            }
            // Codec output can depend on the input's int-metadata, so it is
            // part of the key.
            size_t const nbMeta = STREAM_nbIntMetadata(d);
            XXH3_128bits_update(&hs, &nbMeta, sizeof(nbMeta));
            for (size_t m = 0; m < nbMeta; m++) {
                int mId, mValue;
                STREAM_getIntMetadataByIndex(d, m, &mId, &mValue);
                XXH3_128bits_update(&hs, &mId, sizeof(mId));
                XXH3_128bits_update(&hs, &mValue, sizeof(mValue));
            }
        }
        h = XXH3_128bits_digest(&hs);
    }

    // Serialize the node's parameters (int params + copy params) into the
    // scratch arena so the key can be compared by exact content. All param
    // planes that affect codec output must be included; ref-params make the
    // node non-cacheable (handled above). The tokenize sort flag, for example,
    // is an int param -- omitting int params would collide tokenize with
    // tokenize_sorted.
    const ZL_LocalIntParams* const ip  = (lp != NULL) ? &lp->intParams : NULL;
    const ZL_LocalCopyParams* const cp = (lp != NULL) ? &lp->copyParams : NULL;
    size_t blobSize                    = 0;
    if (ip != NULL) {
        blobSize += ip->nbIntParams * (sizeof(int) + sizeof(int));
    }
    if (cp != NULL) {
        for (size_t j = 0; j < cp->nbCopyParams; j++) {
            blobSize +=
                    sizeof(int) + sizeof(size_t) + cp->copyParams[j].paramSize;
        }
    }
    void* blob = NULL;
    if (blobSize != 0) {
        blob = ALLOC_Arena_malloc(eictx->wkspArena, blobSize);
        if (blob == NULL) {
            return false; // cannot build key -> just don't cache
        }
        uint8_t* w = (uint8_t*)blob;
        if (ip != NULL) {
            for (size_t j = 0; j < ip->nbIntParams; j++) {
                int const id  = ip->intParams[j].paramId;
                int const val = ip->intParams[j].paramValue;
                memcpy(w, &id, sizeof(id));
                w += sizeof(id);
                memcpy(w, &val, sizeof(val));
                w += sizeof(val);
            }
        }
        if (cp != NULL) {
            for (size_t j = 0; j < cp->nbCopyParams; j++) {
                int const id     = cp->copyParams[j].paramId;
                size_t const psz = cp->copyParams[j].paramSize;
                memcpy(w, &id, sizeof(id));
                w += sizeof(id);
                memcpy(w, &psz, sizeof(psz));
                w += sizeof(psz);
                if (psz != 0) {
                    memcpy(w, cp->copyParams[j].paramPtr, psz);
                    w += psz;
                }
            }
        }
    }

    memset(key, 0, sizeof(*key));
    key->contentHashHigh = h.high64;
    key->contentHashLow  = h.low64;
    key->codecID         = (uint32_t)trDesc->publicDesc.gd.CTid;
    key->formatVersion =
            (uint32_t)ZL_Encoder_getCParam(eictx, ZL_CParam_formatVersion);
    key->compressionLevel =
            ZL_Encoder_getCParam(eictx, ZL_CParam_compressionLevel);
    key->copyParams     = blob;
    key->copyParamsSize = (uint32_t)blobSize;
    return true;
}

// Replay a memoized result into the RTGraph as if the codec had just run:
// recreate each output stream and its bytes, then re-emit the codec header.
static ZL_Report ENC_cocReplay(ZL_Encoder* eictx, const COC_Entry* entry)
{
    ZL_RESULT_DECLARE_SCOPE_REPORT(eictx);
    for (size_t i = 0; i < entry->nbOutputs; i++) {
        const COC_Output* const o = &entry->outputs[i];
        // Recreate from the codec's original output port (o->outcomeIndex), not
        // the sequential position: variable-output codecs create every stream
        // from a single VO port, so using `i` would mistype outputs past it.
        ZL_Output* out;
        if (o->contentSize != 0) {
            // Replay by REFERENCE: point the output stream at the cache-owned
            // blob instead of allocating a fresh buffer and memcpy'ing into it.
            // The blob is immutable and outlives this compression -- the cache
            // is only reset between files / top branches, never mid-compress --
            // and downstream codecs read their inputs read-only, so a const
            // reference produces a byte-identical frame while skipping the
            // per-graph copy that dominates replay cost. The cache HeapArena
            // returns 16-byte-aligned blobs, satisfying the numeric-stream
            // alignment requirement of STREAM_refConstBuffer.
            Stream* const wrap =
                    STREAM_createInArena(eictx->wkspArena, (ZL_DataID){ 0 });
            ZL_ERR_IF_NULL(wrap, allocation);
            ZL_ERR_IF_ERR(STREAM_refConstBuffer(
                    wrap, o->content, o->type, o->eltWidth, o->numElts));
            // CCTX_refContentIntoNewStream references wrap's buffer (the cache
            // blob) into a fresh RTGraph output stream and commits it; the
            // throwaway `wrap` is no longer needed afterwards (the new stream
            // aliases the raw pointer, not wrap), and is freed with wkspArena.
            out = ZL_codemodDataAsOutput(CCTX_refContentIntoNewStream(
                    eictx->cctx,
                    eictx->rtnodeid,
                    o->outcomeIndex,
                    o->eltWidth,
                    o->numElts,
                    wrap,
                    0));
            ZL_ERR_IF_NULL(out, allocation);
            // Already committed by the reference path -- do NOT commit again.
        } else {
            // Empty output: nothing to reference; allocate a zero-length
            // stream.
            out = ZL_Encoder_createTypedStream(
                    eictx, o->outcomeIndex, o->numElts, o->eltWidth);
            ZL_ERR_IF_NULL(out, allocation);
            ZL_ERR_IF_ERR(ZL_Output_commit(out, o->numElts));
        }
        ZL_ASSERT_EQ((int)ZL_Output_type(out), (int)o->type);
        for (size_t m = 0; m < o->nbIntMetas; m++) {
            ZL_ERR_IF_ERR(ZL_Output_setIntMetadata(
                    out, o->intMetas[m].mId, o->intMetas[m].mValue));
        }
        // Stamp the replayed stream with the producer's memoized content-key
        // hash (after metadata, which invalidates it) so the consuming codec
        // keys off it instead of re-hashing the replayed content. The replayed
        // content+metadata match exactly what produced this hash at snapshot.
        STREAM_setKeyHash(
                ZL_codemodOutputAsData(out), o->keyHashHigh, o->keyHashLow);
    }
    if (entry->hasHeader) {
        ZL_Encoder_sendCodecHeader(eictx, entry->header, entry->headerSize);
        ZL_ERR_IF_ERR(eictx->sendTransformHeaderError);
    }
    return ZL_returnSuccess();
}

// Snapshot the outputs (and codec header) just produced by a codec run and
// store them in the cache. String-typed outputs are not snapshotted (their
// side metadata is not yet captured); such invocations simply aren't cached.
static ZL_Report ENC_cocSnapshotInsert(
        ZL_Encoder* eictx,
        ZL_CodecOutputCache* cache,
        const COC_Key* key)
{
    ZL_RESULT_DECLARE_SCOPE_REPORT(eictx);
    ZL_CCtx* const cctx      = eictx->cctx;
    const RTGraph* const rtg = CCTX_getRTGraph(cctx);
    size_t const nbOut       = RTGM_getNbOutStreams(rtg, eictx->rtnodeid);

    COC_Output* outs = NULL;
    if (nbOut != 0) {
        outs = (COC_Output*)ALLOC_Arena_malloc(
                eictx->wkspArena, nbOut * sizeof(COC_Output));
        ZL_ERR_IF_NULL(outs, allocation);
        for (size_t i = 0; i < nbOut; i++) {
            RTStreamID const rtsid =
                    RTGM_getOutStreamID(rtg, eictx->rtnodeid, (int)i);
            const ZL_Data* const d = RTGM_getRStream(rtg, rtsid);
            ZL_Type const t        = ZL_Data_type(d);
            if (t == ZL_Type_string) {
                COC_recordSkip(cache, COC_SKIP_OTHER);
                return ZL_returnSuccess(); // run succeeded; just not cached
            }
            outs[i].type = t;
            outs[i].outcomeIndex =
                    (int)RTGM_getOutcomeID_fromRtstream(rtg, rtsid);
            outs[i].eltWidth    = ZL_Data_eltWidth(d);
            outs[i].numElts     = ZL_Data_numElts(d);
            outs[i].contentSize = ZL_Data_contentSize(d);
            outs[i].content     = ZL_Data_rPtr(d);
            size_t const nbMeta = STREAM_nbIntMetadata(d);
            outs[i].nbIntMetas  = nbMeta;
            if (nbMeta != 0) {
                COC_IntMeta* meta = (COC_IntMeta*)ALLOC_Arena_malloc(
                        eictx->wkspArena, nbMeta * sizeof(COC_IntMeta));
                ZL_ERR_IF_NULL(meta, allocation);
                for (size_t m = 0; m < nbMeta; m++) {
                    STREAM_getIntMetadataByIndex(
                            d, m, &meta[m].mId, &meta[m].mValue);
                }
                outs[i].intMetas = meta;
            } else {
                outs[i].intMetas = NULL;
            }
            // Memoize this output's content-key hash so a future consumer that
            // replays it can key off it without re-hashing (see
            // ENC_cocBuildKey).
            XXH128_hash_t const oh = ENC_hashStreamForKey(d);
            outs[i].keyHashHigh    = oh.high64;
            outs[i].keyHashLow     = oh.low64;
        }
    }

    COC_Entry entry;
    memset(&entry, 0, sizeof(entry));
    entry.nbOutputs = nbOut;
    entry.outputs   = outs;
    if (eictx->hasSentTrHeader) {
        ZL_RBuffer const hdr = CCTX_getNodeHeader(cctx, eictx->rtnodeid);
        entry.hasHeader      = true;
        entry.header         = hdr.start;
        entry.headerSize     = hdr.size;
    }
    return COC_insert(cache, key, &entry);
}

static ZL_Report ENC_runTransform_internal(
        ZL_Encoder* eictx,
        ZL_NodeID nodeid,
        const InternalTransform_Desc* trDesc,
        const ZL_Data* inStreams[],
        size_t nbInStreams)
{
    ZL_RESULT_DECLARE_SCOPE_REPORT(eictx);
    ZL_DLOG(BLOCK,
            "ENC_runTransform_internal (%s, nodeid=%zu, nbInputs=%zu)",
            CT_getTrName(trDesc),
            nodeid.nid,
            nbInStreams);
    ZL_RESULT_SCOPE_ADD_GRAPH_CONTEXT(
            (ZL_GraphContext){ .transformID = trDesc->publicDesc.gd.CTid,
                               .name        = trDesc->publicDesc.name });

    eictx->privateParam             = trDesc->privateParam;
    eictx->opaquePtr                = trDesc->publicDesc.opaque.ptr;
    eictx->sendTransformHeaderError = ZL_returnSuccess();

    // Codec-output cache: replay a memoized result on a hit, otherwise run the
    // codec normally and snapshot its result below.
    ZL_CodecOutputCache* const coc = CCTX_getCodecOutputCache(eictx->cctx);
    COC_Key cocKey;
    bool cocCacheable = false;
    bool cocHit       = false;
    if (coc != NULL) {
        cocCacheable = ENC_cocBuildKey(
                eictx, trDesc, inStreams, nbInStreams, coc, &cocKey);
        if (cocCacheable) {
            const COC_Entry* const cached = COC_find(coc, &cocKey);
            if (cached != NULL) {
                ZL_ERR_IF_ERR(ENC_cocReplay(eictx, cached));
                cocHit = true;
            }
        }
    }

    if (!cocHit) {
        // Run transform
        ZL_ASSERT_NN(trDesc->publicDesc.transform_f);
        IF_CWAYPOINT_ENABLED(on_codecEncode_start, eictx)
        {
            CWAYPOINT(
                    on_codecEncode_start,
                    eictx,
                    CCTX_getCGraph(eictx->cctx),
                    nodeid,
                    ZL_codemodDatasAsInputs(inStreams),
                    nbInStreams);
        }
        ZL_Report codecExecResult = (trDesc->publicDesc.transform_f(
                eictx, ZL_codemodDatasAsInputs(inStreams), nbInStreams));
        if (ZL_isError(codecExecResult)) {
            CWAYPOINT(on_codecEncode_end, eictx, NULL, 0, codecExecResult);
            ZL_ERR_IF_ERR_COERCE(
                    codecExecResult,
                    "transform %s failed",
                    CT_getTrName(trDesc));
        }
    }
    const RTGraph* rtgm       = CCTX_getRTGraph(eictx->cctx);
    const size_t nbOutStreams = RTGM_getNbOutStreams(rtgm, eictx->rtnodeid);
    IF_CWAYPOINT_ENABLED(on_codecEncode_end, eictx)
    {
        DECLARE_VECTOR_CONST_POINTERS_TYPE(ZL_Data);
        VECTOR_CONST_POINTERS(ZL_Data) odata = { 0 };
        VECTOR_INIT(odata, nbOutStreams);
        for (size_t i = 0; i < nbOutStreams; ++i) {
            RTStreamID rtsid =
                    RTGM_getOutStreamID(rtgm, eictx->rtnodeid, (int)i);
            const ZL_Data* d     = RTGM_getRStream(rtgm, rtsid);
            bool pushbackSuccess = VECTOR_PUSHBACK(odata, d);
            if (!pushbackSuccess) {
                VECTOR_DESTROY(odata);
                ZL_ERR(allocation,
                       "Unable to append to the waypoint odata vector");
            }
        }
        CWAYPOINT(
                on_codecEncode_end,
                eictx,
                ZL_codemodConstDatasAsOutputs(VECTOR_DATA(odata)),
                VECTOR_SIZE(odata),
                ZL_returnSuccess());
        VECTOR_DESTROY(odata);
    }

    // Check that we didn't encounter an error sending the transform header.
    ZL_ERR_IF_ERR(eictx->sendTransformHeaderError);

    // Check that the transform has generated
    // at least as many output streams as compulsory singleton outputs.
    // Note : the check could be more thorough, for example
    //        it could verify that all compulsory outputs have been created.
    //        This can't be done with a simple counter though,
    //        and would require contribution from the RTGraph Manager.
    size_t const nbOut1 = trDesc->publicDesc.gd.nbSOs;
    ZL_ERR_IF_LT(nbOutStreams, nbOut1, transform_executionFailure);

    unsigned const formatVersion =
            (unsigned)ZL_Encoder_getCParam(eictx, ZL_CParam_formatVersion);
    if (formatVersion < 9) {
        // Format versions less than 9 don't support 0 output streams.
        ZL_ERR_IF_EQ(
                nbOutStreams,
                0,
                formatVersion_unsupported,
                "Not supported until format version 9");
    }

    ZL_ERR_IF_GT(
            nbOutStreams,
            ZL_transformOutStreamsLimit(formatVersion),
            formatVersion_unsupported);

    if (cocCacheable && !cocHit) {
        ZL_ERR_IF_ERR(ENC_cocSnapshotInsert(eictx, coc, &cocKey));
    }

    return ZL_returnValue(nbOutStreams);
}

ZL_Report ENC_runTransform(
        const InternalTransform_Desc* trDesc,
        const ZL_Data* inputs[],
        size_t nbInputs,
        ZL_NodeID nodeid,
        RTNodeID rtnodeid,
        const CNode* cnode,
        const ZL_LocalParams* lparams,
        ZL_CCtx* cctx,
        Arena* wkspArena,
        CachedStates* trstates)
{
    ZL_RESULT_DECLARE_SCOPE_REPORT(cctx);
    ZL_ASSERT_NN(trDesc);
    ZL_DLOG(BLOCK,
            "ENC_runTransform on Transform '%s' (%u) (lparams=%p)",
            CNODE_getName(cnode),
            trDesc->publicDesc.gd.CTid,
            lparams);
    if (lparams == NULL)
        lparams = CNODE_getLocalParams(cnode);
    if (cnode->maybeDictIndex != ZL_DICT_INDEX_NONE
        && CCTX_getAppliedGParam(cctx, ZL_CParam_formatVersion)
                < ZL_MATERIALIZED_DICT_VERSION_MIN) {
        char const* const nodeName = CNODE_getName(cnode);
        ZL_ERR(formatVersion_unsupported,
               "Frame format version %u does not support dict-backed transforms. "
               "Node `%s` requires a dictionary; use format version >= %u.",
               CCTX_getAppliedGParam(cctx, ZL_CParam_formatVersion),
               nodeName == NULL ? "<unnamed>" : nodeName,
               ZL_MATERIALIZED_DICT_VERSION_MIN);
    }
    ZL_Encoder eiState;
    ZL_ERR_IF_ERR(ENC_initEICtx(
            &eiState, cctx, wkspArena, &rtnodeid, cnode, lparams, trstates));
    ZL_Report const transformRes = ENC_runTransform_internal(
            &eiState, nodeid, trDesc, inputs, nbInputs);
    ENC_destroyEICtx(&eiState);
    return transformRes;
}

void* ZL_Encoder_getScratchSpace(ZL_Encoder* ei, size_t size)
{
    CWAYPOINT(on_ZL_Encoder_getScratchSpace, ei, size);
    return ALLOC_Arena_malloc(ei->wkspArena, size);
}

ZL_CONST_FN
ZL_OperationContext* ZL_Encoder_getOperationContext(ZL_Encoder* ei)
{
    if (ei == NULL) {
        return NULL;
    }
    return ZL_CCtx_getOperationContext(ei->cctx);
}

void* ZL_Encoder_getState(ZL_Encoder* ei)
{
    ZL_ASSERT_NN(ei);
    return TRS_getCodecState(ei->cachedStates, ei->cnode);
}

const void* ZL_Encoder_getOpaquePtr(const ZL_Encoder* eictx)
{
    ZL_ASSERT_NN(eictx);
    return eictx->opaquePtr;
}
