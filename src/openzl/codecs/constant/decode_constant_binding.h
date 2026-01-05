// Copyright (c) Meta Platforms, Inc. and affiliates.

#ifndef ZSTRONG_TRANSFORMS_CONSTANT_DECODE_CONSTANT_BINDING_H
#define ZSTRONG_TRANSFORMS_CONSTANT_DECODE_CONSTANT_BINDING_H

#include "openzl/codecs/common/graph_constant.h"
#include "openzl/shared/portability.h"
#include "openzl/zl_dtransform.h"

ZL_BEGIN_C_DECLS

ZL_Report DI_constant_typed(ZL_Decoder* dictx, const ZL_Input* ins[]);
ZL_Report DI_constant_numeric(ZL_Decoder* dictx, const ZL_Input* ins[]);

#define DI_CONSTANT_SERIALIZED(id) \
    { .transform_f = DI_constant_typed, .name = "constant" }
#define DI_CONSTANT_FIXED(id) \
    { .transform_f = DI_constant_typed, .name = "constant" }
#define DI_CONSTANT_NUMERIC(id) \
    { .transform_f = DI_constant_numeric, .name = "constant_numeric" }

ZL_END_C_DECLS

#endif
