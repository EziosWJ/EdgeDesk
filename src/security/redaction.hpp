#pragma once

#include <cstddef>
#include <string>
#include <string_view>

// Centralized redaction for diagnostic text that reaches the audit log.
//
// Extension-provided error messages, stacks and URLs are untrusted: they can
// contain credentials copied from requests, tokens embedded in query strings,
// or multi-line text that tries to forge log entries. AuditLog::record applies
// this module at the sink so every call site gets the same protection.

namespace edgedesk {

inline constexpr std::string_view kAuditRedactionMarker = "[REDACTED]";
inline constexpr std::string_view kAuditTruncationMarker = "[TRUNCATED]";
// Hard upper bound for a single detail/path field at the audit sink.
inline constexpr std::size_t kAuditFieldMaxBytes = 8192;

// Replaces credentials and secret values in free-form diagnostic text:
//   - Authorization / Proxy-Authorization, Cookie / Set-Cookie header values
//   - key=value, key: value and "key":"value" pairs whose key names a secret
//   - bare `Bearer <token>` strings
//   - URL userinfo passwords, sensitive query parameters and fragments
// Non-sensitive surrounding text is preserved. The function is idempotent and
// the output is always valid UTF-8.
std::string redact_sensitive(std::string_view text);

// Truncates to at most max_bytes without splitting a UTF-8 code point and
// appends kAuditTruncationMarker. Returns the input unchanged when it fits.
std::string truncate_utf8(std::string_view text, std::size_t max_bytes);

// Redacts then bounds one audit field. Result is at most max_bytes and is
// valid UTF-8.
std::string sanitize_audit_field(std::string_view text,
                                 std::size_t max_bytes = kAuditFieldMaxBytes);

// Encodes one structured key=value field as a quoted string. Quotes,
// backslashes and control bytes are escaped, so the result can never contain
// an unescaped newline even before JSON escaping at the sink.
std::string quote_audit_field(std::string_view value);

} // namespace edgedesk
