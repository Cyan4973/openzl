// Copyright (c) Meta Platforms, Inc. and affiliates.

#include "openzl/compress/selectors/selector_constant.h"
#include "openzl/common/assertion.h"
#include "openzl/compress/private_nodes.h"

/* Returns non-zero if all bytes of the first element are identical.
 * This covers 0x00..00, 0xFF..FF, 0x55..55, etc.
 * For such patterns, Serial path is equally efficient. */
static int isSingleBytePattern(const ZL_Input* inputStream)
{
    const uint8_t* ptr = ZL_Input_ptr(inputStream);
    size_t const eltWidth = ZL_Input_eltWidth(inputStream);
    uint8_t const firstByte = ptr[0];
    for (size_t i = 1; i < eltWidth; ++i) {
        if (ptr[i] != firstByte) return 0;
    }
    return 1;
}

/* SI_selector_constant():
 *
 * The goal of this selector is to select between serialized,
 * fixed-size, or numeric constant encoding given an input that can be any of
 * these types.
 *
 * For numeric types, single-byte patterns (0x00, 0xFF, 0x55, etc.) are routed
 * to CONSTANT_SERIAL (more efficient), while other constants use
 * CONSTANT_NUMERIC.
 */

ZL_GraphID SI_selector_constant(
        const ZL_Selector* selCtx,
        const ZL_Input* inputStream,
        const ZL_GraphID* customGraphs,
        size_t nbCustomGraphs)
{
    (void)selCtx;
    (void)customGraphs;
    (void)nbCustomGraphs;

    ZL_Type const inType = ZL_Input_type(inputStream);
    ZL_ASSERT(
            inType == ZL_Type_serial || inType == ZL_Type_struct
            || inType == ZL_Type_numeric);
    ZL_ASSERT_GE(ZL_Input_eltWidth(inputStream), 1);

    switch (inType) {
        case ZL_Type_serial: return ZL_GRAPH_CONSTANT_SERIAL;
        case ZL_Type_struct: return ZL_GRAPH_CONSTANT_FIXED;
        case ZL_Type_numeric:
            /* Single-byte patterns (0x00, 0xFF, etc.) use serial path */
            if (isSingleBytePattern(inputStream)) {
                return ZL_GRAPH_CONSTANT_SERIAL;
            }
            return ZL_GRAPH_CONSTANT_NUMERIC;
        case ZL_Type_string: /* fallthrough - not supported */
        default: ZL_REQUIRE(0, "Unsupported input type for constant selector");
    }
}
