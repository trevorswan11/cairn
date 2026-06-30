#pragma once

#ifndef CAIRN_FUZZING
#    include <catch2/catch_test_macros.hpp>

#    define CAIRN_REQUIRE(expression) REQUIRE(expression)
#    define CAIRN_REQUIRE_FALSE(expression) REQUIRE_FALSE(expression)
#    define CAIRN_CHECK(expression) CHECK(expression)
#    define CAIRN_CHECK_FALSE(expression) CHECK_FALSE(expression)
#else
#    include <gtest/gtest.h>

#    define CAIRN_REQUIRE(expression) EXPECT_TRUE(expression)
#    define CAIRN_REQUIRE_FALSE(expression) EXPECT_FALSE(expression)
#    define CAIRN_CHECK(expression) CAIRN_REQUIRE(expression)
#    define CAIRN_CHECK_FALSE(expression) CAIRN_REQUIRE_FALSE(expression)
#endif
