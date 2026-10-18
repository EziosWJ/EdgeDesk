#include "test_util.hpp"

#include "core/utf8.hpp"

using namespace edgedesk;

namespace {

constexpr const char* kReplacement = "\xEF\xBF\xBD";
constexpr const char* kBoxHorizontal = "\xE2\x94\x80"; // U+2500
constexpr const char* kCjk = "\xE4\xB8\xAD\xE6\x96\x87"; // 中文
constexpr const char* kEmoji = "\xF0\x9F\x98\x80"; // U+1F600

void test_complete_input() {
    const std::string ascii = "hello\r\n\x1b[31mred\x1b[0m";
    CHECK_STR_EQ(sanitize_utf8(ascii), ascii);

    const std::string mixed = std::string("a") + kCjk + kBoxHorizontal + kEmoji + "z";
    CHECK_STR_EQ(sanitize_utf8(mixed), mixed);

    const std::string with_nul("a\0b", 3);
    CHECK_STR_EQ(sanitize_utf8(with_nul), with_nul);
}

void test_invalid_input() {
    CHECK_STR_EQ(sanitize_utf8("\xE2"), kReplacement);
    CHECK_STR_EQ(sanitize_utf8(std::string("a\xFF") + "b"), std::string("a") + kReplacement + "b");
    // Overlong encoding: C0 is never a valid lead byte.
    CHECK_STR_EQ(sanitize_utf8("\xC0\xAF"), std::string(kReplacement) + kReplacement);
    // Broken sequence followed by an ASCII byte keeps the ASCII byte.
    CHECK_STR_EQ(sanitize_utf8("\xE2\x28\xA1"), std::string(kReplacement) + "(" + kReplacement);
    // UTF-16 surrogates are not valid UTF-8.
    CHECK_STR_EQ(sanitize_utf8("\xED\xA0\x80"),
                 std::string(kReplacement) + kReplacement + kReplacement);
    // Values above U+10FFFF are rejected at the first continuation byte; each
    // remaining byte is ill-formed on its own.
    CHECK_STR_EQ(sanitize_utf8("\xF4\x90\x80\x80"),
                 std::string(kReplacement) + kReplacement + kReplacement + kReplacement);
}

void test_stream_split_sequences() {
    Utf8StreamSanitizer sanitizer;
    CHECK_STR_EQ(sanitizer.feed("ab\xE2"), "ab");
    CHECK_STR_EQ(sanitizer.feed("\x94"), "");
    CHECK_STR_EQ(sanitizer.feed(std::string("\x80") + "cd"), std::string(kBoxHorizontal) + "cd");
    CHECK_STR_EQ(sanitizer.finish(), "");

    CHECK_STR_EQ(sanitizer.feed(std::string_view(kEmoji).substr(0, 1)), "");
    CHECK_STR_EQ(sanitizer.feed(std::string_view(kEmoji).substr(1)), kEmoji);
    CHECK_STR_EQ(sanitizer.finish(), "");
}

void test_stream_incomplete_tail() {
    Utf8StreamSanitizer sanitizer;
    CHECK_STR_EQ(sanitizer.feed("\xE2\x94"), "");
    CHECK_STR_EQ(sanitizer.finish(), kReplacement);
    CHECK_STR_EQ(sanitizer.feed("ok"), "ok");

    CHECK_STR_EQ(sanitizer.feed("x\xE2"), "x");
    CHECK_STR_EQ(sanitizer.feed("A"), std::string(kReplacement) + "A");
    CHECK_STR_EQ(sanitizer.finish(), "");
}

void test_validator() {
    CHECK(is_valid_utf8(""));
    CHECK(is_valid_utf8("hello"));
    CHECK(is_valid_utf8(std::string(kCjk) + kBoxHorizontal + kEmoji));
    CHECK(!is_valid_utf8(std::string("a\xFF") + "b"));
    CHECK(!is_valid_utf8("\xE2"));
    CHECK(!is_valid_utf8("\xC0\xAF"));          // overlong
    CHECK(!is_valid_utf8("\xED\xA0\x80"));      // surrogate
    CHECK(!is_valid_utf8("\xF4\x90\x80\x80"));  // > U+10FFFF
    CHECK(!is_valid_utf8("\x80"));              // lone continuation
}

} // namespace

int main() {
    test_complete_input();
    test_invalid_input();
    test_stream_split_sequences();
    test_stream_incomplete_tail();
    test_validator();
    return edgedesk::test::finish("utf8_test");
}
