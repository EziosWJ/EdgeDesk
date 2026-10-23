#pragma once

#include "core/error.hpp"
#include "supervisor/supervisor_types.hpp"

#include <cstdint>
#include <string>

// Low-level Linux process inspection and spawning helpers.
//
// The Supervisor is single-threaded, so fork() never runs while another thread
// holds malloc/NSS locks. Identity and NSS resolution happen in the parent
// before fork; the child only performs async-signal-safe syscalls before
// fexecve().

namespace edgedesk::supervisor {

struct ProcStat {
    bool valid = false;
    int pid = 0;
    char state = '?';
    int ppid = 0;
    int pgrp = 0;
    int session = 0;
    std::uint64_t start_ticks = 0;
    std::uint64_t utime_ticks = 0;
    std::uint64_t stime_ticks = 0;
    std::int64_t rss_pages = 0;
};

// Reads /proc/sys/kernel/random/boot_id. Empty on failure.
std::string read_boot_id();

bool read_proc_stat(int pid, ProcStat& out);

// Same boot + same kernel start time means the PID still names the process we
// started. A mismatched start time means the PID was reused.
bool process_identity_matches(int pid, std::string_view boot_id, std::uint64_t start_ticks);

// Result of proving that a numeric process group still belongs to one service
// instance (SPEC-0010 PSH-01).
//
// A group is only VERIFIED when every live non-anchor member is in the
// service's own session (setsid makes SID == PGID == original leader PID), no
// member predates the recorded leader start time, and both a still-visible
// leader and the identity anchor match their recorded start times.
//
// The anchor is a Supervisor-created sentinel that lives in the service's
// process group and ignores every catchable signal. While it is alive the
// kernel cannot recycle the numeric PGID, so check-to-kill races on old kernels
// are eliminated without pidfds (ADR-0019 §1). `anchor_only` means the service
// workload is gone but the sentinel still has to be reaped with SIGKILL;
// `uncertain` still means "refuse to signal".
enum class GroupState { absent, verified, anchor_only, uncertain };

struct GroupProbe {
    GroupState state = GroupState::absent;
    int pgid = 0;
    int leader_pid = 0;
    // Live non-anchor members. The anchor is never counted as a service process.
    int live_members = 0;
    bool anchor_alive = false;
    std::string detail;
};

// `anchor_pid`/`anchor_start_ticks` identify the sentinel created by
// spawn_service(). Zero means a legacy instance started before the anchor was
// introduced; such an instance is only signalled while its verified leader is
// still visible, because the leader is then the PGID reservation. Descendant-only
// legacy groups fail closed as uncertain.
GroupProbe probe_service_group(int pgid, int leader_pid, std::uint64_t leader_start_ticks,
                               std::string_view recorded_boot_id,
                               std::string_view current_boot_id = {},
                               int anchor_pid = 0,
                               std::uint64_t anchor_start_ticks = 0);

// Conservative membership test used only when identity is unknown: true when
// any live process currently has `pgid` as its process group. This never
// authorises a signal; it only makes recovery/start/delete fail closed.
bool process_group_has_any_members(int pgid);

long clock_ticks_per_second();
std::int64_t page_size_bytes();

struct SpawnOutcome {
    Error error;
    int pid = -1;
    int pgid = 0;
    std::uint64_t start_ticks = 0;
    int log_read_fd = -1;
    int exec_read_fd = -1;
    // Parent side of the exec handshake. The child blocks before execve until
    // the caller writes one byte after persisting the process identity
    // (SPEC-0010 PSH-04). Closing this descriptor without writing makes the
    // child exit 127 without executing the target program.
    int handshake_fd = -1;
    // True when the entry point is a shebang script executed through
    // /proc/self/fd/<script_fd>; used to produce actionable exec errors.
    bool script_entry = false;
    // Identity of the group sentinel. It keeps the PGID reserved for the life of
    // the instance and is reaped by the Supervisor (it is created with
    // CLONE_PARENT, so it is a direct child of the daemon).
    int anchor_pid = -1;
    std::uint64_t anchor_start_ticks = 0;
    // Staged executable symlink (and its private directory). The link is named
    // after the configured executable so AT_EXECFN keeps a correct basename
    // while the target stays pinned to the verified descriptor. The caller owns
    // cleanup after the process has exited.
    std::string exec_staging_link;
    std::string exec_staging_directory;
};

// Starts `config` with `identity`. stdout/stderr share one non-blocking pipe;
// the exec/identity error channel is CLOEXEC and reports failure before exec.
// The caller owns and must close both returned descriptors plus handshake_fd.
//
// The child waits on handshake_fd before executing the target program. After
// the caller has persisted PID/PGID/boot ID/start time it must write one byte
// ('1') and close the descriptor; otherwise it closes it and reaps the child.
// Before blocking on the handshake the child creates the group sentinel and
// reports its PID and start time through SpawnOutcome.
//
// When `exec_staging_directory` is non-empty, the executable is reached through
// a symlink in a private per-spawn directory. Executing an open descriptor
// directly (fexecve) makes AT_EXECFN the descriptor number, which breaks
// multicall binaries that dispatch on it; the staged link keeps both the
// basename and the pinned inode. The link must remain until the process exits.
SpawnOutcome spawn_service(const ServiceConfig& config, const ResolvedIdentity& identity,
                           const std::string& exec_staging_directory = {});

// Sends a signal to the process group only when the probe authorises it:
// verified groups accept any signal; anchor_only groups accept only SIGKILL
// (the sentinel reserves the PGID, so this cannot reach a recycled group).
// Never takes a client-supplied PID/PGID. Returns false otherwise; the caller
// decides whether the group disappeared or the identity became unverifiable.
bool signal_service_group(const GroupProbe& probe, int signal, std::string& error);

// Writes the one-byte exec handshake and closes the descriptor. Returns false
// when the child could not be released (for example because it already exited).
bool release_service_child(int& handshake_fd, std::string& error);

// Closes the handshake descriptor without releasing the child; the child sees
// EOF and exits 127 without executing the target program. The caller still has
// to waitpid() the child.
void abort_service_child(int& handshake_fd);

} // namespace edgedesk::supervisor
