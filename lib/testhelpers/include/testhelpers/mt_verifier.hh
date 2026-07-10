#pragma once

#include <mutex>
#include <string>
#include <string_view>
#include <vector>

namespace cairn::tests::helpers {

// Meant to be used in multithreaded contexts to avoid Catch's lack of macro thread safety
class mt_verifier {
  public:
    auto add_failure(std::string msg) -> void;
    auto check(bool condition, std::string_view expr, std::string_view file, int line) -> void;

    // Returns true only if there were errors emitted
    auto dump_if_error() -> bool;

  private:
    std::mutex               mutex_;
    std::vector<std::string> failures_;
};

#define MT_CHECK(verifier, cond) \
    (verifier).check(static_cast<bool>(cond), #cond, __FILE__, __LINE__)

#define THREAD_CHECK_FALSE(verifier, cond) \
    (verifier).check(!static_cast<bool>(cond), #cond, __FILE__, __LINE__)

#define MT_REQUIRE(verifier, cond)                              \
    do {                                                        \
        if (!(cond)) {                                          \
            (verifier).check(false, #cond, __FILE__, __LINE__); \
            return;                                             \
        }                                                       \
    } while (0)

#define THREAD_REQUIRE_FALSE(verifier, cond)                    \
    do {                                                        \
        if (cond) {                                             \
            (verifier).check(false, #cond, __FILE__, __LINE__); \
            return;                                             \
        }                                                       \
    } while (0)

} // namespace cairn::tests::helpers
