#include "core/types.hpp"
#include "core/util.hpp"
#include "supervisor/ipc_server.hpp"
#include "supervisor/supervisor_engine.hpp"

#include <cerrno>
#include <csignal>
#include <cstdio>
#include <cstring>
#include <grp.h>
#include <iostream>
#include <limits>
#include <pwd.h>
#include <string>
#include <sys/stat.h>
#include <unistd.h>
#include <vector>

namespace {

volatile std::sig_atomic_t g_stop = 0;

void on_signal(int) { g_stop = 1; }

struct Arguments {
    std::string socket_path = "/run/edgedesk-supervisor.sock";
    std::string database_path = "/var/lib/edgedesk-supervisor/supervisor.db";
    std::string log_directory = "/var/lib/edgedesk-supervisor/logs";
    std::vector<uid_t> allowed_uids;
    std::vector<gid_t> allowed_gids;
    bool has_socket_gid = false;
    gid_t socket_gid = 0;
    bool show_help = false;
    bool show_version = false;
};

void usage() {
    std::cout
        << "Usage: edgedesk-supervisor [options]\n\n"
        << "Options:\n"
        << "  --socket PATH          Unix socket path (default /run/edgedesk-supervisor.sock)\n"
        << "  --database PATH        SQLite database (default /var/lib/edgedesk-supervisor/supervisor.db)\n"
        << "  --log-dir PATH         Per-service log directory\n"
        << "  --allow-uid USER|UID   Allow this peer UID to connect (repeatable)\n"
        << "  --allow-gid GROUP|GID  Allow this peer primary GID to connect (repeatable)\n"
        << "  --socket-group GROUP   Group owner for the socket (default unchanged)\n"
        << "  --version              Print version\n"
        << "  --help                 Show this help\n\n"
        << "The Supervisor runs in the foreground; use the provided systemd/OpenRC/BusyBox\n"
        << "init adapters. It never opens a TCP listener.\n";
}

bool parse_uid_text(const std::string& text, uid_t& out) {
    std::uint64_t numeric = 0;
    if (edgedesk::parse_uint64(text, numeric)) {
        if (numeric > std::numeric_limits<uid_t>::max()) return false;
        out = static_cast<uid_t>(numeric);
        return true;
    }
    struct passwd* entry = ::getpwnam(text.c_str());
    if (entry == nullptr) return false;
    out = entry->pw_uid;
    return true;
}

bool parse_gid_text(const std::string& text, gid_t& out) {
    std::uint64_t numeric = 0;
    if (edgedesk::parse_uint64(text, numeric)) {
        if (numeric > std::numeric_limits<gid_t>::max()) return false;
        out = static_cast<gid_t>(numeric);
        return true;
    }
    struct group* entry = ::getgrnam(text.c_str());
    if (entry == nullptr) return false;
    out = entry->gr_gid;
    return true;
}

bool take_value(int argc, char** argv, int& index, std::string& value) {
    if (index + 1 >= argc) return false;
    value = argv[++index];
    return true;
}

bool parse_arguments(int argc, char** argv, Arguments& out) {
    for (int index = 1; index < argc; ++index) {
        const std::string option = argv[index];
        if (option == "--help" || option == "-h") {
            out.show_help = true;
            continue;
        }
        if (option == "--version") {
            out.show_version = true;
            continue;
        }
        std::string value;
        if (option == "--socket") {
            if (!take_value(argc, argv, index, value)) return false;
            out.socket_path = value;
        } else if (option == "--database") {
            if (!take_value(argc, argv, index, value)) return false;
            out.database_path = value;
        } else if (option == "--log-dir") {
            if (!take_value(argc, argv, index, value)) return false;
            out.log_directory = value;
        } else if (option == "--allow-uid") {
            if (!take_value(argc, argv, index, value)) return false;
            uid_t uid = 0;
            if (!parse_uid_text(value, uid)) {
                std::fprintf(stderr, "edgedesk-supervisor: unknown user: %s\n", value.c_str());
                return false;
            }
            out.allowed_uids.push_back(uid);
        } else if (option == "--allow-gid") {
            if (!take_value(argc, argv, index, value)) return false;
            gid_t gid = 0;
            if (!parse_gid_text(value, gid)) {
                std::fprintf(stderr, "edgedesk-supervisor: unknown group: %s\n", value.c_str());
                return false;
            }
            out.allowed_gids.push_back(gid);
        } else if (option == "--socket-group") {
            if (!take_value(argc, argv, index, value)) return false;
            gid_t gid = 0;
            if (!parse_gid_text(value, gid)) {
                std::fprintf(stderr, "edgedesk-supervisor: unknown group: %s\n", value.c_str());
                return false;
            }
            out.has_socket_gid = true;
            out.socket_gid = gid;
        } else {
            std::fprintf(stderr, "edgedesk-supervisor: unknown option: %s\n", option.c_str());
            return false;
        }
    }
    return true;
}

} // namespace

int main(int argc, char** argv) {
    Arguments arguments;
    if (!parse_arguments(argc, argv, arguments)) {
        usage();
        return 2;
    }
    if (arguments.show_help) {
        usage();
        return 0;
    }
    if (arguments.show_version) {
        std::cout << "edgedesk-supervisor " EDGEDESK_VERSION << std::endl;
        return 0;
    }

    (void)::umask(0077);
    // A client may close between our read and write; sockets must never kill
    // the daemon with SIGPIPE.
    (void)::signal(SIGPIPE, SIG_IGN);
    struct sigaction action {};
    action.sa_handler = on_signal;
    ::sigemptyset(&action.sa_mask);
    action.sa_flags = 0;
    (void)::sigaction(SIGTERM, &action, nullptr);
    (void)::sigaction(SIGINT, &action, nullptr);
    (void)::sigaction(SIGHUP, &action, nullptr);

    if (::geteuid() != 0) {
        std::fprintf(stderr,
                     "edgedesk-supervisor: warning: running without root; services may only use "
                     "uid=%u gid=%u\n",
                     static_cast<unsigned>(::geteuid()),
                     static_cast<unsigned>(::getegid()));
        if (arguments.allowed_uids.empty() && arguments.allowed_gids.empty()) {
            arguments.allowed_uids.push_back(::geteuid());
        }
    } else if (arguments.allowed_uids.empty() && arguments.allowed_gids.empty()) {
        // The Supervisor itself is root-owned. Allow the EdgeDesk backend to be
        // authorized explicitly with --allow-uid/--allow-gid when it does not
        // run as root.
        arguments.allowed_uids.push_back(0);
    }

    edgedesk::supervisor::EngineOptions engine_options;
    engine_options.database_path = arguments.database_path;
    engine_options.log_directory = arguments.log_directory;

    edgedesk::Error error;
    std::unique_ptr<edgedesk::supervisor::SupervisorEngine> engine =
        edgedesk::supervisor::SupervisorEngine::create(engine_options, error);
    if (!engine) {
        std::fprintf(stderr, "edgedesk-supervisor: cannot start: %s\n", error.message.c_str());
        return 1;
    }

    edgedesk::supervisor::IpcServerOptions ipc_options;
    ipc_options.socket_path = arguments.socket_path;
    ipc_options.allowed_uids = arguments.allowed_uids;
    ipc_options.allowed_gids = arguments.allowed_gids;
    ipc_options.socket_gid =
        arguments.has_socket_gid ? std::optional<gid_t>(arguments.socket_gid) : std::nullopt;
    std::unique_ptr<edgedesk::supervisor::IpcServer> server =
        edgedesk::supervisor::IpcServer::create(ipc_options, *engine, error);
    if (!server) {
        std::fprintf(stderr, "edgedesk-supervisor: cannot open IPC socket: %s\n",
                     error.message.c_str());
        engine->shutdown(edgedesk::steady_millis(), 3000);
        return 1;
    }

    std::cout << "edgedesk-supervisor " EDGEDESK_VERSION << " listening on "
              << arguments.socket_path << std::endl;

    error = server->run([] { return g_stop != 0; });
    if (!error.ok()) {
        std::fprintf(stderr, "edgedesk-supervisor: event loop failed: %s\n",
                     error.message.c_str());
    }
    std::cout << "edgedesk-supervisor: shutting down" << std::endl;
    engine->shutdown(edgedesk::steady_millis());
    return error.ok() ? 0 : 1;
}
