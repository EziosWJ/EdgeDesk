#include "terminal/command_guard.hpp"

#include "core/types.hpp"

#include <algorithm>
#include <cctype>
#include <set>
#include <vector>

namespace edgedesk {
namespace {

struct SubstringPattern {
    const char* id;
    const char* needle;
    const char* description;
};

// Matched anywhere in the normalized line (these strings never appear in
// harmless commands).
const SubstringPattern kSubstringPatterns[] = {
    {"dd_raw_device", "dd if=/dev/zero of=/dev/", "向块设备写入数据（会破坏设备）"},
    {"dd_raw_device", "dd of=/dev/sd", "向块设备写入数据（会破坏设备）"},
    {"dd_raw_device", "dd of=/dev/mmcblk", "向块设备写入数据（会破坏设备）"},
    {"dd_raw_device", "dd of=/dev/nvme", "向块设备写入数据（会破坏设备）"},
    {"overwrite_device", "> /dev/sd", "直接覆盖块设备"},
    {"overwrite_device", "> /dev/mmcblk", "直接覆盖块设备"},
    {"overwrite_device", "> /dev/nvme", "直接覆盖块设备"},
    {"overwrite_device", "> /dev/mem", "写入物理内存设备"},
    {"fork_bomb", ":(){:|:&};:", "fork 炸弹会耗尽进程资源"},
    {"fork_bomb", ":(){ :|:& };:", "fork 炸弹会耗尽进程资源"},
    {"kill_all", "kill -9 -1", "终止当前用户的全部进程"},
    {"chmod_root", "chmod -r 777 / ", "递归放宽根目录权限，存在安全风险"},
    {"system_critical", "> /etc/passwd", "覆盖关键系统文件"},
    {"system_critical", "> /etc/shadow", "覆盖关键系统文件"},
    {"system_critical", "> /etc/fstab", "覆盖关键系统文件（可能导致设备无法启动）"},
};

// Command words that are dangerous when they are the command being executed
// (not an argument). Matched per shell segment after known command prefixes.
const struct {
    const char* id;
    const char* word;
    const char* description;
} kCommandWords[] = {
    {"power", "shutdown", "关机操作会中断设备服务"},
    {"power", "poweroff", "停机操作会中断设备服务"},
    {"power", "halt", "停机操作会中断设备服务"},
    {"power", "reboot", "重启操作会中断设备服务"},
    {"kill_all", "killall5", "终止系统中的全部进程"},
    {"partition", "fdisk", "修改磁盘分区表"},
    {"partition", "parted", "修改磁盘分区表"},
    {"mkfs", "mkfs", "格式化文件系统"},
    {"wipefs", "wipefs", "擦除文件系统签名"},
};

// Wrappers that may precede the real command name.
const std::set<std::string> kCommandPrefixes = {
    "sudo", "doas", "nohup", "time", "nice", "ionice", "setsid", "stdbuf",
    "busybox", "command", "exec", "pkexec", "runuser", "!",
};

// Top-level system directories whose recursive removal is never expected.
const std::set<std::string> kCriticalRoots = {
    "/bin", "/boot", "/dev", "/etc", "/home", "/lib", "/lib64", "/proc",
    "/root", "/run", "/sbin", "/srv", "/sys", "/usr", "/var",
};

std::string normalize_line(std::string_view line) {
    std::string out;
    out.reserve(line.size());
    bool last_space = false;
    for (const char ch : line) {
        const auto lowered = static_cast<char>(std::tolower(static_cast<unsigned char>(ch)));
        if (lowered == '\t') {
            if (!out.empty() && !last_space) out.push_back(' ');
            last_space = true;
            continue;
        }
        if (lowered == ' ') {
            if (out.empty() || last_space) continue;
            last_space = true;
            out.push_back(' ');
            continue;
        }
        last_space = false;
        out.push_back(lowered);
    }
    while (!out.empty() && out.back() == ' ') out.pop_back();
    return out;
}

std::vector<std::string> split_segments(const std::string& normalized) {
    std::vector<std::string> segments;
    std::string current;
    for (std::size_t i = 0; i < normalized.size(); ++i) {
        const char ch = normalized[i];
        if (ch == ';' || ch == '&' || ch == '|' || ch == '(' || ch == ')') {
            if (!current.empty()) segments.push_back(trim(current));
            current.clear();
            while (i + 1 < normalized.size() &&
                   (normalized[i + 1] == ';' || normalized[i + 1] == '&' || normalized[i + 1] == '|')) {
                ++i;
            }
            continue;
        }
        current.push_back(ch);
    }
    if (!current.empty()) segments.push_back(trim(current));
    return segments;
}

std::vector<std::string> split_tokens(const std::string& segment) {
    std::vector<std::string> tokens;
    std::string current;
    for (const char ch : segment) {
        if (ch == ' ') {
            if (!current.empty()) tokens.push_back(current);
            current.clear();
            continue;
        }
        current.push_back(ch);
    }
    if (!current.empty()) tokens.push_back(current);
    return tokens;
}

std::size_t skip_prefixes(const std::vector<std::string>& tokens) {
    std::size_t index = 0;
    while (index < tokens.size() && kCommandPrefixes.count(tokens[index]) > 0) ++index;
    return index;
}

bool rm_recursive_force_hits_root(const std::string& normalized) {
    for (const auto& segment : split_segments(normalized)) {
        const auto tokens = split_tokens(segment);
        std::size_t index = skip_prefixes(tokens);
        if (index >= tokens.size() || tokens[index] != "rm") continue;
        bool recursive = false;
        bool force = false;
        std::vector<std::string> targets;
        for (++index; index < tokens.size(); ++index) {
            const std::string& token = tokens[index];
            if (token == "--") continue;
            if (token.size() > 1 && token[0] == '-') {
                if (token.find('r') != std::string::npos || token == "--recursive") recursive = true;
                if (token.find('f') != std::string::npos || token == "--force") force = true;
                continue;
            }
            if (!token.empty()) targets.push_back(token);
        }
        if (!recursive || !force) continue;
        for (const auto& target : targets) {
            std::string cleaned = target;
            while (!cleaned.empty() && (cleaned.back() == '"' || cleaned.back() == '\'')) {
                cleaned.pop_back();
            }
            while (!cleaned.empty() && cleaned.back() == '/') {
                cleaned.pop_back();
            }
            if (cleaned.empty()) return true; // the target was "/" or "///"
            if (cleaned == "*" || cleaned == "~" || cleaned == "$home") return true;
            if (cleaned.front() != '/') continue;
            if (kCriticalRoots.count(cleaned) > 0) return true;
            // "/*" and "/.*" style wildcards on the root itself.
            if (cleaned.size() > 1 && cleaned[1] == '*') return true;
        }
    }
    return false;
}

// Catches quoted forms such as sh -c "rm -rf /" where tokenization is opaque.
bool rm_quoted_root(const std::string& normalized) {
    constexpr const char* kNeedle = "rm -rf /";
    std::size_t pos = 0;
    while ((pos = normalized.find(kNeedle, pos)) != std::string::npos) {
        const std::size_t after = pos + std::string_view(kNeedle).size();
        if (after >= normalized.size()) return true;
        const char next = normalized[after];
        if (next == '*' || next == '"' || next == '\'' || next == ';' || next == '&' || next == '|' ||
            next == ')' || next == ' ') {
            return true;
        }
        pos = after;
    }
    return false;
}

bool commands_contain(const std::string& normalized, const char* word) {
    const std::string needle(word);
    const std::string dotted = needle + ".";
    for (const auto& segment : split_segments(normalized)) {
        const auto tokens = split_tokens(segment);
        const std::size_t index = skip_prefixes(tokens);
        if (index >= tokens.size()) continue;
        const std::string& token = tokens[index];
        // Accept "mkfs", "mkfs.ext4", "mkfs.xfs" ...
        if (token == needle || token.rfind(dotted, 0) == 0) return true;
    }
    return false;
}

} // namespace

std::optional<GuardMatch> CommandGuard::inspect(std::string_view line) {
    if (line.empty()) return std::nullopt;
    const auto normalized = normalize_line(line);
    if (normalized.empty()) return std::nullopt;

    for (const auto& pattern : kSubstringPatterns) {
        if (normalized.find(pattern.needle) != std::string::npos) {
            return GuardMatch{pattern.id, pattern.description};
        }
    }
    if (rm_recursive_force_hits_root(normalized) || rm_quoted_root(normalized)) {
        return GuardMatch{"recursive_root_delete", "递归删除根目录或系统路径"};
    }
    if (normalized.rfind("chmod -r 777 /", 0) == 0) {
        return GuardMatch{"chmod_root", "递归放宽根目录权限，存在安全风险"};
    }
    for (const auto& pattern : kCommandWords) {
        if (commands_contain(normalized, pattern.word)) {
            return GuardMatch{pattern.id, pattern.description};
        }
    }
    return std::nullopt;
}

bool CommandGuard::breaks_line_tracking(unsigned char byte) {
    // Newline and backspace are handled by the caller; everything else in the
    // C0 range (arrows, Ctrl+A/E/U/W, escape sequences) resets tracking because
    // the cursor position can no longer be inferred.
    if (byte == '\r' || byte == '\n' || byte == 0x7f || byte == 0x08) return false;
    return byte < 0x20;
}

} // namespace edgedesk
