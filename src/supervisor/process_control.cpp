#include "supervisor/process_control.hpp"

#include "core/types.hpp"
#include "core/util.hpp"

#include <algorithm>
#include <cerrno>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <dirent.h>
#include <fcntl.h>
#include <fstream>
#include <grp.h>
#include <limits>
#include <map>
#include <pthread.h>
#include <signal.h>
#include <sstream>
#include <sys/stat.h>
#include <sys/wait.h>
#include <unistd.h>

namespace edgedesk::supervisor {
namespace {

bool parse_u64_field(const std::string& text, std::uint64_t& out) {
    if (text.empty()) return false;
    std::uint64_t value = 0;
    for (const char ch : text) {
        if (ch < '0' || ch > '9') return false;
        if (value > (std::numeric_limits<std::uint64_t>::max() - static_cast<std::uint64_t>(ch - '0')) / 10) {
            return false;
        }
        value = value * 10 + static_cast<std::uint64_t>(ch - '0');
    }
    out = value;
    return true;
}

bool parse_i64_field(const std::string& text, std::int64_t& out) {
    if (text.empty()) return false;
    bool negative = false;
    std::size_t start = 0;
    if (text[0] == '-') {
        negative = true;
        start = 1;
    }
    std::uint64_t value = 0;
    if (!parse_u64_field(text.substr(start), value)) return false;
    out = negative ? -static_cast<std::int64_t>(value) : static_cast<std::int64_t>(value);
    return true;
}

bool is_numeric_name(const char* name) {
    if (name == nullptr || name[0] == '\0') return false;
    for (const char* p = name; *p != '\0'; ++p) {
        if (*p < '0' || *p > '9') return false;
    }
    return true;
}

} // namespace

std::string read_boot_id() {
    std::ifstream input("/proc/sys/kernel/random/boot_id");
    if (!input) return {};
    std::string value;
    std::getline(input, value);
    value = trim(value);
    return value;
}

bool read_proc_stat(int pid, ProcStat& out) {
    out = {};
    if (pid <= 0) return false;
    std::ifstream input("/proc/" + std::to_string(pid) + "/stat");
    if (!input) return false;
    std::string line;
    std::getline(input, line);
    // The comm field may contain spaces and parentheses; parse after the last ')'.
    const auto close = line.rfind(')');
    if (close == std::string::npos || close + 2 >= line.size()) return false;
    std::istringstream fields(line.substr(close + 2));
    std::vector<std::string> tokens;
    std::string token;
    while (fields >> token && tokens.size() < 22) tokens.push_back(token);
    if (tokens.size() < 22) return false;
    out.pid = pid;
    out.state = tokens[0].empty() ? '?' : tokens[0][0];
    std::int64_t field = 0;
    if (!parse_i64_field(tokens[2], field)) return false;
    out.pgrp = static_cast<int>(field);
    if (!parse_i64_field(tokens[3], field)) return false;
    out.session = static_cast<int>(field);
    if (!parse_u64_field(tokens[11], out.utime_ticks)) return false;
    if (!parse_u64_field(tokens[12], out.stime_ticks)) return false;
    if (!parse_u64_field(tokens[19], out.start_ticks)) return false;
    if (!parse_i64_field(tokens[21], out.rss_pages)) return false;
    if (parse_i64_field(tokens[1], field)) out.ppid = static_cast<int>(field);
    out.valid = true;
    return true;
}

bool process_identity_matches(int pid, std::string_view boot_id, std::uint64_t start_ticks) {
    if (boot_id.empty() || start_ticks == 0) return false;
    if (read_boot_id() != boot_id) return false;
    ProcStat stat;
    return read_proc_stat(pid, stat) && stat.start_ticks == start_ticks;
}

bool process_group_has_any_members(int pgid) {
    if (pgid <= 1) return false;
    DIR* dir = ::opendir("/proc");
    if (dir == nullptr) return true;  // Fail closed: cannot prove absence.
    bool found = false;
    while (dirent* entry = ::readdir(dir)) {
        if (!is_numeric_name(entry->d_name)) continue;
        const int pid = std::atoi(entry->d_name);
        if (pid <= 0) continue;
        ProcStat stat;
        if (!read_proc_stat(pid, stat)) continue;
        if (stat.state == 'Z') continue;
        if (stat.pgrp == pgid) {
            found = true;
            break;
        }
    }
    ::closedir(dir);
    return found;
}

GroupProbe probe_service_group(int pgid, int leader_pid, std::uint64_t leader_start_ticks,
                               std::string_view recorded_boot_id,
                               std::string_view current_boot_id) {
    GroupProbe probe;
    probe.pgid = pgid;
    probe.leader_pid = leader_pid;
    if (pgid <= 1 || leader_start_ticks == 0) {
        probe.state = GroupState::uncertain;
        probe.detail = "process group identity is incomplete";
        return probe;
    }
    if (recorded_boot_id.empty()) {
        probe.state = GroupState::uncertain;
        probe.detail = "process group has no recorded boot ID";
        return probe;
    }
    // The caller passes the boot ID it believes is current (an injected value in
    // tests); an empty value means read the live one.
    const std::string current =
        current_boot_id.empty() ? read_boot_id() : std::string(current_boot_id);
    if (current.empty() || current != recorded_boot_id) {
        probe.state = GroupState::uncertain;
        probe.detail = "boot ID changed; recorded process identity is no longer valid";
        return probe;
    }

    // If the original leader PID is visible, it must still be the same process
    // and must still lead this group/session. A different start time proves PID
    // (and PGID) reuse.
    bool leader_visible = false;
    if (leader_pid > 0) {
        ProcStat leader;
        if (read_proc_stat(leader_pid, leader)) {
            leader_visible = true;
            if (leader.start_ticks != leader_start_ticks) {
                probe.state = GroupState::uncertain;
                probe.detail = "leader PID was reused by a different process";
                return probe;
            }
            if (leader.pgrp != pgid || leader.session != pgid) {
                probe.state = GroupState::uncertain;
                probe.detail = "leader no longer owns the recorded process group/session";
                return probe;
            }
        }
    }

    DIR* dir = ::opendir("/proc");
    if (dir == nullptr) {
        probe.state = GroupState::uncertain;
        probe.detail = "cannot inspect /proc to verify process group membership";
        return probe;
    }
    bool uncertain = false;
    std::string detail;
    int members = 0;
    while (dirent* entry = ::readdir(dir)) {
        if (!is_numeric_name(entry->d_name)) continue;
        const int pid = std::atoi(entry->d_name);
        if (pid <= 0) continue;
        ProcStat stat;
        if (!read_proc_stat(pid, stat)) continue;
        if (stat.state == 'Z') continue;
        if (stat.pgrp != pgid) continue;
        ++members;
        // setsid() makes every descendant share SID == PGID == original leader
        // PID. A live member outside that session cannot be ours. While a
        // session member exists the kernel reserves the SID's numeric PID, so
        // the identifier cannot be recycled underneath this check.
        if (stat.session != pgid) {
            uncertain = true;
            detail = "process group contains a member outside the managed session";
            break;
        }
        if (stat.start_ticks < leader_start_ticks) {
            uncertain = true;
            detail = "process group contains a member that predates the service instance";
            break;
        }
    }
    ::closedir(dir);
    if (uncertain) {
        probe.state = GroupState::uncertain;
        probe.detail = std::move(detail);
        return probe;
    }
    probe.live_members = members;
    if (members == 0) {
        probe.state = GroupState::absent;
        probe.detail = "process group no longer exists";
        return probe;
    }
    if (!leader_visible && leader_pid > 0) {
        // The leader is gone but live members remain; the session check above
        // anchors them to our instance. This is the descendant-cleanup path.
        probe.state = GroupState::verified;
        return probe;
    }
    probe.state = GroupState::verified;
    return probe;
}

long clock_ticks_per_second() {
    static const long value = ::sysconf(_SC_CLK_TCK);
    return value > 0 ? value : 100;
}

std::int64_t page_size_bytes() {
    static const std::int64_t value = ::sysconf(_SC_PAGESIZE);
    return value > 0 ? value : 4096;
}

namespace {

struct ScriptHeader {
    bool is_script = false;
    bool unreadable = false;
    std::string interpreter;
    std::string interpreter_arg;
};

// Parses a Linux shebang line without executing through a shell. Returns false
// with an actionable error for malformed headers or an unusable interpreter so
// the caller does not surface a misleading ENOENT.
bool read_script_header(int fd, ScriptHeader& out, std::string& error) {
    char buffer[512];
    const ssize_t count = ::pread(fd, buffer, sizeof(buffer) - 1, 0);
    if (count < 0) {
        out.unreadable = true;
        return true;
    }
    out.is_script = count >= 2 && buffer[0] == '#' && buffer[1] == '!';
    if (!out.is_script) return true;

    std::size_t end = 2;
    while (end < static_cast<std::size_t>(count) && buffer[end] != '\n') ++end;
    if (end >= sizeof(buffer) - 1) {
        error = "shebang line is too long";
        return false;
    }
    std::string line(buffer + 2, end - 2);
    const auto first = line.find_first_not_of(" \t");
    if (first == std::string::npos) {
        error = "shebang line has no interpreter";
        return false;
    }
    line.erase(0, first);
    const auto split = line.find_first_of(" \t");
    if (split == std::string::npos) {
        out.interpreter = line;
    } else {
        out.interpreter = line.substr(0, split);
        std::string rest = line.substr(split);
        const auto arg_first = rest.find_first_not_of(" \t");
        if (arg_first != std::string::npos) {
            rest.erase(0, arg_first);
            const auto arg_last = rest.find_last_not_of(" \t");
            rest.erase(arg_last + 1);
            out.interpreter_arg = rest;
        }
    }
    if (out.interpreter.empty()) {
        error = "shebang line has no interpreter";
        return false;
    }
    if (out.interpreter.front() != '/') {
        error = "shebang interpreter must be an absolute path: " + out.interpreter;
        return false;
    }
    struct stat interpreter_stat {};
    if (::stat(out.interpreter.c_str(), &interpreter_stat) != 0) {
        error = "shebang interpreter not found: " + out.interpreter + " (" +
                std::strerror(errno) + ")";
        return false;
    }
    if (!S_ISREG(interpreter_stat.st_mode) || (interpreter_stat.st_mode & 0111) == 0) {
        error = "shebang interpreter is not an executable file: " + out.interpreter;
        return false;
    }
    return true;
}

} // namespace

SpawnOutcome spawn_service(const ServiceConfig& config, const ResolvedIdentity& identity) {
    SpawnOutcome outcome;
    if (::geteuid() != 0) {
        if (identity.uid != ::geteuid() || identity.gid != ::getegid()) {
            outcome.error = Error::make(ErrorCode::permission_denied,
                                        "supervisor must run as root to switch run_user/run_group");
            return outcome;
        }
    }

    const int exec_fd = ::open(config.executable.c_str(), O_PATH | O_CLOEXEC);
    if (exec_fd < 0) {
        outcome.error = Error::make(ErrorCode::invalid_argument,
                                    "cannot open executable " + config.executable + ": " +
                                        std::strerror(errno), errno);
        return outcome;
    }
    struct stat exec_stat {};
    if (::fstat(exec_fd, &exec_stat) != 0 || !S_ISREG(exec_stat.st_mode) ||
        (exec_stat.st_mode & 0111) == 0) {
        outcome.error = Error::make(ErrorCode::invalid_argument,
                                    "executable is not a regular executable file: " + config.executable);
        ::close(exec_fd);
        return outcome;
    }

    // Script entry points need a readable descriptor that survives exec so the
    // interpreter can open /proc/self/fd/<n>; this preserves inode identity and
    // avoids re-resolving the configured path after validation.
    int script_fd = -1;
    ScriptHeader script;
    const int header_fd = ::open(config.executable.c_str(), O_RDONLY | O_CLOEXEC);
    if (header_fd >= 0) {
        std::string script_error;
        if (!read_script_header(header_fd, script, script_error)) {
            outcome.error = Error::make(ErrorCode::invalid_argument, script_error);
            ::close(header_fd);
            ::close(exec_fd);
            return outcome;
        }
        if (script.is_script) {
            script_fd = header_fd;
            outcome.script_entry = true;
        } else {
            ::close(header_fd);
        }
    } else if (errno != EACCES) {
        // The executable itself was opened O_PATH above; failure here means the
        // format cannot be inspected, not that the file is absent.
        outcome.error = Error::make(ErrorCode::invalid_argument,
                                    "cannot inspect executable " + config.executable + ": " +
                                        std::strerror(errno), errno);
        ::close(exec_fd);
        return outcome;
    }
    const std::string script_exec_path =
        script_fd >= 0 ? "/proc/self/fd/" + std::to_string(script_fd) : std::string();

    const int cwd_fd = ::open(config.cwd.c_str(), O_PATH | O_DIRECTORY | O_CLOEXEC);
    if (cwd_fd < 0) {
        outcome.error = Error::make(ErrorCode::invalid_argument,
                                    "cannot open working directory " + config.cwd + ": " +
                                        std::strerror(errno), errno);
        if (script_fd >= 0) ::close(script_fd);
        ::close(exec_fd);
        return outcome;
    }

    int log_pipe[2] = {-1, -1};
    int err_pipe[2] = {-1, -1};
    int handshake_pipe[2] = {-1, -1};
    const auto close_initial = [&]() {
        if (log_pipe[0] >= 0) ::close(log_pipe[0]);
        if (log_pipe[1] >= 0) ::close(log_pipe[1]);
        if (err_pipe[0] >= 0) ::close(err_pipe[0]);
        if (err_pipe[1] >= 0) ::close(err_pipe[1]);
        if (handshake_pipe[0] >= 0) ::close(handshake_pipe[0]);
        if (handshake_pipe[1] >= 0) ::close(handshake_pipe[1]);
        if (script_fd >= 0) ::close(script_fd);
        ::close(cwd_fd);
        ::close(exec_fd);
    };
    if (::pipe2(log_pipe, 0) != 0) {
        outcome.error = Error::make(ErrorCode::io_error, std::string("pipe2: ") + std::strerror(errno), errno);
        close_initial();
        return outcome;
    }
    if (::pipe2(err_pipe, O_CLOEXEC) != 0) {
        outcome.error = Error::make(ErrorCode::io_error, std::string("pipe2: ") + std::strerror(errno), errno);
        close_initial();
        return outcome;
    }
    if (::pipe2(handshake_pipe, O_CLOEXEC) != 0) {
        outcome.error = Error::make(ErrorCode::io_error,
                                    std::string("pipe2: ") + std::strerror(errno), errno);
        close_initial();
        return outcome;
    }

    // Build argv/envp before fork; the child never allocates.
    std::vector<std::string> argv_storage;
    argv_storage.push_back(config.executable);
    for (const auto& arg : config.args) argv_storage.push_back(arg);
    std::vector<char*> argv;
    argv.reserve(argv_storage.size() + 1);
    for (auto& arg : argv_storage) argv.push_back(const_cast<char*>(arg.c_str()));
    argv.push_back(nullptr);

    std::map<std::string, std::string> environment;
    environment["PATH"] = "/usr/local/sbin:/usr/local/bin:/usr/sbin:/usr/bin:/sbin:/bin";
    environment["HOME"] = identity.home;
    const std::string user_name = identity.user_name.empty() ? config.run_user : identity.user_name;
    environment["USER"] = user_name;
    environment["LOGNAME"] = user_name;
    for (const auto& item : config.env) environment[item.first] = item.second;
    std::vector<std::string> env_storage;
    env_storage.reserve(environment.size());
    for (const auto& item : environment) env_storage.push_back(item.first + "=" + item.second);
    std::vector<char*> envp;
    envp.reserve(env_storage.size() + 1);
    for (auto& item : env_storage) envp.push_back(const_cast<char*>(item.c_str()));
    envp.push_back(nullptr);

    const pid_t pid = ::fork();
    if (pid < 0) {
        outcome.error = Error::make(ErrorCode::io_error, std::string("fork: ") + std::strerror(errno), errno);
        close_initial();
        return outcome;
    }

    if (pid == 0) {
        // Child: only syscalls, no allocation or C++ objects.
        ::close(log_pipe[0]);
        ::close(err_pipe[0]);
        ::close(handshake_pipe[1]);
        if (::setsid() < 0) {
            const int value = errno;
            (void)!::write(err_pipe[1], &value, sizeof(value));
            ::_exit(127);
        }
        if (::dup2(log_pipe[1], STDOUT_FILENO) < 0 || ::dup2(log_pipe[1], STDERR_FILENO) < 0) {
            const int value = errno;
            (void)!::write(err_pipe[1], &value, sizeof(value));
            ::_exit(127);
        }
        if (log_pipe[1] > STDERR_FILENO) ::close(log_pipe[1]);
        if (::fchdir(cwd_fd) != 0) {
            const int value = errno;
            (void)!::write(err_pipe[1], &value, sizeof(value));
            ::_exit(127);
        }
        ::close(cwd_fd);
        ::umask(0022);
        sigset_t empty;
        ::sigemptyset(&empty);
        ::sigprocmask(SIG_SETMASK, &empty, nullptr);
        for (int sig = 1; sig < NSIG; ++sig) {
            if (sig == SIGKILL || sig == SIGSTOP) continue;
            ::signal(sig, SIG_DFL);
        }
        if (::geteuid() == 0) {
            if (::setgroups(identity.supplementary_groups.size(), identity.supplementary_groups.data()) != 0 ||
                ::setgid(identity.gid) != 0 || ::setuid(identity.uid) != 0) {
                const int value = errno;
                (void)!::write(err_pipe[1], &value, sizeof(value));
                ::_exit(127);
            }
        } else if (::getuid() != identity.uid || ::getgid() != identity.gid ||
                   ::geteuid() != identity.uid || ::getegid() != identity.gid) {
            const int value = EPERM;
            (void)!::write(err_pipe[1], &value, sizeof(value));
            ::_exit(127);
        }

        // Wait until the parent has persisted PID/PGID/boot ID/start time.
        // EOF (parent crash or explicit abort) means the target must not run.
        char handshake = 0;
        ssize_t received = -1;
        do {
            received = ::read(handshake_pipe[0], &handshake, 1);
        } while (received < 0 && errno == EINTR);
        ::close(handshake_pipe[0]);
        if (received != 1 || handshake != '1') ::_exit(127);

        if (script_fd >= 0) {
            // The interpreter must be able to reopen the script through this
            // descriptor, so clear CLOEXEC on this one FD only.
            (void)::fcntl(script_fd, F_SETFD, 0);
            ::execve(script_exec_path.c_str(), argv.data(), envp.data());
        } else {
            ::fexecve(exec_fd, argv.data(), envp.data());
        }
        const int value = errno;
        (void)!::write(err_pipe[1], &value, sizeof(value));
        ::_exit(127);
    }

    // Parent.
    ::close(log_pipe[1]);
    ::close(err_pipe[1]);
    ::close(handshake_pipe[0]);
    ::close(cwd_fd);
    ::close(exec_fd);
    if (script_fd >= 0) ::close(script_fd);
    const int flags = ::fcntl(log_pipe[0], F_GETFL, 0);
    if (flags >= 0) (void)::fcntl(log_pipe[0], F_SETFL, flags | O_NONBLOCK);
    (void)::fcntl(log_pipe[0], F_SETFD, FD_CLOEXEC);
    (void)::fcntl(err_pipe[0], F_SETFD, FD_CLOEXEC);

    ProcStat stat;
    std::uint64_t start_ticks = 0;
    // The child may not have been scheduled yet; retry briefly. A missing stat
    // is recorded as zero, in which case recovery will treat identity as
    // unverifiable rather than signaling a reused PID.
    for (int attempt = 0; attempt < 50; ++attempt) {
        if (read_proc_stat(pid, stat)) {
            start_ticks = stat.start_ticks;
            break;
        }
        ::usleep(1000);
    }

    outcome.pid = pid;
    outcome.pgid = pid;
    outcome.start_ticks = start_ticks;
    outcome.log_read_fd = log_pipe[0];
    outcome.exec_read_fd = err_pipe[0];
    outcome.handshake_fd = handshake_pipe[1];
    return outcome;
}

bool signal_service_group(const GroupProbe& probe, int signal, std::string& error) {
    if (probe.pgid <= 1 || signal <= 0) {
        error = "invalid process group signal request";
        return false;
    }
    if (probe.state != GroupState::verified) {
        error = probe.detail.empty() ? "process group identity is not verifiable" : probe.detail;
        return false;
    }
    // Residual TOCTOU: Linux cannot atomically bind kill(-pgid) to a verified
    // group without process-group pidfds. Verification above proves the session
    // lineage; if the group disappears before this call kill() returns ESRCH
    // and no unrelated group is signalled. If every member exits and the kernel
    // recycles the numeric PGID between verification and kill, the signal can
    // reach the new group. That window is inherent to the supported kernels and
    // is documented in ADR-0018; no additional plain /proc scan closes it.
    if (::kill(-probe.pgid, signal) != 0) {
        if (errno == ESRCH) {
            error = "process group no longer exists";
            return false;
        }
        error = std::string("kill(-") + std::to_string(probe.pgid) + "): " +
                std::strerror(errno);
        return false;
    }
    return true;
}

bool release_service_child(int& handshake_fd, std::string& error) {
    if (handshake_fd < 0) {
        error = "service child handshake channel is missing";
        return false;
    }
    // The child may have exited before reading (for example after a failed
    // setuid). Block SIGPIPE so a library-internal write cannot terminate the
    // Supervisor or a test process.
    sigset_t block;
    sigset_t previous;
    ::sigemptyset(&block);
    ::sigaddset(&block, SIGPIPE);
    (void)::pthread_sigmask(SIG_BLOCK, &block, &previous);
    const char byte = '1';
    ssize_t written = -1;
    do {
        written = ::write(handshake_fd, &byte, 1);
    } while (written < 0 && errno == EINTR);
    const int saved = errno;
    (void)::pthread_sigmask(SIG_SETMASK, &previous, nullptr);
    (void)::close(handshake_fd);
    handshake_fd = -1;
    if (written != 1) {
        error = std::string("cannot release service child: ") + std::strerror(saved);
        return false;
    }
    error.clear();
    return true;
}

void abort_service_child(int& handshake_fd) {
    if (handshake_fd >= 0) {
        (void)::close(handshake_fd);
        handshake_fd = -1;
    }
}

} // namespace edgedesk::supervisor
