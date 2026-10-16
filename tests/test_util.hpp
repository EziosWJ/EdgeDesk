#pragma once

#include <cstdio>
#include <cstdlib>
#include <string>

// Minimal test harness shared by every native test executable.
// Usage: CHECK(condition); ... return edgedesk::test::finish("name");

namespace edgedesk::test {

inline int g_failures = 0;

inline void report(bool ok, const char* expression, const char* file, int line) {
    if (ok) return;
    ++g_failures;
    std::fprintf(stderr, "FAILED %s:%d: %s\n", file, line, expression);
}

inline void report_equal(bool ok, const char* expression, const char* file, int line, const std::string& a,
                         const std::string& b) {
    if (ok) return;
    ++g_failures;
    std::fprintf(stderr, "FAILED %s:%d: %s\n  left:  %s\n  right: %s\n", file, line, expression, a.c_str(),
                 b.c_str());
}

inline int finish(const char* name) {
    if (g_failures == 0) {
        std::printf("%s: PASS\n", name);
        return 0;
    }
    std::printf("%s: %d FAILURE(S)\n", name, g_failures);
    return 1;
}

template <typename T>
std::string to_display(const T& value) {
    return std::to_string(value);
}

inline std::string to_display(const std::string& value) { return value; }
inline std::string to_display(const char* value) { return value == nullptr ? "(null)" : value; }
inline std::string to_display(bool value) { return value ? "true" : "false"; }

} // namespace edgedesk::test

#define CHECK(expression) ::edgedesk::test::report(static_cast<bool>(expression), #expression, __FILE__, __LINE__)

#define CHECK_EQ(left, right)                                                                    \
    do {                                                                                         \
        const auto& check_left = (left);                                                         \
        const auto& check_right = (right);                                                       \
        ::edgedesk::test::report_equal(check_left == check_right, #left " == " #right, __FILE__, \
                                       __LINE__, ::edgedesk::test::to_display(check_left),       \
                                       ::edgedesk::test::to_display(check_right));               \
    } while (false)

#define CHECK_STR_EQ(left, right)                                                                  \
    do {                                                                                           \
        const std::string check_left = (left);                                                     \
        const std::string check_right = (right);                                                   \
        ::edgedesk::test::report_equal(check_left == check_right, #left " == " #right, __FILE__,   \
                                       __LINE__, check_left, check_right);                         \
    } while (false)
