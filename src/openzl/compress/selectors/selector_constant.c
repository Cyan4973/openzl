// Copyright (c) Meta Platforms, Inc. and affiliates.

#include "openzl/compress/selectors/selector_constant.h"
#include "openzl/common/assertion.h"
#include "openzl/compress/private_nodes.h"

/* Returns non-zero if the first element is all zeros */
static int isZeroConstant(const ZL_Input* inputStream)
{
    const uint8_t* ptr = ZL_Input_ptr(inputStream);
    size_t const eltWidth = ZL_Input_eltWidth(inputStream);
    for (size_t i = 0; i < eltWidth; ++i) {
        if (ptr[i] != 0) return 0;
    }
    return 1;
}

/* SI_selector_constant():
 *
 * The goal of this selector is to select between serialized,
 * fixed-size, or numeric constant encoding given an input that can be any of
 * these types.
 *
 * For numeric types, zeros are routed to CONSTANT_SERIAL (more efficient),
 * while non-zeros use CONSTANT_NUMERIC.
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
            /* Zeros are more efficiently handled via serial path */
            if (isZeroConstant(inputStream)) {
                return ZL_GRAPH_CONSTANT_SERIAL;
            }
            return ZL_GRAPH_CONSTANT_NUMERIC;
        case ZL_Type_string: /* fallthrough - not supported */
        default: ZL_REQUIRE(0, "Unsupported input type for constant selector");
    }
}
