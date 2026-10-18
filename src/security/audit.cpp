#include "security/audit.hpp"

#include "core/util.hpp"

#include <cerrno>
#include <cstdio>
#include <cstring>
#include <sys/stat.h>
#include <vector>

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

bool AuditLog::prune_older_than(std::int64_t cutoff_seconds, std::size_t& removed,
                                std::string& error) {
    removed = 0;
    error.clear();
    if (!enabled_) return true;
    std::lock_guard<std::mutex> lock(mutex_);
    if (stream_.is_open()) {
        stream_.flush();
        stream_.close();
    }
    std::vector<std::string> paths{options_.path};
    for (int index = 1; index <= options_.max_files; ++index) {
        paths.push_back(options_.path + "." + std::to_string(index));
    }
    for (const auto& path : paths) {
        std::ifstream input(path);
        if (!input) continue;
        const std::string temporary = path + ".prune.tmp";
        std::ofstream output(temporary, std::ios::out | std::ios::trunc);
        if (!output) {
            error = "cannot write audit prune temporary file: " + temporary;
            open_locked(true);
            return false;
        }
        std::string line;
        while (std::getline(input, line)) {
            std::int64_t stamp = 0;
            bool keep = true;
            // Records start with {"ts":"YYYY-MM-DDTHH:MM:SSZ".
            if (line.size() >= 28 && line.rfind("{\"ts\":\"", 0) == 0) {
                if (parse_iso8601_utc(std::string_view(line).substr(7, 20), stamp)) {
                    keep = stamp >= cutoff_seconds;
                }
            }
            if (keep) {
                output << line << '\n';
            } else {
                ++removed;
            }
        }
        output.flush();
        if (!output) {
            error = "cannot rewrite audit log: " + path;
            input.close();
            ::remove(temporary.c_str());
            open_locked(true);
            return false;
        }
        output.close();
        input.close();
        struct stat info {};
        if (::stat(path.c_str(), &info) == 0) {
            ::chmod(temporary.c_str(), info.st_mode & 07777);
        }
        if (::rename(temporary.c_str(), path.c_str()) != 0) {
            error = "cannot replace audit log: " + path;
            ::remove(temporary.c_str());
            open_locked(true);
            return false;
        }
    }
    open_locked(true);
    if (!stream_.is_open()) {
        error = "cannot reopen audit log after pruning";
        return false;
    }
    return true;
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
