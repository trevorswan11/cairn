#include <fuzztest/fuzztest.h>
#include <gtest/gtest.h>

namespace {

auto happy_test(int) -> void { EXPECT_TRUE(true); }

} // namespace

FUZZ_TEST(HappyTest, happy_test).WithDomains(fuzztest::Positive<int>());
