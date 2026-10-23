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
// A group is only VERIFIED when every live member is in the service's own
// session (setsid makes SID == PGID == original leader PID), no member predates
// the recorded leader start time, and a still-visible leader PID carries the
// recorded start time. If the leader PID is visible with a different start time
// the PID (and therefore the PGID) was reused and the group is UNCERTAIN.
//
// Linux cannot make "check group membership" and kill(-pgid) atomic without
// pidfd support for process groups. Callers must treat UNCERTAIN as "refuse to
// signal" and surface it as UNKNOWN/manual intervention; ABSENT means the
// group has no live member and must not be signalled either.
enum class GroupState { absent, verified, uncertain };

struct GroupProbe {
    GroupState state = GroupState::absent;
    int pgid = 0;
    int leader_pid = 0;
    int live_members = 0;
    std::string detail;
};

GroupProbe probe_service_group(int pgid, int leader_pid, std::uint64_t leader_start_ticks,
                               std::string_view recorded_boot_id,
                               std::string_view current_boot_id = {});

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
};

// Starts `config` with `identity`. stdout/stderr share one non-blocking pipe;
// the exec/identity error channel is CLOEXEC and reports failure before exec.
// The caller owns and must close both returned descriptors plus handshake_fd.
//
// The child waits on handshake_fd before executing the target program. After
// the caller has persisted PID/PGID/boot ID/start time it must write one byte
// ('1') and close the descriptor; otherwise it closes it and reaps the child.
SpawnOutcome spawn_service(const ServiceConfig& config, const ResolvedIdentity& identity);

// Sends a signal to the process group only when `probe.state == verified`.
// Never takes a client-supplied PID/PGID. Returns false for ABSENT, UNCERTAIN
// and kill() failures; the caller decides whether the group disappeared or the
// identity became unverifiable.
bool signal_service_group(const GroupProbe& probe, int signal, std::string& error);

// Writes the one-byte exec handshake and closes the descriptor. Returns false
// when the child could not be released (for example because it already exited).
bool release_service_child(int& handshake_fd, std::string& error);

// Closes the handshake descriptor without releasing the child; the child sees
// EOF and exits 127 without executing the target program. The caller still has
// to waitpid() the child.
void abort_service_child(int& handshake_fd);

} // namespace edgedesk::supervisor
