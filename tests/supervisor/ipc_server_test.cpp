#include "test_util.hpp"

#include "core/util.hpp"
#include "supervisor/ipc_server.hpp"
#include "supervisor/supervisor_engine.hpp"

#include <cerrno>
#include <chrono>
#include <csignal>
#include <cstring>
#include <filesystem>
#include <grp.h>
#include <nlohmann/json.hpp>
#include <poll.h>
#include <pwd.h>
#include <string>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/un.h>
#include <sys/wait.h>
#include <thread>
#include <unistd.h>
#include <vector>

using namespace edgedesk;
using namespace edgedesk::supervisor;
using nlohmann::json;
namespace fs = std::filesystem;

namespace {

constexpr int kTimeoutMs = 8000;

fs::path make_root(const char* name) {
    const fs::path root = fs::temp_directory_path() /
                          (std::string(name) + "-" + std::to_string(::getpid()));
    std::error_code ignored;
    fs::remove_all(root, ignored);
    fs::create_directories(root);
    ::chmod(root.c_str(), 0700);
    return root;
}

std::string current_user() {
    struct passwd* entry = ::getpwuid(::geteuid());
    if (entry != nullptr && entry->pw_name != nullptr) return entry->pw_name;
    return std::to_string(::geteuid());
}

std::string current_group() {
    struct group* entry = ::getgrgid(::getegid());
    if (entry != nullptr && entry->gr_name != nullptr) return entry->gr_name;
    return std::to_string(::getegid());
}

volatile std::sig_atomic_t g_child_stop = 0;
void child_signal(int) { g_child_stop = 1; }

[[noreturn]] void run_child(const std::string& socket_path, const std::string& database,
                            const std::string& logs, const std::vector<uid_t>& allowed_uids) {
    (void)::signal(SIGPIPE, SIG_IGN);
    struct sigaction action {};
    action.sa_handler = child_signal;
    ::sigemptyset(&action.sa_mask);
    (void)::sigaction(SIGTERM, &action, nullptr);
    (void)::sigaction(SIGINT, &action, nullptr);

    EngineOptions engine_options;
    engine_options.database_path = database;
    engine_options.log_directory = logs;
    engine_options.boot_id = "ipc-test-boot";
    Error error;
    auto engine = SupervisorEngine::create(engine_options, error);
    if (!engine) {
        std::fprintf(stderr, "child engine: %s\n", error.message.c_str());
        ::_exit(2);
    }
    IpcServerOptions ipc_options;
    ipc_options.socket_path = socket_path;
    ipc_options.allowed_uids = allowed_uids;
    auto server = IpcServer::create(ipc_options, *engine, error);
    if (!server) {
        std::fprintf(stderr, "child ipc: %s\n", error.message.c_str());
        engine->shutdown(steady_millis(), 1000);
        ::_exit(3);
    }
    error = server->run([] { return g_child_stop != 0; });
    if (!error.ok()) {
        std::fprintf(stderr, "child run: %s\n", error.message.c_str());
    }
    engine->shutdown(steady_millis(), 1000);
    ::_exit(error.ok() ? 0 : 4);
}

struct Child {
    pid_t pid = -1;

    ~Child() { stop(); }

    void stop() {
        if (pid <= 0) return;
        (void)::kill(pid, SIGTERM);
        const auto deadline = std::chrono::steady_clock::now() +
                              std::chrono::milliseconds(3000);
        int status = 0;
        while (std::chrono::steady_clock::now() < deadline) {
            const pid_t result = ::waitpid(pid, &status, WNOHANG);
            if (result == pid) {
                pid = -1;
                return;
            }
            std::this_thread::sleep_for(std::chrono::milliseconds(10));
        }
        (void)::kill(pid, SIGKILL);
        (void)::waitpid(pid, &status, 0);
        pid = -1;
    }
};

bool wait_for_socket(const std::string& path, pid_t& pid, int timeout_ms) {
    const auto deadline = std::chrono::steady_clock::now() +
                          std::chrono::milliseconds(timeout_ms);
    while (std::chrono::steady_clock::now() < deadline) {
        if (::access(path.c_str(), F_OK) == 0) return true;
        int status = 0;
        if (::waitpid(pid, &status, WNOHANG) == pid) {
            pid = -1;
            return false;
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(5));
    }
    return false;
}

int connect_socket(const std::string& path) {
    const int fd = ::socket(AF_UNIX, SOCK_STREAM | SOCK_CLOEXEC, 0);
    if (fd < 0) return -1;
    struct sockaddr_un address {};
    address.sun_family = AF_UNIX;
    if (path.size() >= sizeof(address.sun_path)) {
        ::close(fd);
        return -1;
    }
    std::memcpy(address.sun_path, path.c_str(), path.size() + 1);
    if (::connect(fd, reinterpret_cast<struct sockaddr*>(&address), sizeof(address)) != 0) {
        ::close(fd);
        return -1;
    }
    return fd;
}

// The socket file appears at bind() time, shortly before listen(). Fast
// static-musl test binaries can observe the file while connect() still gets
// ECONNREFUSED, so retry until the listener is actually ready.
int connect_socket_retry(const std::string& path, int timeout_ms = 3000) {
    const auto deadline = std::chrono::steady_clock::now() +
                          std::chrono::milliseconds(timeout_ms);
    for (;;) {
        const int fd = connect_socket(path);
        if (fd >= 0) return fd;
        if (std::chrono::steady_clock::now() >= deadline) return -1;
        std::this_thread::sleep_for(std::chrono::milliseconds(2));
    }
}

bool send_line(int fd, const std::string& line) {
    std::string data = line;
    data.push_back('\n');
    std::size_t sent = 0;
    while (sent < data.size()) {
        const ssize_t count = ::send(fd, data.data() + sent, data.size() - sent, MSG_NOSIGNAL);
        if (count > 0) {
            sent += static_cast<std::size_t>(count);
        } else if (count < 0 && errno == EINTR) {
            continue;
        } else {
            return false;
        }
    }
    return true;
}

// Returns one NDJSON line. An empty string means EOF or timeout.
std::string read_line(int fd, int timeout_ms = kTimeoutMs) {
    std::string buffer;
    const auto deadline = std::chrono::steady_clock::now() +
                          std::chrono::milliseconds(timeout_ms);
    while (std::chrono::steady_clock::now() < deadline) {
        const auto newline = buffer.find('\n');
        if (newline != std::string::npos) return buffer.substr(0, newline);
        struct pollfd item {};
        item.fd = fd;
        item.events = POLLIN;
        const int ready = ::poll(&item, 1, 100);
        if (ready < 0 && errno == EINTR) continue;
        if (ready <= 0) continue;
        char chunk[4096];
        const ssize_t count = ::read(fd, chunk, sizeof(chunk));
        if (count > 0) {
            buffer.append(chunk, static_cast<std::size_t>(count));
        } else if (count == 0) {
            return {};
        } else if (errno != EINTR && errno != EAGAIN) {
            return {};
        }
    }
    return {};
}

json rpc_call(int fd) {
    const std::string line = read_line(fd);
    if (line.empty()) return json::object();
    try {
        return json::parse(line);
    } catch (const std::exception&) {
        return json::object();
    }
}

json make_request(const std::string& id, const std::string& method,
                  const json& params = json::object()) {
    json request;
    request["jsonrpc"] = "2.0";
    request["id"] = id;
    request["method"] = method;
    request["params"] = params;
    return request;
}

void test_option_validation() {
    fs::path root = make_root("edgedesk-ipc-validate-test");
    const std::string database = (root / "state" / "supervisor.db").string();
    EngineOptions engine_options;
    engine_options.database_path = database;
    engine_options.log_directory = (root / "logs").string();
    engine_options.boot_id = "validate-boot";
    Error error;
    auto engine = SupervisorEngine::create(engine_options, error);
    CHECK(engine != nullptr);
    if (!engine) return;

    IpcServerOptions options;
    options.socket_path = "relative.sock";
    options.allowed_uids.push_back(::geteuid());
    CHECK(IpcServer::create(options, *engine, error) == nullptr);

    options.socket_path = (root / "sock").string();
    options.socket_mode = 0666;
    CHECK(IpcServer::create(options, *engine, error) == nullptr);

    options.socket_mode = 0660;
    options.allowed_uids.clear();
    CHECK(IpcServer::create(options, *engine, error) == nullptr);

    options.allowed_uids.push_back(::geteuid());
    options.socket_path = "/" + std::string(200, 'x') + ".sock";
    CHECK(IpcServer::create(options, *engine, error) == nullptr);

    std::error_code ignored;
    fs::remove_all(root, ignored);
}

void test_authorized_client_and_async_stop() {
    const fs::path root = make_root("edgedesk-ipc-client-test");
    const std::string socket = (root / "supervisor.sock").string();
    const std::string database = (root / "state" / "supervisor.db").string();
    const std::string logs = (root / "logs").string();

    Child child;
    child.pid = ::fork();
    CHECK(child.pid >= 0);
    if (child.pid == 0) {
        run_child(socket, database, logs, {::geteuid()});
    }
    CHECK(wait_for_socket(socket, child.pid, 3000));
    if (child.pid <= 0) return;

    struct stat info {};
    CHECK(::stat(socket.c_str(), &info) == 0);
    CHECK_EQ(static_cast<int>(info.st_mode & 0777), 0660);

    const int fd = connect_socket_retry(socket);
    CHECK(fd >= 0);
    if (fd < 0) return;

    CHECK(send_line(fd, make_request("1", "health").dump()));
    json response = rpc_call(fd);
    CHECK_EQ(response.value("jsonrpc", std::string()), "2.0");
    CHECK_EQ(response["id"].get<std::string>(), "1");
    CHECK_EQ(response["result"].value("status", std::string()), "ok");
    CHECK_EQ(response["result"].value("service", std::string()), "edgedesk-supervisor");

    // Malformed JSON is a parse error and must not drop the connection.
    CHECK(send_line(fd, "{not json"));
    response = rpc_call(fd);
    CHECK_EQ(response["error"].value("code", 0), -32700);

    // Unknown methods are refused without a PID/PGID surface.
    CHECK(send_line(fd, make_request("2", "service.kill").dump()));
    response = rpc_call(fd);
    CHECK_EQ(response["error"].value("code", 0), -32601);

    CHECK(send_line(fd, make_request("3", "service.list").dump()));
    response = rpc_call(fd);
    CHECK(response["result"].contains("services"));
    CHECK_EQ(response["result"].value("total", -1), 0);

    json config;
    config["name"] = "ipc-sleeper";
    config["executable"] = "/bin/sleep";
    config["args"] = json::array({"60"});
    config["cwd"] = "/";
    config["run_user"] = current_user();
    config["run_group"] = current_group();
    config["allow_root"] = true;
    config["stop_timeout_ms"] = 200;
    CHECK(send_line(fd, make_request("4", "service.create", config).dump()));
    response = rpc_call(fd);
    CHECK(response.contains("result"));
    const std::string id = response["result"].value("id", std::string());
    CHECK(!id.empty());
    if (id.empty()) return;

    json id_params;
    id_params["id"] = id;
    CHECK(send_line(fd, make_request("5", "service.start", id_params).dump()));
    response = rpc_call(fd);
    CHECK(response.contains("result"));
    CHECK(response["result"]["state"].get<std::string>() == "STARTING" ||
          response["result"]["state"].get<std::string>() == "RUNNING");

    // Let the child event loop drain exec confirmation before stopping.
    std::this_thread::sleep_for(std::chrono::milliseconds(100));

    CHECK(send_line(fd, make_request("6", "service.stop", id_params).dump()));
    response = rpc_call(fd);
    CHECK(response.contains("result"));
    CHECK_EQ(response["result"]["state"].get<std::string>(), "STOPPED");

    CHECK(send_line(fd, make_request("7", "service.delete", id_params).dump()));
    response = rpc_call(fd);
    CHECK(response["result"].value("deleted", false));

    ::close(fd);
    child.stop();
    std::error_code ignored;
    fs::remove_all(root, ignored);
}

void test_unauthorized_peer_rejected() {
    const fs::path root = make_root("edgedesk-ipc-unauthorized-test");
    const std::string socket = (root / "supervisor.sock").string();
    const std::string database = (root / "state" / "supervisor.db").string();
    const std::string logs = (root / "logs").string();

    uid_t other = ::geteuid() == 0 ? static_cast<uid_t>(1) : static_cast<uid_t>(0);
    Child child;
    child.pid = ::fork();
    CHECK(child.pid >= 0);
    if (child.pid == 0) {
        run_child(socket, database, logs, {other});
    }
    CHECK(wait_for_socket(socket, child.pid, 3000));
    if (child.pid <= 0) return;

    const int fd = connect_socket_retry(socket);
    CHECK(fd >= 0);
    if (fd >= 0) {
        // The server closes unauthenticated peers immediately, so the send may
        // race with that close (EPIPE). Either outcome must yield no response.
        (void)send_line(fd, make_request("1", "health").dump());
        CHECK(read_line(fd).empty());
        ::close(fd);
    }

    child.stop();
    std::error_code ignored;
    fs::remove_all(root, ignored);
}

} // namespace

int main() {
    test_option_validation();
    test_authorized_client_and_async_stop();
    test_unauthorized_peer_rejected();
    return edgedesk::test::finish("supervisor_ipc_test");
}
