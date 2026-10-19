#include "security/redaction.hpp"

#include "core/utf8.hpp"
#include "core/util.hpp"

#include <algorithm>
#include <cstdio>

namespace edgedesk {
namespace {

char ascii_lower(char ch) {
    if (ch >= 'A' && ch <= 'Z') return static_cast<char>(ch - 'A' + 'a');
    return ch;
}

bool is_key_char(char ch) {
    return (ch >= 'a' && ch <= 'z') || (ch >= 'A' && ch <= 'Z') || (ch >= '0' && ch <= '9') ||
           ch == '_' || ch == '-';
}

bool is_horizontal_space(char ch) { return ch == ' ' || ch == '\t'; }

bool is_control(char ch) {
    const auto byte = static_cast<unsigned char>(ch);
    return byte < 0x20 || byte == 0x7f;
}

std::string normalize_key(std::string_view key) {
    std::string out;
    out.reserve(key.size());
    for (const char ch : key) {
        if (ch == '_' || ch == '-' || ch == ' ') continue;
        out.push_back(ascii_lower(ch));
    }
    return out;
}

bool ends_with(std::string_view text, std::string_view suffix) {
    return text.size() >= suffix.size() &&
           text.compare(text.size() - suffix.size(), suffix.size(), suffix) == 0;
}

bool starts_with(std::string_view text, std::string_view prefix) {
    return text.size() >= prefix.size() && text.compare(0, prefix.size(), prefix) == 0;
}

// Keys are matched after lowercasing and removing '-', '_' and spaces so
// "X-Api-Key", "api_key" and "APIKEY" share one rule. Suffix matching covers
// names such as csrf_token or client_secret without enumerating every product.
bool is_sensitive_key(std::string_view key) {
    const std::string normalized = normalize_key(key);
    if (normalized.empty()) return false;
    static constexpr const char* kExact[] = {
        "token", "accesstoken", "refreshtoken", "idtoken", "apikey", "secret",
        "password", "passwd", "pwd", "session", "cookie", "authorization",
        "setcookie", "proxyauthorization",
    };
    for (const char* exact : kExact) {
        if (normalized == exact) return true;
    }
    return ends_with(normalized, "token") || ends_with(normalized, "apikey") ||
           ends_with(normalized, "secret") || starts_with(normalized, "secret") ||
           ends_with(normalized, "password") || ends_with(normalized, "passwd") ||
           ends_with(normalized, "cookie") || ends_with(normalized, "authorization") ||
           starts_with(normalized, "session");
}

bool is_authorization_key(std::string_view key) {
    const std::string normalized = normalize_key(key);
    return normalized == "authorization" || normalized == "proxyauthorization";
}

bool is_header_secret_key(std::string_view key) {
    const std::string normalized = normalize_key(key);
    return normalized == "authorization" || normalized == "proxyauthorization" ||
           normalized == "cookie" || normalized == "setcookie";
}

// Ends an unquoted key=value token. Whitespace, separators used by JSON,
// forms, query strings and prose all terminate the value so non-sensitive
// context after it is preserved.
bool is_pair_value_end(char ch) {
    return ch == ' ' || is_control(ch) || ch == '&' || ch == ';' || ch == ',' || ch == '"' ||
           ch == '\'' || ch == '<' || ch == '>' || ch == '}' || ch == ']' || ch == ')' ||
           ch == '#' || ch == '`';
}

// Ends a credential token. Keeps base64url/JWT characters but stops at
// punctuation used to delimit free-form text.
bool is_credential_end(char ch) {
    return ch == ' ' || is_control(ch) || ch == '&' || ch == '#' || ch == '?' || ch == ',' ||
           ch == ';' || ch == '"' || ch == '\'' || ch == '<' || ch == '>' || ch == ')' ||
           ch == ']' || ch == '}' || ch == '`';
}

bool is_scheme_char(char ch) {
    return (ch >= 'a' && ch <= 'z') || (ch >= 'A' && ch <= 'Z') || (ch >= '0' && ch <= '9') ||
           ch == '-' || ch == '_';
}

std::size_t line_end(std::string_view text, std::size_t begin) {
    std::size_t end = begin;
    while (end < text.size() && text[end] != '\r' && text[end] != '\n') ++end;
    return end;
}

// Redacts one key/value pair starting at `i`. Returns false when the text at
// `i` is not a sensitive pair so the caller can continue normally.
bool redact_pair_at(std::string_view text, std::size_t& i, std::string& out) {
    const std::size_t size = text.size();
    std::size_t key_end = i + 1;
    while (key_end < size && is_key_char(text[key_end])) ++key_end;

    // A JSON-style quoted key: "access_token": "..." / 'password': '...'.
    std::size_t after_key = key_end;
    if (after_key < size && (text[after_key] == '"' || text[after_key] == '\'')) ++after_key;

    std::size_t separator = after_key;
    while (separator < size && is_horizontal_space(text[separator])) ++separator;
    if (separator >= size || (text[separator] != '=' && text[separator] != ':')) return false;

    const std::string_view key = text.substr(i, key_end - i);
    if (!is_sensitive_key(key)) return false;

    std::size_t value_begin = separator + 1;
    while (value_begin < size && is_horizontal_space(text[value_begin])) ++value_begin;
    if (value_begin >= size) return false;

    const bool quoted = text[value_begin] == '"' || text[value_begin] == '\'';
    std::size_t value_end = value_begin;
    std::size_t content_begin = value_begin;
    std::size_t content_end = value_begin;
    bool closed = false;

    if (quoted) {
        const char quote = text[value_begin];
        content_begin = value_begin + 1;
        std::size_t scan = content_begin;
        while (scan < size) {
            if (text[scan] == '\\' && scan + 1 < size) {
                scan += 2;
                continue;
            }
            if (text[scan] == quote) {
                content_end = scan;
                value_end = scan + 1;
                closed = true;
                break;
            }
            ++scan;
        }
        if (!closed) {
            content_end = size;
            value_end = size;
        }
        if (content_end == content_begin) return false;                 // empty value
        if (starts_with(text.substr(content_begin), kAuditRedactionMarker)) return false;
    } else {
        const char separator_char = text[separator];
        if (is_header_secret_key(key) && separator_char == ':') {
            // Header lines continue to the end of the line; this also covers
            // multi-parameter Authorization schemes.
            value_end = line_end(text, value_begin);
        } else {
            value_end = value_begin;
            while (value_end < size && !is_pair_value_end(text[value_end])) ++value_end;
        }
        if (value_end == value_begin) return false;                    // empty value
        if (starts_with(text.substr(value_begin), kAuditRedactionMarker)) return false;
    }

    out.append(text.data() + i, value_begin - i);
    if (quoted) {
        const char quote = text[value_begin];
        out.push_back(quote);
        out.append(kAuditRedactionMarker);
        if (closed) out.push_back(quote);
    } else if (is_authorization_key(key) && text[separator] == ':') {
        // Preserve the scheme (Bearer/Basic/Digest/...) for diagnostics and
        // redact the credentials that follow it.
        std::size_t scheme_end = value_begin;
        while (scheme_end < value_end && is_scheme_char(text[scheme_end])) ++scheme_end;
        if (scheme_end > value_begin && scheme_end < value_end &&
            is_horizontal_space(text[scheme_end])) {
            out.append(text.data() + value_begin, scheme_end - value_begin);
            out.push_back(' ');
            out.append(kAuditRedactionMarker);
        } else {
            out.append(kAuditRedactionMarker);
        }
    } else {
        out.append(kAuditRedactionMarker);
    }
    i = value_end;
    return true;
}

std::string redact_pairs(std::string_view text) {
    std::string out;
    out.reserve(text.size() + 16);
    std::size_t i = 0;
    while (i < text.size()) {
        const bool key_start = is_key_char(text[i]) && (i == 0 || !is_key_char(text[i - 1]));
        if (key_start && redact_pair_at(text, i, out)) continue;
        out.push_back(text[i]);
        ++i;
    }
    return out;
}

bool is_url_end(char ch) {
    return ch == ' ' || is_control(ch) || ch == '"' || ch == '\'' || ch == '<' || ch == '>' ||
           ch == '`' || ch == '\\';
}

std::string redact_url_component(std::string_view component, bool fragment) {
    const std::size_t equals = component.find('=');
    if (equals == std::string_view::npos) {
        // A fragment without a key/value structure cannot be classified; drop
        // it rather than risk persisting an opaque OAuth/access token.
        if (fragment && !component.empty() && component != kAuditRedactionMarker) {
            return std::string(kAuditRedactionMarker);
        }
        return std::string(component);
    }

    std::string_view key = component.substr(0, equals);
    std::string decoded_key;
    if (key.find('%') != std::string_view::npos && url_decode(key, decoded_key)) key = decoded_key;
    if (!is_sensitive_key(key)) return std::string(component);

    const std::string_view value = component.substr(equals + 1);
    if (value.empty() || starts_with(value, kAuditRedactionMarker)) return std::string(component);

    std::string out(component.substr(0, equals + 1));
    out.append(kAuditRedactionMarker);
    return out;
}

std::string redact_url_components(std::string_view list, bool fragment) {
    std::string out;
    out.reserve(list.size() + 16);
    std::size_t start = 0;
    for (std::size_t pos = 0; pos <= list.size(); ++pos) {
        if (pos == list.size() || list[pos] == '&' || list[pos] == ';') {
            out += redact_url_component(list.substr(start, pos - start), fragment);
            if (pos < list.size()) out.push_back(list[pos]);
            start = pos + 1;
        }
    }
    return out;
}

std::string redact_url_body(std::string_view url) {
    std::size_t authority_end = 0;
    while (authority_end < url.size() && url[authority_end] != '/' && url[authority_end] != '?' &&
           url[authority_end] != '#') {
        ++authority_end;
    }

    std::string authority(url.substr(0, authority_end));
    const std::size_t at = authority.rfind('@');
    if (at != std::string::npos) {
        std::string userinfo = authority.substr(0, at);
        const std::size_t colon = userinfo.find(':');
        if (colon != std::string::npos) {
            userinfo = userinfo.substr(0, colon + 1) + std::string(kAuditRedactionMarker);
        } else {
            userinfo = std::string(kAuditRedactionMarker);
        }
        authority = userinfo + authority.substr(at);
    }

    std::size_t query = url.size();
    std::size_t fragment = url.size();
    for (std::size_t pos = authority_end; pos < url.size(); ++pos) {
        if (url[pos] == '?' && query == url.size()) query = pos;
        if (url[pos] == '#') {
            fragment = pos;
            break;
        }
    }
    if (query > fragment) query = url.size();
    const std::size_t path_end = std::min(query, fragment);

    std::string out = authority;
    out.append(url.data() + authority_end, path_end - authority_end);
    if (query < url.size()) {
        const std::size_t query_end = fragment < url.size() ? fragment : url.size();
        out.push_back('?');
        out += redact_url_components(url.substr(query + 1, query_end - query - 1), false);
    }
    if (fragment < url.size()) {
        out.push_back('#');
        out += redact_url_components(url.substr(fragment + 1), true);
    }
    return out;
}

// Relative URLs and paths can carry a fragment without a scheme. Treat the
// fragment as URL-like when the adjacent token already contains URL syntax.
bool fragment_has_url_context(std::string_view text, std::size_t hash) {
    std::size_t start = hash;
    while (start > 0) {
        const char ch = text[start - 1];
        if (is_control(ch) || ch == ' ' || ch == '\t' || ch == '"' || ch == '\'' || ch == '<' ||
            ch == '>' || ch == '(' || ch == '[' || ch == '{' || ch == ',' || ch == ';' ||
            ch == '`' || ch == '\\') {
            break;
        }
        --start;
    }
    const std::string_view token = text.substr(start, hash - start);
    return token.find('/') != std::string_view::npos || token.find('?') != std::string_view::npos;
}

std::string redact_urls(std::string_view text) {
    std::string out;
    out.reserve(text.size() + 16);
    std::size_t i = 0;
    while (i < text.size()) {
        if (i + 3 <= text.size() && text.compare(i, 3, "://") == 0) {
            std::size_t end = i + 3;
            while (end < text.size() && !is_url_end(text[end])) ++end;
            out.append(text.data() + i, 3);
            out += redact_url_body(text.substr(i + 3, end - (i + 3)));
            i = end;
            continue;
        }
        if (text[i] == '#' && fragment_has_url_context(text, i)) {
            std::size_t end = i + 1;
            while (end < text.size() && !is_url_end(text[end])) ++end;
            out.push_back('#');
            out += redact_url_components(text.substr(i + 1, end - (i + 1)), true);
            i = end;
            continue;
        }
        out.push_back(text[i]);
        ++i;
    }
    return out;
}

std::string redact_bearers(std::string_view text) {
    std::string out;
    out.reserve(text.size() + 16);
    std::size_t i = 0;
    while (i < text.size()) {
        const bool word_start = (i == 0 || !is_key_char(text[i - 1]));
        if (word_start && i + 6 <= text.size()) {
            bool bearer = true;
            static constexpr char kBearer[] = "bearer";
            for (std::size_t offset = 0; offset < 6; ++offset) {
                if (ascii_lower(text[i + offset]) != kBearer[offset]) {
                    bearer = false;
                    break;
                }
            }
            if (bearer && (i + 6 == text.size() || !is_key_char(text[i + 6]))) {
                out.append(text.data() + i, 6);
                std::size_t token_begin = i + 6;
                while (token_begin < text.size() && is_horizontal_space(text[token_begin])) {
                    ++token_begin;
                }
                if (token_begin > i + 6) {
                    out.append(text.data() + i + 6, token_begin - (i + 6));
                    if (starts_with(text.substr(token_begin), kAuditRedactionMarker)) {
                        out.append(kAuditRedactionMarker);
                        i = token_begin + kAuditRedactionMarker.size();
                        continue;
                    }
                    std::size_t token_end = token_begin;
                    while (token_end < text.size() && !is_credential_end(text[token_end])) {
                        ++token_end;
                    }
                    if (token_end > token_begin) {
                        out.append(kAuditRedactionMarker);
                        i = token_end;
                        continue;
                    }
                }
                i = token_begin;
                continue;
            }
        }
        out.push_back(text[i]);
        ++i;
    }
    return out;
}

} // namespace

std::string redact_sensitive(std::string_view text) {
    std::string sanitized = sanitize_utf8(text);
    sanitized = redact_urls(sanitized);
    sanitized = redact_pairs(sanitized);
    sanitized = redact_bearers(sanitized);
    return sanitized;
}

std::string truncate_utf8(std::string_view text, std::size_t max_bytes) {
    if (text.size() <= max_bytes) return std::string(text);
    if (max_bytes <= kAuditTruncationMarker.size()) {
        return std::string(kAuditTruncationMarker.substr(0, max_bytes));
    }
    std::size_t keep = max_bytes - kAuditTruncationMarker.size();
    // If the cut lands inside a multi-byte sequence, move back to its lead
    // byte so the result remains valid UTF-8.
    while (keep > 0 && (static_cast<unsigned char>(text[keep]) & 0xC0) == 0x80) --keep;
    std::string out(text.substr(0, keep));
    out.append(kAuditTruncationMarker);
    return out;
}

std::string sanitize_audit_field(std::string_view text, std::size_t max_bytes) {
    return truncate_utf8(redact_sensitive(text), max_bytes);
}

std::string quote_audit_field(std::string_view value) {
    const std::string sanitized = sanitize_utf8(value);
    std::string out;
    out.reserve(sanitized.size() + 2);
    out.push_back('"');
    for (const char ch : sanitized) {
        switch (ch) {
            case '"': out += "\\\""; break;
            case '\\': out += "\\\\"; break;
            case '\n': out += "\\n"; break;
            case '\r': out += "\\r"; break;
            case '\t': out += "\\t"; break;
            default:
                if (is_control(ch)) {
                    char buffer[8];
                    std::snprintf(buffer, sizeof(buffer), "\\u%04x",
                                  static_cast<unsigned char>(ch));
                    out += buffer;
                } else {
                    out.push_back(ch);
                }
        }
    }
    out.push_back('"');
    return out;
}

} // namespace edgedesk
