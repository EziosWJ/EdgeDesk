#pragma once

#include <string>
#include <string_view>

// UTF-8 helpers for text protocols. PTY output is a byte stream and a
// multi-byte character may be split across read() chunks, so callers must not
// assume an individual chunk is valid UTF-8.

namespace edgedesk {

// Replaces ill-formed UTF-8 bytes with U+FFFD. The result is always valid
// UTF-8 and can be embedded in JSON text without throwing.
std::string sanitize_utf8(std::string_view input);

// Streaming sanitizer for chunked byte producers. A valid multi-byte sequence
// split across feed() calls is carried over to the next call instead of being
// replaced. finish() flushes an incomplete trailing sequence as U+FFFD.
class Utf8StreamSanitizer {
public:
    std::string feed(std::string_view bytes);
    std::string finish();

private:
    std::string pending_;
};

} // namespace edgedesk
