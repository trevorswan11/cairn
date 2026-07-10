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
    auto dump_if_error() -> void;

  private:
    std::mutex               mutex_;
    std::vector<std::string> failures_;
};

#define THREAD_CHECK(verifier, cond) \
    (verifier).check(static_cast<bool>(cond), #cond, __FILE__, __LINE__)

#define THREAD_REQUIRE(verifier, cond)                          \
    do {                                                        \
        if (!(cond)) {                                          \
            (verifier).check(false, #cond, __FILE__, __LINE__); \
            return;                                             \
        }                                                       \
    } while (0)

} // namespace cairn::tests::helpers
