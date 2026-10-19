#include "file/protected_paths.hpp"

#include "core/config.hpp"

#include <algorithm>
#include <filesystem>
#include <set>
#include <sys/stat.h>
#include <system_error>
#include <unordered_set>

namespace edgedesk {
namespace {

std::string canonicalize(std::string path) {
    if (path.empty()) return {};
    std::error_code error;
    std::filesystem::path candidate(path);
    if (candidate.is_relative()) {
        const auto absolute = std::filesystem::absolute(candidate, error);
        if (!error) {
            candidate = absolute;
        } else {
            error.clear();
        }
    }
    std::filesystem::path canonical = std::filesystem::weakly_canonical(candidate, error);
    if (error || canonical.empty()) {
        error.clear();
        canonical = candidate.lexically_normal();
    }
    std::string out = canonical.string();
    while (out.size() > 1 && out.back() == '/') out.pop_back();
    if (out.empty()) out = "/";
    return out;
}

bool is_within(std::string_view candidate, std::string_view base) {
    if (candidate == base) return true;
    if (base == "/") return !candidate.empty() && candidate.front() == '/';
    if (candidate.size() <= base.size()) return false;
    if (candidate.compare(0, base.size(), base) != 0) return false;
    return candidate[base.size()] == '/';
}

using Identity = std::pair<dev_t, ino_t>;

// Records the identity of every object below a protected directory. Hard
// links share an inode, so a link placed outside the protected tree can only
// be recognised by identity. Symbolic links are never followed; the visited
// set also stops bind-mount cycles.
void collect_tree_identities(const std::string& root, std::vector<Identity>& out) {
    namespace fs = std::filesystem;
    struct stat root_info {};
    if (::stat(root.c_str(), &root_info) != 0 || !S_ISDIR(root_info.st_mode)) return;
    out.emplace_back(root_info.st_dev, root_info.st_ino);

    std::set<Identity> visited;
    visited.emplace(root_info.st_dev, root_info.st_ino);
    std::error_code error;
    fs::recursive_directory_iterator it(root, fs::directory_options::skip_permission_denied, error);
    const fs::recursive_directory_iterator end;
    if (error) return;
    while (it != end) {
        struct stat info {};
        if (::lstat(it->path().c_str(), &info) == 0) {
            out.emplace_back(info.st_dev, info.st_ino);
            if (S_ISDIR(info.st_mode) && !visited.emplace(info.st_dev, info.st_ino).second) {
                it.disable_recursion_pending();
            }
        }
        it.increment(error);
        if (error) break;
    }
}

} // namespace

ProtectedPaths ProtectedPaths::from_paths(const std::vector<std::string>& paths) {
    ProtectedPaths result;
    std::unordered_set<std::string> seen;
    result.entries_.reserve(paths.size());
    for (const auto& path : paths) {
        const std::string canonical = canonicalize(path);
        if (canonical.empty() || !seen.insert(canonical).second) continue;
        ProtectedPathEntry entry;
        entry.canonical = canonical;
        struct stat info {};
        if (::stat(canonical.c_str(), &info) == 0) {
            entry.exists = true;
            entry.device = info.st_dev;
            entry.inode = info.st_ino;
            result.identities_.emplace_back(info.st_dev, info.st_ino);
            // Pointless (and unbounded) to index the whole host if "/" is
            // protected: the canonical guard already denies every path.
            if (S_ISDIR(info.st_mode) && canonical != "/") {
                collect_tree_identities(canonical, result.identities_);
            }
        }
        result.entries_.push_back(std::move(entry));
    }
    std::sort(result.identities_.begin(), result.identities_.end());
    result.identities_.erase(std::unique(result.identities_.begin(), result.identities_.end()),
                             result.identities_.end());
    return result;
}

bool ProtectedPaths::blocks_identity(dev_t device, ino_t inode) const {
    return std::binary_search(identities_.begin(), identities_.end(), Identity{device, inode});
}

bool ProtectedPaths::blocks_canonical(std::string_view canonical) const {
    for (const auto& entry : entries_) {
        if (is_within(canonical, entry.canonical)) return true;
    }
    return false;
}

std::vector<std::string> platform_protected_paths(const Config& config) {
    std::vector<std::string> paths;
    auto add = [&paths](const std::string& path) {
        if (!path.empty()) paths.push_back(path);
    };

    add(config.data_dir);
    add(config.state_dir);
    add(config.apps_root);
    add(config.audit_log_path);
    add(config.config_file);
    add(config.auth_token_file);

    if (!config.audit_log_path.empty()) {
        // Rotated siblings and the prune temporaries live next to the log.
        for (int index = 1; index <= config.audit_max_files; ++index) {
            add(config.audit_log_path + "." + std::to_string(index));
            add(config.audit_log_path + "." + std::to_string(index) + ".prune.tmp");
        }
        add(config.audit_log_path + ".prune.tmp");
    }
    return paths;
}

} // namespace edgedesk
