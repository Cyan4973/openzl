// Copyright (c) Meta Platforms, Inc. and affiliates.

#include <random>
#include <vector>

#include <gtest/gtest.h>

#include "openzl/codecs/delta/decode_delta_kernel.h"
#include "openzl/codecs/delta/encode_delta_kernel.h"

namespace {
template <typename T>
class DeltaKernelTest : public testing::Test {
   public:
    void testInput(std::vector<T> const& input)
    {
        T first;
        std::vector<T> delta(input.size() - 1);
        std::vector<T> output(input.size());

        ZS_deltaEncode(
                &first, delta.data(), input.data(), input.size(), sizeof(T));

        ZS_deltaDecode(
                output.data(), &first, delta.data(), input.size(), sizeof(T));

        EXPECT_EQ(output, input);
    }

    T base = T(1) << (sizeof(T) * 8 - 1);
};

using DeltaTypes = ::testing::Types<uint8_t, uint16_t, uint32_t, uint64_t>;
TYPED_TEST_SUITE(DeltaKernelTest, DeltaTypes, );

TYPED_TEST(DeltaKernelTest, EmptyRoundTrip)
{
    ZS_deltaEncode(nullptr, nullptr, nullptr, 0, sizeof(TypeParam));
    ZS_deltaDecode(nullptr, nullptr, nullptr, 0, sizeof(TypeParam));
}

TYPED_TEST(DeltaKernelTest, OneRoundTrip)
{
    std::vector<TypeParam> input = { this->base };
    this->testInput(input);
}

TYPED_TEST(DeltaKernelTest, RoundTrip)
{
    std::vector<TypeParam> input = {
        TypeParam(this->base),
        TypeParam(this->base + 1),
        TypeParam(this->base - 1),
        TypeParam(this->base + 2),
        TypeParam(this->base + 3),
        TypeParam(this->base - 5),
        0,
        TypeParam(this->base + (this->base - 1)),
        0,
        1,
        255,
    };
    this->testInput(input);
}

// Vectorized kernels process up to 64 elements per iteration:
// sizes up to 150 cover several iterations and every tail length.
TYPED_TEST(DeltaKernelTest, SizeSweepRoundTrip)
{
    std::mt19937_64 gen(0xde17a);
    for (size_t n = 1; n <= 150; ++n) {
        SCOPED_TRACE(n);
        std::vector<TypeParam> input(n);
        for (auto& v : input) {
            v = TypeParam(gen());
        }
        this->testInput(input);
    }
}
} // namespace
