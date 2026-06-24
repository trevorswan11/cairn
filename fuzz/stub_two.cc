#include <fuzztest/fuzztest.h>
#include <gtest/gtest.h>

namespace {

auto excited_test(int) -> void { EXPECT_TRUE(true); }

} // namespace

FUZZ_TEST(ExcitedTest, excited_test).WithDomains(fuzztest::Positive<int>());
