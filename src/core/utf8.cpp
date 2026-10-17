#include "core/utf8.hpp"

namespace edgedesk {
namespace {

constexpr char kReplacement[] = "\xEF\xBF\xBD";
constexpr std::size_t kReplacementLength = sizeof(kReplacement) - 1;

std::size_t sequence_length(unsigned char lead) {
    if (lead < 0x80) return 1;
    if (lead >= 0xC2 && lead <= 0xDF) return 2;
    if (lead >= 0xE0 && lead <= 0xEF) return 3;
    if (lead >= 0xF0 && lead <= 0xF4) return 4;
    return 0;
}

bool allows_continuation(unsigned char lead, unsigned char byte, std::size_t position) {
    if ((byte & 0xC0) != 0x80) return false;
    // Reject overlong encodings, UTF-16 surrogates and values above U+10FFFF
    // at the first continuation byte.
    if (position == 1) {
        if (lead == 0xE0) return byte >= 0xA0;
        if (lead == 0xED) return byte <= 0x9F;
        if (lead == 0xF0) return byte >= 0x90;
        if (lead == 0xF4) return byte <= 0x8F;
    }
    return true;
}

} // namespace

std::string Utf8StreamSanitizer::feed(std::string_view bytes) {
    std::string out;
    out.reserve(pending_.size() + bytes.size());
    std::size_t index = 0;
    while (index < bytes.size()) {
        const char raw = bytes[index];
        const auto byte = static_cast<unsigned char>(raw);
        if (pending_.empty()) {
            const std::size_t length = sequence_length(byte);
            if (length == 0) {
                out.append(kReplacement, kReplacementLength);
            } else if (length == 1) {
                out.push_back(raw);
            } else {
                pending_.push_back(raw);
            }
            ++index;
            continue;
        }

        const auto lead = static_cast<unsigned char>(pending_.front());
        if (!allows_continuation(lead, byte, pending_.size())) {
            // Invalid continuation: replace the valid prefix accumulated so
            // far, then re-examine this byte as a potential new lead byte.
            out.append(kReplacement, kReplacementLength);
            pending_.clear();
            continue;
        }
        pending_.push_back(raw);
        ++index;
        if (pending_.size() == sequence_length(lead)) {
            out.append(pending_);
            pending_.clear();
        }
    }
    return out;
}

std::string Utf8StreamSanitizer::finish() {
    if (pending_.empty()) return {};
    pending_.clear();
    return std::string(kReplacement, kReplacementLength);
}

std::string sanitize_utf8(std::string_view input) {
    Utf8StreamSanitizer sanitizer;
    std::string out = sanitizer.feed(input);
    out += sanitizer.finish();
    return out;
}

} // namespace edgedesk
