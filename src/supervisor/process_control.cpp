#include "supervisor/process_control.hpp"

#include "core/types.hpp"
#include "core/util.hpp"

#include <algorithm>
#include <atomic>
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
#include <poll.h>
#include <pthread.h>
#include <sched.h>
#include <signal.h>
#include <sstream>
#include <sys/prctl.h>
#include <sys/stat.h>
#include <sys/syscall.h>
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

std::string path_basename(const std::string& path) {
    const auto slash = path.rfind('/');
    return slash == std::string::npos ? path : path.substr(slash + 1);
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
                               std::string_view current_boot_id,
                               int anchor_pid, std::uint64_t anchor_start_ticks) {
    GroupProbe probe;
    probe.pgid = pgid;
    probe.leader_pid = leader_pid;
    const auto refuse = [&probe](std::string detail) {
        probe.state = GroupState::uncertain;
        probe.detail = std::move(detail);
        return probe;
    };
    if (pgid <= 1 || leader_start_ticks == 0) {
        return refuse("process group identity is incomplete");
    }
    if (recorded_boot_id.empty()) {
        return refuse("process group has no recorded boot ID");
    }
    // The caller passes the boot ID it believes is current (an injected value in
    // tests); an empty value means read the live one.
    const std::string current =
        current_boot_id.empty() ? read_boot_id() : std::string(current_boot_id);
    if (current.empty() || current != recorded_boot_id) {
        return refuse("boot ID changed; recorded process identity is no longer valid");
    }
    if (anchor_pid > 0 && anchor_pid == leader_pid) {
        return refuse("service identity anchor and leader refer to the same process");
    }
    if (anchor_pid > 0 && anchor_start_ticks == 0) {
        return refuse("service identity anchor has no recorded start time");
    }

    // If the original leader PID is visible, it must still be the same process
    // and must still lead this group/session. A different start time proves PID
    // (and PGID) reuse.
    bool leader_live = false;
    if (leader_pid > 0) {
        ProcStat leader;
        if (read_proc_stat(leader_pid, leader)) {
            if (leader.start_ticks != leader_start_ticks) {
                return refuse("leader PID was reused by a different process");
            }
            if (leader.pgrp != pgid || leader.session != pgid) {
                return refuse("leader no longer owns the recorded process group/session");
            }
            leader_live = leader.state != 'Z';
        }
    }

    // Verify the sentinel before scanning members. It must be the same process
    // (start time) and must still be in the managed session/group. A live
    // sentinel reserves the numeric PGID in the kernel, so a later
    // kill(-pgid) cannot reach a recycled group.
    bool anchor_alive = false;
    if (anchor_pid > 0) {
        ProcStat anchor;
        if (read_proc_stat(anchor_pid, anchor)) {
            if (anchor.start_ticks != anchor_start_ticks) {
                return refuse("identity anchor PID was reused by a different process");
            }
            if (anchor.pgrp != pgid || anchor.session != pgid) {
                return refuse("identity anchor no longer belongs to the managed process group");
            }
            anchor_alive = anchor.state != 'Z';
        }
    }

    DIR* dir = ::opendir("/proc");
    if (dir == nullptr) {
        return refuse("cannot inspect /proc to verify process group membership");
    }
    std::string detail;
    int members = 0;
    while (dirent* entry = ::readdir(dir)) {
        if (!is_numeric_name(entry->d_name)) continue;
        const int pid = std::atoi(entry->d_name);
        if (pid <= 0) continue;
        if (anchor_pid > 0 && pid == anchor_pid) continue;
        ProcStat stat;
        if (!read_proc_stat(pid, stat)) continue;
        if (stat.state == 'Z') continue;
        if (stat.pgrp != pgid) continue;
        ++members;
        // setsid() makes every descendant share SID == PGID == original leader
        // PID. A live member outside that session cannot be ours.
        if (stat.session != pgid) {
            ::closedir(dir);
            return refuse("process group contains a member outside the managed session");
        }
        if (stat.start_ticks < leader_start_ticks) {
            ::closedir(dir);
            return refuse("process group contains a member that predates the service instance");
        }
    }
    ::closedir(dir);

    probe.live_members = members;
    probe.anchor_alive = anchor_alive;
    if (anchor_pid > 0) {
        if (!anchor_alive && members > 0) {
            // Without the sentinel the PGID reservation depends on the current
            // members; signalling would reintroduce the check/kill race.
            return refuse("identity anchor exited while service processes remain");
        }
        if (!anchor_alive) {
            probe.state = GroupState::absent;
            probe.detail = "process group no longer exists";
            return probe;
        }
        if (members == 0) {
            probe.state = GroupState::anchor_only;
            probe.detail = "service workload exited; identity anchor must be reaped";
            return probe;
        }
        probe.state = GroupState::verified;
        return probe;
    }

    // Legacy instance started before the anchor existed. The leader is the only
    // proof that the PGID is still reserved, so descendant-only groups fail
    // closed until the instance is restarted (ADR-0019 migration boundary).
    if (members == 0) {
        probe.state = GroupState::absent;
        probe.detail = "process group no longer exists";
        return probe;
    }
    if (!leader_live) {
        return refuse("legacy process group has no live verified leader");
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
    // Linux binfmt_script only inspects BINPRM_BUF_SIZE (256) bytes and fails
    // with a misleading ENOEXEC beyond that. Reject it with the real cause.
    if (end >= 255) {
        error = "shebang line is too long for the kernel (limit 255 bytes)";
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
    if ((interpreter_stat.st_mode & (S_IWGRP | S_IWOTH)) != 0) {
        error = "shebang interpreter is writable by group or other users: " + out.interpreter;
        return false;
    }
    return true;
}

} // namespace

namespace {

// Group sentinel ("anchor"). It is created in the service session with
// CLONE_PARENT so it becomes a direct child of the Supervisor while inheriting
// the service's PGID/SID. It blocks every catchable signal and only exits when
// (a) the exec commit pipe reaches EOF because the service child died before
// exec, or (b) the Supervisor sends SIGKILL to the group. A live anchor keeps
// the numeric PGID reserved in the kernel, which removes the check-to-kill reuse
// window on kernels without process-group pidfds (ADR-0019 §1).
constexpr std::size_t kAnchorStackSize = 64 * 1024;
alignas(16) unsigned char g_anchor_stack[kAnchorStackSize];

struct AnchorSetup {
    int commit_read = -1;
};
AnchorSetup g_anchor_setup;

// The sentinel is cloned from the service child before exec and therefore
// inherits the Supervisor's entire descriptor table (IPC sockets, SQLite, log
// files, client connections). It never execs, so CLOEXEC cannot clean that up;
// close every descriptor except the commit pipe explicitly.
void close_all_except(int keep) {
#ifdef SYS_close_range
    bool ok = keep == 0 || ::syscall(SYS_close_range, 0u,
                                     static_cast<unsigned>(keep) - 1u, 0u) == 0;
    if (ok) {
        if (::syscall(SYS_close_range, static_cast<unsigned>(keep) + 1u, ~0u, 0u) == 0) {
            return;
        }
    }
#endif
    const long configured = ::sysconf(_SC_OPEN_MAX);
    const long limit = std::min<long>(configured > 0 ? configured : 1024, 65536);
    for (long fd = 0; fd < limit; ++fd) {
        if (fd != keep) ::close(static_cast<int>(fd));
    }
}

int anchor_trampoline(void*) {
    // Runs on the dedicated stack in the clone child. Only syscalls from here.
    const int commit_read = g_anchor_setup.commit_read;
    close_all_except(commit_read);
    (void)::prctl(PR_SET_NAME, "edgedesk-anchor", 0, 0, 0);
    sigset_t blocked;
    ::sigfillset(&blocked);
    (void)::sigprocmask(SIG_SETMASK, &blocked, nullptr);
    char byte = 0;
    ssize_t received = -1;
    do {
        received = ::read(commit_read, &byte, 1);
    } while (received < 0 && errno == EINTR);
    if (received != 1) ::_exit(0);
    ::close(commit_read);
    for (;;) ::pause();
    return 0;
}

struct AnchorReport {
    int pid = -1;
    int error = 0;
};

// Reads the anchor report written by the child before it blocks on the exec
// handshake. Bounded so a wedged child cannot block the event loop.
bool read_anchor_report(int fd, int& anchor_pid, std::string& error) {
    struct pollfd wait {};
    wait.fd = fd;
    wait.events = POLLIN;
    int result = -1;
    do {
        result = ::poll(&wait, 1, 5000);
    } while (result < 0 && errno == EINTR);
    if (result <= 0) {
        error = result == 0 ? "service child did not report its identity anchor"
                            : std::string("cannot read identity anchor report: ") +
                                  std::strerror(errno);
        return false;
    }
    AnchorReport report;
    ssize_t count = -1;
    do {
        count = ::read(fd, &report, sizeof(report));
    } while (count < 0 && errno == EINTR);
    if (count != static_cast<ssize_t>(sizeof(report)) || report.pid <= 0) {
        if (report.error != 0) {
            error = "cannot create service identity anchor: " +
                    std::string(std::strerror(report.error));
        } else {
            error = "service child did not report a usable identity anchor";
        }
        return false;
    }
    anchor_pid = report.pid;
    return true;
}

// Best-effort reaping for a failed spawn: the anchor exits when the commit pipe
// reaches EOF, but it may still be a child of this process. A short bounded
// loop avoids leaving a zombie behind while never blocking the event loop.
void reap_anchor_short(int pid) {
    if (pid <= 1) return;
    for (int attempt = 0; attempt < 100; ++attempt) {
        int status = 0;
        const pid_t result = ::waitpid(pid, &status, WNOHANG);
        if (result == pid || (result < 0 && errno == ECHILD)) return;
        ::usleep(1000);
    }
}

} // namespace

SpawnOutcome spawn_service(const ServiceConfig& config, const ResolvedIdentity& identity,
                           const std::string& exec_staging_directory) {
    SpawnOutcome outcome;
    if (::geteuid() != 0) {
        if (identity.uid != ::geteuid() || identity.gid != ::getegid()) {
            outcome.error = Error::make(ErrorCode::permission_denied,
                                        "supervisor must run as root to switch run_user/run_group");
            return outcome;
        }
    }

    // A single readable descriptor is opened once and used both for format
    // inspection and execution. The old code opened the path twice, which let a
    // rename/symlink swap make the inspected inode differ from the executed one
    // (PSH-07). O_NONBLOCK only guards against a FIFO blocking the open; the
    // descriptor is rejected below unless it is a regular executable file.
    int exec_fd = ::open(config.executable.c_str(), O_RDONLY | O_CLOEXEC | O_NONBLOCK);
    if (exec_fd < 0) {
        outcome.error = Error::make(ErrorCode::invalid_argument,
                                    "cannot open executable " + config.executable + ": " +
                                        std::strerror(errno), errno);
        return outcome;
    }
    if (exec_fd < 3) {
        const int raised = ::fcntl(exec_fd, F_DUPFD_CLOEXEC, 3);
        const int saved = errno;
        ::close(exec_fd);
        if (raised < 0) {
            outcome.error = Error::make(ErrorCode::io_error,
                                        "cannot secure executable descriptor: " +
                                            std::string(std::strerror(saved)), saved);
            return outcome;
        }
        exec_fd = raised;
    }
    struct stat exec_stat {};
    if (::fstat(exec_fd, &exec_stat) != 0 || !S_ISREG(exec_stat.st_mode) ||
        (exec_stat.st_mode & 0111) == 0) {
        outcome.error = Error::make(ErrorCode::invalid_argument,
                                    "executable is not a regular executable file: " + config.executable);
        ::close(exec_fd);
        return outcome;
    }
    const int exec_flags = ::fcntl(exec_fd, F_GETFL, 0);
    if (exec_flags >= 0) (void)::fcntl(exec_fd, F_SETFL, exec_flags & ~O_NONBLOCK);

    // Script entry points need a readable descriptor that survives exec so the
    // interpreter can open /proc/self/fd/<n>; this preserves inode identity and
    // avoids re-resolving the configured path after validation.
    ScriptHeader script;
    {
        std::string script_error;
        if (!read_script_header(exec_fd, script, script_error)) {
            outcome.error = Error::make(ErrorCode::invalid_argument, script_error);
            ::close(exec_fd);
            return outcome;
        }
    }
    outcome.script_entry = script.is_script;
    const int script_fd = script.is_script ? exec_fd : -1;
    const std::string script_exec_path =
        script.is_script ? "/proc/self/fd/" + std::to_string(exec_fd) : std::string();

    // fexecve()/execveat(AT_EMPTY_PATH) makes AT_EXECFN and comm the descriptor
    // number. Multicall binaries (for example Ubuntu's coreutils) dispatch on
    // that name, so stage a symlink with the real basename whose target is the
    // verified descriptor. The inode stays pinned; only the name is restored.
    std::string exec_path = script_exec_path;
    std::string staging_link;
    std::string staging_dir;
    if (!exec_staging_directory.empty()) {
        static std::atomic<unsigned long> sequence{0};
        const std::string component = path_basename(config.executable);
        if (component.empty() || component == "." || component == "..") {
            outcome.error = Error::make(ErrorCode::invalid_argument,
                                        "executable path has an unusable basename: " +
                                            config.executable);
            ::close(exec_fd);
            return outcome;
        }
        staging_dir = exec_staging_directory + "/run-" + std::to_string(::getpid()) + "-" +
                      std::to_string(sequence.fetch_add(1) + 1) + "-" + random_hex(4);
        const mode_t dir_mode = ::geteuid() == 0 ? 0711 : 0700;
        if (::mkdir(staging_dir.c_str(), dir_mode) != 0) {
            outcome.error = Error::make(ErrorCode::io_error,
                                        "cannot create executable staging directory " +
                                            staging_dir + ": " + std::strerror(errno), errno);
            ::close(exec_fd);
            return outcome;
        }
        // The daemon runs with umask 0077; restore the traversal bits that a
        // non-root service needs to reach its own symlink.
        if (::chmod(staging_dir.c_str(), dir_mode) != 0) {
            const int saved = errno;
            (void)::rmdir(staging_dir.c_str());
            outcome.error = Error::make(ErrorCode::io_error,
                                        "cannot set permissions on executable staging directory " +
                                            staging_dir + ": " + std::strerror(saved), saved);
            ::close(exec_fd);
            return outcome;
        }
        staging_link = staging_dir + "/" + component;
        const std::string target = "/proc/self/fd/" + std::to_string(exec_fd);
        if (::symlink(target.c_str(), staging_link.c_str()) != 0) {
            const int saved = errno;
            (void)::rmdir(staging_dir.c_str());
            outcome.error = Error::make(ErrorCode::io_error,
                                        "cannot stage executable " + config.executable + ": " +
                                            std::strerror(saved), saved);
            ::close(exec_fd);
            return outcome;
        }
        exec_path = staging_link;
    }

    const int cwd_fd = ::open(config.cwd.c_str(), O_PATH | O_DIRECTORY | O_CLOEXEC);
    if (cwd_fd < 0) {
        outcome.error = Error::make(ErrorCode::invalid_argument,
                                    "cannot open working directory " + config.cwd + ": " +
                                        std::strerror(errno), errno);
        ::close(exec_fd);
        if (!staging_link.empty()) (void)::unlink(staging_link.c_str());
        if (!staging_dir.empty()) (void)::rmdir(staging_dir.c_str());
        return outcome;
    }

    int log_pipe[2] = {-1, -1};
    int err_pipe[2] = {-1, -1};
    int handshake_pipe[2] = {-1, -1};
    int report_pipe[2] = {-1, -1};
    int commit_pipe[2] = {-1, -1};
    const auto close_initial = [&]() {
        if (log_pipe[0] >= 0) ::close(log_pipe[0]);
        if (log_pipe[1] >= 0) ::close(log_pipe[1]);
        if (err_pipe[0] >= 0) ::close(err_pipe[0]);
        if (err_pipe[1] >= 0) ::close(err_pipe[1]);
        if (handshake_pipe[0] >= 0) ::close(handshake_pipe[0]);
        if (handshake_pipe[1] >= 0) ::close(handshake_pipe[1]);
        if (report_pipe[0] >= 0) ::close(report_pipe[0]);
        if (report_pipe[1] >= 0) ::close(report_pipe[1]);
        if (commit_pipe[0] >= 0) ::close(commit_pipe[0]);
        if (commit_pipe[1] >= 0) ::close(commit_pipe[1]);
        ::close(cwd_fd);
        ::close(exec_fd);
        if (!staging_link.empty()) (void)::unlink(staging_link.c_str());
        if (!staging_dir.empty()) (void)::rmdir(staging_dir.c_str());
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
    if (::pipe2(report_pipe, O_CLOEXEC) != 0) {
        outcome.error = Error::make(ErrorCode::io_error,
                                    std::string("pipe2: ") + std::strerror(errno), errno);
        close_initial();
        return outcome;
    }
    if (::pipe2(commit_pipe, O_CLOEXEC) != 0) {
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
        ::close(report_pipe[0]);
        // commit_pipe[1] stays open: it releases the sentinel just before exec.
        // commit_pipe[0] is only used by the sentinel and is CLOEXEC.
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

        // The sentinel is created while this process is still root, so a
        // service running as an unprivileged user cannot terminate it. It is a
        // direct child of the Supervisor via CLONE_PARENT but inherits this
        // process's session and process group.
        g_anchor_setup.commit_read = commit_pipe[0];
        pid_t anchor = ::clone(anchor_trampoline,
                               g_anchor_stack + kAnchorStackSize,
                               CLONE_PARENT | SIGCHLD, nullptr);
        if (anchor < 0) {
            // QEMU user-mode on some architectures (notably aarch64 CI) rejects
            // CLONE_PARENT with EINVAL. Fall back to a plain fork: the sentinel
            // still inherits the session/PGID, closes every descriptor and keeps
            // the PGID reserved. The trade-off is that it becomes a child of the
            // service process instead of the Supervisor, so the Supervisor cannot
            // reap it and a service that waits for all children may block until
            // the stop-timeout SIGKILL; document this as the fallback boundary.
            anchor = ::fork();
            if (anchor == 0) {
                anchor_trampoline(nullptr);
                ::_exit(0);
            }
        }
        AnchorReport report;
        report.pid = anchor;
        report.error = anchor < 0 ? errno : 0;
        ssize_t reported = -1;
        do {
            reported = ::write(report_pipe[1], &report, sizeof(report));
        } while (reported < 0 && errno == EINTR);
        ::close(report_pipe[1]);
        if (anchor < 0) ::_exit(127);
        if (reported != static_cast<ssize_t>(sizeof(report))) {
            // The Supervisor disappeared before learning the anchor identity.
            // Exiting closes the commit pipe, so the sentinel exits as well.
            ::_exit(127);
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

        // Wait until the parent has persisted PID/PGID/boot ID/start time and
        // the anchor identity. EOF (parent crash or explicit abort) means the
        // target must not run.
        char handshake = 0;
        ssize_t received = -1;
        do {
            received = ::read(handshake_pipe[0], &handshake, 1);
        } while (received < 0 && errno == EINTR);
        ::close(handshake_pipe[0]);
        if (received != 1 || handshake != '1') ::_exit(127);

        // Commit the sentinel before exec: from now on it must outlive us even
        // if exec fails, so the Supervisor can still prove the group identity
        // while it cleans the failed start up.
        const char commit = '1';
        ssize_t committed = -1;
        do {
            committed = ::write(commit_pipe[1], &commit, 1);
        } while (committed < 0 && errno == EINTR);
        ::close(commit_pipe[1]);
        if (committed != 1) {
            const int value = EIO;
            (void)!::write(err_pipe[1], &value, sizeof(value));
            ::_exit(127);
        }

        if (script_fd >= 0) {
            // The interpreter must be able to reopen the script through this
            // descriptor, so clear CLOEXEC on this one FD only.
            (void)::fcntl(script_fd, F_SETFD, 0);
        }
        if (!exec_path.empty()) {
            ::execve(exec_path.c_str(), argv.data(), envp.data());
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
    ::close(report_pipe[1]);
    ::close(commit_pipe[0]);
    ::close(commit_pipe[1]);
    ::close(cwd_fd);
    ::close(exec_fd);
    const int flags = ::fcntl(log_pipe[0], F_GETFL, 0);
    if (flags >= 0) (void)::fcntl(log_pipe[0], F_SETFL, flags | O_NONBLOCK);
    (void)::fcntl(log_pipe[0], F_SETFD, FD_CLOEXEC);
    (void)::fcntl(err_pipe[0], F_SETFD, FD_CLOEXEC);

    const auto abort_spawn = [&](std::string message) {
        // Closing the handshake without writing makes the child exit 127; the
        // commit pipe then reaches EOF and the sentinel exits as well.
        if (handshake_pipe[1] >= 0) {
            ::close(handshake_pipe[1]);
            handshake_pipe[1] = -1;
        }
        (void)::kill(pid, SIGKILL);
        int status = 0;
        while (::waitpid(pid, &status, 0) < 0 && errno == EINTR) {
        }
        if (outcome.anchor_pid > 0) reap_anchor_short(outcome.anchor_pid);
        if (log_pipe[0] >= 0) ::close(log_pipe[0]);
        if (err_pipe[0] >= 0) ::close(err_pipe[0]);
        if (!staging_link.empty()) (void)::unlink(staging_link.c_str());
        if (!staging_dir.empty()) (void)::rmdir(staging_dir.c_str());
        outcome.exec_staging_link.clear();
        outcome.exec_staging_directory.clear();
        outcome.pid = -1;
        outcome.anchor_pid = -1;
        outcome.error = Error::make(ErrorCode::unavailable, std::move(message));
    };

    int anchor_pid = -1;
    std::string anchor_error;
    if (!read_anchor_report(report_pipe[0], anchor_pid, anchor_error)) {
        ::close(report_pipe[0]);
        abort_spawn(anchor_error);
        return outcome;
    }
    ::close(report_pipe[0]);
    outcome.anchor_pid = anchor_pid;

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
    if (start_ticks == 0) {
        abort_spawn("cannot determine child start time; refusing to manage the process");
        return outcome;
    }

    // Prove the sentinel inherited the service's session/group before the
    // caller persists it. A mismatch would make every later identity check fail.
    std::uint64_t anchor_start_ticks = 0;
    for (int attempt = 0; attempt < 50; ++attempt) {
        ProcStat anchor_stat;
        if (read_proc_stat(anchor_pid, anchor_stat)) {
            if (anchor_stat.start_ticks == 0 || anchor_stat.pgrp != pid ||
                anchor_stat.session != pid) {
                break;
            }
            anchor_start_ticks = anchor_stat.start_ticks;
            break;
        }
        ::usleep(1000);
    }
    if (anchor_start_ticks == 0) {
        abort_spawn("identity anchor did not join the service process group");
        return outcome;
    }

    outcome.pid = pid;
    outcome.pgid = pid;
    outcome.start_ticks = start_ticks;
    outcome.anchor_start_ticks = anchor_start_ticks;
    outcome.log_read_fd = log_pipe[0];
    outcome.exec_read_fd = err_pipe[0];
    outcome.handshake_fd = handshake_pipe[1];
    outcome.exec_staging_link = staging_link;
    outcome.exec_staging_directory = staging_dir;
    return outcome;
}

bool signal_service_group(const GroupProbe& probe, int signal, std::string& error) {
    if (probe.pgid <= 1 || signal <= 0) {
        error = "invalid process group signal request";
        return false;
    }
    if (probe.state == GroupState::anchor_only) {
        // Only the sentinel remains. SIGKILL is safe because the live sentinel
        // still reserves the PGID; anything else is not meaningful.
        if (signal != SIGKILL) {
            error = "only the identity anchor remains; SIGKILL is the only permitted signal";
            return false;
        }
    } else if (probe.state != GroupState::verified) {
        error = probe.detail.empty() ? "process group identity is not verifiable" : probe.detail;
        return false;
    }
    // For verified groups a live anchor (or, for legacy instances, a live
    // verified leader) keeps the numeric PGID reserved in the kernel, so this
    // kill cannot reach a recycled process group. ABSENT and UNCERTAIN never
    // reach here. A legacy descendant-only group is reported as UNCERTAIN.
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
