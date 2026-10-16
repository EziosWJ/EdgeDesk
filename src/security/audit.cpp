#include "security/audit.hpp"

#include "core/util.hpp"

#include <cerrno>
#include <cstdio>
#include <cstring>
#include <sys/stat.h>

namespace edgedesk {
namespace {

std::string json_escape(std::string_view text) {
    std::string out;
    out.reserve(text.size() + 8);
    for (const char ch : text) {
        switch (ch) {
            case '"': out += "\\\""; break;
            case '\\': out += "\\\\"; break;
            case '\n': out += "\\n"; break;
            case '\r': out += "\\r"; break;
            case '\t': out += "\\t"; break;
            default:
                if (static_cast<unsigned char>(ch) < 0x20) {
                    char buffer[8];
                    std::snprintf(buffer, sizeof(buffer), "\\u%04x", static_cast<unsigned char>(ch));
                    out += buffer;
                } else {
                    out.push_back(ch);
                }
        }
    }
    return out;
}

} // namespace

std::unique_ptr<AuditLog> AuditLog::create(const AuditOptions& options, Error& error) {
    if (!options.enabled) {
        error = Error::none();
        auto log = std::unique_ptr<AuditLog>(new AuditLog(options));
        log->enabled_ = false;
        return log;
    }
    if (options.path.empty()) {
        error = Error::make(ErrorCode::invalid_argument, "audit log path must not be empty");
        return nullptr;
    }
    auto log = std::unique_ptr<AuditLog>(new AuditLog(options));
    log->open_locked(true);
    if (!log->stream_.is_open()) {
        error = Error::make(ErrorCode::io_error, "cannot open audit log: " + options.path);
        return nullptr;
    }
    error = Error::none();
    return log;
}

AuditLog::~AuditLog() {
    std::lock_guard<std::mutex> lock(mutex_);
    if (stream_.is_open()) stream_.flush();
}

void AuditLog::open_locked(bool append) {
    stream_.open(options_.path, append ? (std::ios::out | std::ios::app) : (std::ios::out | std::ios::trunc));
    if (!stream_.is_open()) return;
    struct stat info {};
    if (::stat(options_.path.c_str(), &info) == 0 && S_ISREG(info.st_mode)) {
        written_ = static_cast<std::uint64_t>(info.st_size);
    } else {
        written_ = 0;
    }
}

void AuditLog::rotate_locked() {
    stream_.close();
    // audit.log -> audit.log.1 -> ... -> audit.log.<max_files>; older removed.
    ::remove((options_.path + "." + std::to_string(options_.max_files + 1)).c_str());
    for (int index = options_.max_files - 1; index >= 1; --index) {
        const std::string from = options_.path + "." + std::to_string(index);
        const std::string to = options_.path + "." + std::to_string(index + 1);
        ::rename(from.c_str(), to.c_str());
    }
    ::rename(options_.path.c_str(), (options_.path + ".1").c_str());
    open_locked(false);
}

void AuditLog::record(std::string_view event, std::string_view outcome, std::string_view detail,
                      std::string_view ip, std::string_view path) {
    if (!enabled_) return;
    std::string line = "{\"ts\":\"" + format_iso8601_utc(now_unix_seconds()) + "\"";
    line += ",\"event\":\"" + json_escape(event) + "\"";
    line += ",\"outcome\":\"" + json_escape(outcome) + "\"";
    if (!detail.empty()) line += ",\"detail\":\"" + json_escape(detail) + "\"";
    if (!ip.empty()) line += ",\"ip\":\"" + json_escape(ip) + "\"";
    if (!path.empty()) line += ",\"path\":\"" + json_escape(path) + "\"";
    line += "}\n";

    std::lock_guard<std::mutex> lock(mutex_);
    if (!stream_.is_open()) return;
    if (written_ > 0 && written_ + line.size() > options_.max_bytes) {
        rotate_locked();
        if (!stream_.is_open()) return;
    }
    stream_ << line;
    stream_.flush();
    written_ += line.size();
}

} // namespace edgedesk
