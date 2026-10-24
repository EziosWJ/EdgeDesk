#include "test_util.hpp"

#include "core/util.hpp"
#include "extensions/backend_proxy.hpp"
#include "extensions/extension_store.hpp"
#include "extensions/native_backend.hpp"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstddef>
#include <cstring>
#include <filesystem>
#include <map>
#include <memory>
#include <mutex>
#include <poll.h>
#include <string>
#include <sys/socket.h>
#include <sys/un.h>
#include <thread>
#include <utility>
#include <vector>
#include <unistd.h>

// Deterministic end-to-end coverage for the HTTP-over-UDS proxy: real Unix
// domain socket servers, bounded deadlines, validation-before-connect and
// hop-by-hop header isolation.

namespace {

namespace fs = std::filesystem;
using namespace edgedesk;
using namespace edgedesk::extensions;
using Clock = std::chrono::steady_clock;

std::string lower_ascii(std::string value) {
    for (char& c : value) {
        if (c >= 'A' && c <= 'Z') c = static_cast<char>(c - 'A' + 'a');
    }
    return value;
}

struct RecordedRequest {
    std::string method;
    std::string target;
    std::string body;
    std::map<std::string, std::string> headers; // lowercase names
};

class TestBackend {
public:
    explicit TestBackend(std::string path) : path_(std::move(path)) {
        ::unlink(path_.c_str());
        listen_fd_ = ::socket(AF_UNIX, SOCK_STREAM | SOCK_CLOEXEC, 0);
        CHECK(listen_fd_ >= 0);
        if (listen_fd_ < 0) return;
        sockaddr_un address{};
        address.sun_family = AF_UNIX;
        CHECK(path_.size() < sizeof(address.sun_path));
        if (path_.size() >= sizeof(address.sun_path)) return;
        std::memcpy(address.sun_path, path_.data(), path_.size());
        CHECK(::bind(listen_fd_, reinterpret_cast<sockaddr*>(&address), sizeof(address)) == 0);
        CHECK(::listen(listen_fd_, 8) == 0);
        thread_ = std::thread([this] { serve(); });
    }

    ~TestBackend() { stop(); }

    TestBackend(const TestBackend&) = delete;
    TestBackend& operator=(const TestBackend&) = delete;

    void stop() {
        if (stopping_.exchange(true)) return;
        if (listen_fd_ >= 0) ::shutdown(listen_fd_, SHUT_RDWR);
        if (thread_.joinable()) thread_.join();
        if (listen_fd_ >= 0) {
            ::close(listen_fd_);
            listen_fd_ = -1;
        }
        ::unlink(path_.c_str());
    }

    std::vector<RecordedRequest> requests() const {
        std::lock_guard<std::mutex> lock(mutex_);
        return requests_;
    }

private:
    void serve() {
        while (!stopping_.load()) {
            pollfd entry{};
            entry.fd = listen_fd_;
            entry.events = POLLIN;
            const int ready = ::poll(&entry, 1, 20);
            if (ready <= 0) continue;
            const int client = ::accept4(listen_fd_, nullptr, nullptr, SOCK_CLOEXEC);
            if (client < 0) {
                if (errno == EINTR) continue;
                if (stopping_.load()) return;
                continue;
            }
            handle(client);
            ::close(client);
        }
    }

    static std::string header_block_value(const std::string& head, const std::string& name) {
        const std::string lower = lower_ascii(head);
        const std::string needle = "\r\n" + name + ":";
        const std::size_t pos = lower.find(needle);
        if (pos == std::string::npos) return {};
        std::size_t start = pos + needle.size();
        const std::size_t end = head.find("\r\n", start);
        while (start < end && (head[start] == ' ' || head[start] == '\t')) ++start;
        std::size_t finish = end;
        while (finish > start && (head[finish - 1] == ' ' || head[finish - 1] == '\t')) --finish;
        return head.substr(start, finish - start);
    }

    void handle(int client) {
        std::string raw;
        char buffer[4096];
        std::size_t head_end = std::string::npos;
        while (head_end == std::string::npos) {
            head_end = raw.find("\r\n\r\n");
            if (head_end != std::string::npos) break;
            const ssize_t received = ::recv(client, buffer, sizeof(buffer), 0);
            if (received <= 0) return;
            raw.append(buffer, static_cast<std::size_t>(received));
            if (raw.size() > 128 * 1024) return;
        }
        const std::string head = raw.substr(0, head_end + 4);
        std::size_t body_length = 0;
        const std::string length_text = header_block_value(head, "content-length");
        if (!length_text.empty()) {
            body_length = static_cast<std::size_t>(std::stoull(length_text));
        }
        while (raw.size() < head_end + 4 + body_length) {
            const ssize_t received = ::recv(client, buffer, sizeof(buffer), 0);
            if (received <= 0) return;
            raw.append(buffer, static_cast<std::size_t>(received));
            if (raw.size() > 128 * 1024) return;
        }

        RecordedRequest request;
        const std::size_t line_end = head.find("\r\n");
        const std::string request_line = head.substr(0, line_end);
        {
            const std::size_t first = request_line.find(' ');
            const std::size_t second =
                first == std::string::npos ? std::string::npos : request_line.find(' ', first + 1);
            if (first != std::string::npos && second != std::string::npos) {
                request.method = request_line.substr(0, first);
                request.target = request_line.substr(first + 1, second - first - 1);
            }
        }
        std::size_t position = line_end == std::string::npos ? head.size() : line_end + 2;
        while (position < head_end) {
            const std::size_t end = head.find("\r\n", position);
            if (end == std::string::npos || end > head_end) break;
            const std::string line = head.substr(position, end - position);
            const std::size_t colon = line.find(':');
            if (colon != std::string::npos) {
                std::string value = line.substr(colon + 1);
                while (!value.empty() && (value.front() == ' ' || value.front() == '\t')) {
                    value.erase(value.begin());
                }
                while (!value.empty() && (value.back() == ' ' || value.back() == '\t')) {
                    value.pop_back();
                }
                request.headers[lower_ascii(line.substr(0, colon))] = value;
            }
            position = end + 2;
        }
        request.body = raw.substr(head_end + 4, body_length);
        {
            std::lock_guard<std::mutex> lock(mutex_);
            requests_.push_back(request);
        }
        write_all(client, respond(request));
    }

    static std::string respond(const RecordedRequest& request) {
        if (request.target == "/slow") {
            // Bounded sleep so shutdown stays responsive.
            for (int tick = 0; tick < 12; ++tick) {
                std::this_thread::sleep_for(std::chrono::milliseconds(50));
            }
            return response_with_length(200, "OK", R"({"slow":true})", {});
        }
        if (request.target == "/missing") {
            return response_with_length(404, "Not Found", R"({"error":"nope"})", {});
        }
        if (request.target == "/error") {
            return response_with_length(500, "Internal Server Error", R"({"error":"boom"})", {});
        }
        if (request.target == "/badjson") {
            return response_with_length(200, "OK", "not json", {});
        }
        if (request.target == "/chunked") {
            const std::string payload = R"({"chunk":true})";
            const std::string first = payload.substr(0, 5);
            const std::string second = payload.substr(5);
            std::string response =
                "HTTP/1.1 200 OK\r\nContent-Type: application/json\r\n"
                "Transfer-Encoding: chunked\r\n\r\n";
            response += hex_size(first.size()) + "\r\n" + first + "\r\n";
            response += hex_size(second.size()) + "\r\n" + second + "\r\n";
            response += "0\r\n\r\n";
            return response;
        }
        if (request.target == "/close") {
            return "HTTP/1.1 200 OK\r\nContent-Type: application/json\r\n\r\n" +
                   std::string(R"({"close":true})");
        }
        return response_with_length(
            200, "OK", R"({"ok":true,"n":1})",
            {{"X-Trace", "abc-123"},
             {"X-Custom", "kept"},
             {"Set-Cookie", "session=secret"},
             {"Server", "test-backend"}});
    }

    static std::string hex_size(std::size_t value) {
        static const char* digits = "0123456789abcdef";
        std::string out;
        do {
            out.insert(out.begin(), digits[value % 16]);
            value /= 16;
        } while (value > 0);
        return out;
    }

    static std::string response_with_length(
        int status, const std::string& reason, const std::string& body,
        const std::vector<std::pair<std::string, std::string>>& extra_headers) {
        std::string response = "HTTP/1.1 " + std::to_string(status) + " " + reason + "\r\n";
        response += "Content-Type: application/json\r\n";
        for (const auto& [name, value] : extra_headers) {
            response += name + ": " + value + "\r\n";
        }
        response += "Content-Length: " + std::to_string(body.size()) + "\r\n\r\n";
        response += body;
        return response;
    }

    static void write_all(int fd, const std::string& data) {
        std::size_t sent = 0;
        while (sent < data.size()) {
            const ssize_t written = ::send(fd, data.data() + sent, data.size() - sent,
                                           MSG_NOSIGNAL);
            if (written > 0) {
                sent += static_cast<std::size_t>(written);
                continue;
            }
            if (written < 0 && errno == EINTR) continue;
            return;
        }
    }

    std::string path_;
    int listen_fd_ = -1;
    std::atomic<bool> stopping_{false};
    std::thread thread_;
    mutable std::mutex mutex_;
    std::vector<RecordedRequest> requests_;
};

struct Fixture {
    fs::path root;
    std::unique_ptr<ExtensionStore> store;

    explicit Fixture(const std::string& name) {
        root = fs::temp_directory_path() /
               ("edgedesk-backend-proxy-" + name + "-" + std::to_string(::getpid()));
        fs::remove_all(root);
        fs::create_directories(root);
        Error error;
        store = ExtensionStore::open((root / "store").string(), error);
        CHECK(store != nullptr);
        if (!store) std::fprintf(stderr, "store error: %s\n", error.message.c_str());
    }

    ~Fixture() {
        store.reset();
        std::error_code ignored;
        fs::remove_all(root, ignored);
    }

    std::string apps_root() const { return (root / "apps").string(); }

    void add_app(const std::string& app_id, bool with_backend) {
        AppRecord app;
        app.app_id = app_id;
        app.display_name = app_id;
        app.status = app_status::enabled;
        app.created_at = 1;
        CHECK(store->upsert_app(app).ok());
        if (!with_backend) return;
        BackendRecord backend;
        backend.app_id = app_id;
        backend.service_id = backend_service_id(app_id);
        backend.executable_rel = "backend";
        backend.arch = "x86_64";
        backend.state = backend_state::stopped;
        CHECK(store->upsert_backend(backend).ok());
    }

    std::string socket_path(const std::string& app_id) const {
        return backend_socket_path(apps_root(), app_id);
    }

    std::string prepare_socket_dir(const std::string& app_id) const {
        const std::string path = socket_path(app_id);
        fs::create_directories(fs::path(path).parent_path());
        return path;
    }
};

RuntimeSession session_for(const std::string& app_id) {
    RuntimeSession session;
    session.token = "runtime-token";
    session.app_id = app_id;
    session.instance_id = "instance-1";
    return session;
}

struct CallResult {
    bool done = false;
    Error error;
    BackendHttpResponse response;
};

constexpr std::size_t kMaxTestPath = 2048;

CallResult run_request(BackendProxy& proxy, const RuntimeSession& session,
                       nlohmann::json params) {
    CallResult result;
    std::mutex mutex;
    std::condition_variable finished;
    const bool queued = proxy.submit(
        session, std::move(params), [&](Error error, BackendHttpResponse response) {
            std::lock_guard<std::mutex> lock(mutex);
            result.error = std::move(error);
            result.response = std::move(response);
            result.done = true;
            finished.notify_one();
        });
    if (!queued) {
        result.done = true;
        result.error = Error::make(ErrorCode::busy, "queue full");
        return result;
    }
    std::unique_lock<std::mutex> lock(mutex);
    finished.wait_for(lock, std::chrono::seconds(10), [&] { return result.done; });
    CHECK(result.done);
    return result;
}

nlohmann::json get_params(const std::string& path) {
    return {{"method", "GET"}, {"path", path}};
}

bool starts_with(const std::string& text, const std::string& prefix) {
    return text.rfind(prefix, 0) == 0;
}

void expect_invalid(BackendProxy& proxy, const RuntimeSession& session, nlohmann::json params,
                    const char* label) {
    CallResult result = run_request(proxy, session, std::move(params));
    if (result.error.code != ErrorCode::invalid_argument) {
        std::fprintf(stderr, "expected invalid_argument for %s, got %s (%s)\n", label,
                     error_code_name(result.error.code), result.error.message.c_str());
    }
    CHECK(result.error.code == ErrorCode::invalid_argument);
}

void test_get_and_header_filter() {
    Fixture fixture("get");
    fixture.add_app("app.get", true);
    TestBackend backend(fixture.prepare_socket_dir("app.get"));
    BackendProxy proxy(*fixture.store, fixture.apps_root());
    CHECK_EQ(proxy.request_timeout_ms(), 10000);

    const CallResult result =
        run_request(proxy, session_for("app.get"), get_params("/"));
    CHECK(result.error.ok());
    CHECK_EQ(result.response.status, 200);
    CHECK(result.response.body.is_object());
    if (result.response.body.is_object()) {
        CHECK_EQ(result.response.body.value("ok", false), true);
        CHECK_EQ(result.response.body.value("n", 0), 1);
    }
    CHECK_STR_EQ(result.response.raw_body, R"({"ok":true,"n":1})");
    CHECK(result.response.headers.contains("content-type"));
    CHECK_STR_EQ(result.response.headers["content-type"].get<std::string>(),
                 "application/json");
    CHECK(result.response.headers.contains("x-trace"));
    CHECK_STR_EQ(result.response.headers["x-trace"].get<std::string>(), "abc-123");
    CHECK(result.response.headers.contains("x-custom"));
    CHECK(!result.response.headers.contains("set-cookie"));
    CHECK(!result.response.headers.contains("server"));
    CHECK(!result.response.headers.contains("connection"));
    CHECK(!result.response.headers.contains("content-length"));
    CHECK_EQ(proxy.pending(), std::size_t{0});

    const std::vector<RecordedRequest> requests = backend.requests();
    CHECK_EQ(requests.size(), std::size_t{1});
    if (!requests.empty()) {
        CHECK_STR_EQ(requests[0].method, "GET");
        CHECK_STR_EQ(requests[0].target, "/");
        CHECK_EQ(requests[0].headers.count("host"), std::size_t{1});
        CHECK_STR_EQ(requests[0].headers.count("host") ? requests[0].headers.at("host") : "",
                     "localhost");
        CHECK_STR_EQ(requests[0].headers.count("connection")
                         ? requests[0].headers.at("connection")
                         : "",
                     "close");
        CHECK_EQ(requests[0].headers.count("upgrade"), std::size_t{0});
        CHECK_EQ(requests[0].headers.count("transfer-encoding"), std::size_t{0});
    }
}

void test_post_body_and_hop_headers() {
    Fixture fixture("post");
    fixture.add_app("app.post", true);
    TestBackend backend(fixture.prepare_socket_dir("app.post"));
    BackendProxy proxy(*fixture.store, fixture.apps_root());

    const nlohmann::json payload = {{"name", "值"}, {"n", 2}};
    const CallResult result = run_request(
        proxy, session_for("app.post"),
        {{"method", "POST"}, {"path", "/"}, {"body", payload},
         {"contentType", "application/json"}});
    CHECK(result.error.ok());
    CHECK_EQ(result.response.status, 200);

    const std::vector<RecordedRequest> requests = backend.requests();
    CHECK_EQ(requests.size(), std::size_t{1});
    if (!requests.empty()) {
        const RecordedRequest& request = requests[0];
        CHECK_STR_EQ(request.method, "POST");
        CHECK_STR_EQ(request.body, payload.dump());
        CHECK_STR_EQ(request.headers.count("content-type")
                         ? request.headers.at("content-type")
                         : "",
                     "application/json");
        CHECK_STR_EQ(request.headers.count("content-length")
                         ? request.headers.at("content-length")
                         : "",
                     std::to_string(payload.dump().size()));
        CHECK_STR_EQ(request.headers.count("host") ? request.headers.at("host") : "",
                     "localhost");
        CHECK_STR_EQ(request.headers.count("connection")
                         ? request.headers.at("connection")
                         : "",
                     "close");
        CHECK_EQ(request.headers.count("upgrade"), std::size_t{0});
    }

    // Caller-supplied hop-by-hop headers (or any other key) are rejected rather
    // than silently forwarded.
    expect_invalid(proxy, session_for("app.post"),
                   {{"method", "GET"},
                    {"path", "/"},
                    {"Host", "evil.example"},
                    {"Connection", "Upgrade"},
                    {"Upgrade", "websocket"}},
                   "caller headers");
    CHECK_EQ(backend.requests().size(), std::size_t{1});
}

void test_statuses_are_data() {
    Fixture fixture("statuses");
    fixture.add_app("app.status", true);
    TestBackend backend(fixture.prepare_socket_dir("app.status"));
    BackendProxy proxy(*fixture.store, fixture.apps_root());

    const CallResult missing =
        run_request(proxy, session_for("app.status"), get_params("/missing"));
    CHECK(missing.error.ok());
    CHECK_EQ(missing.response.status, 404);
    CHECK(missing.response.body.is_object());
    if (missing.response.body.is_object()) {
        CHECK_STR_EQ(missing.response.body.value("error", ""), "nope");
    }

    const CallResult failure =
        run_request(proxy, session_for("app.status"), get_params("/error"));
    CHECK(failure.error.ok());
    CHECK_EQ(failure.response.status, 500);
    CHECK(failure.response.body.is_object());
    if (failure.response.body.is_object()) {
        CHECK_STR_EQ(failure.response.body.value("error", ""), "boom");
    }
}

void test_validation_before_connect() {
    Fixture fixture("validation");
    fixture.add_app("app.invalid", true);
    TestBackend backend(fixture.prepare_socket_dir("app.invalid"));
    BackendProxy proxy(*fixture.store, fixture.apps_root());
    const RuntimeSession session = session_for("app.invalid");

    expect_invalid(proxy, session, {{"method", "get"}, {"path", "/"}}, "lowercase method");
    expect_invalid(proxy, session, {{"method", "CONNECT"}, {"path", "/"}}, "CONNECT");
    expect_invalid(proxy, session, {{"method", "TRACE"}, {"path", "/"}}, "TRACE");
    expect_invalid(proxy, session, {{"path", "/"}}, "missing method");
    expect_invalid(proxy, session, {{"method", 7}, {"path", "/"}}, "non-string method");

    expect_invalid(proxy, session, get_params("http://evil.example/x"), "absolute URL");
    expect_invalid(proxy, session, get_params("//evil.example/x"), "protocol-relative path");
    expect_invalid(proxy, session, get_params(""), "empty path");
    expect_invalid(proxy, session, get_params("relative"), "relative path");
    expect_invalid(proxy, session, get_params("/a/../b"), "dot-dot traversal");
    expect_invalid(proxy, session, get_params("/a/./b"), "dot traversal");
    expect_invalid(proxy, session, get_params("/a/.."), "trailing dot-dot");
    expect_invalid(proxy, session, get_params("/%2e%2e/etc"), "encoded dot-dot traversal");
    expect_invalid(proxy, session, get_params("/a/%2E%2e"), "mixed encoded dot-dot");
    expect_invalid(proxy, session, get_params("/a%2fb"), "encoded path separator");
    expect_invalid(proxy, session, get_params("/a%00b"), "encoded NUL");
    expect_invalid(proxy, session, get_params("/x#frag"), "fragment");
    expect_invalid(proxy, session, get_params("/x?y=1#frag"), "query with fragment");
    expect_invalid(proxy, session, get_params("/back\\slash"), "backslash");
    expect_invalid(proxy, session, get_params("/bad\x01path"), "control character");
    expect_invalid(proxy, session, get_params("/" + std::string(kMaxTestPath, 'a')),
                   "oversized path");

    expect_invalid(proxy, session,
                   {{"method", "POST"}, {"path", "/"},
                    {"body", std::string(1024 * 1024 + 1, 'a')}},
                   "oversized body");
    expect_invalid(proxy, session,
                   {{"method", "POST"}, {"path", "/"}, {"contentType", "text/plain"}},
                   "wrong content type");

    // Identity/socket/URL parameters are never accepted from the caller.
    expect_invalid(proxy, session,
                   {{"method", "GET"}, {"path", "/"}, {"app_id", "app.other"}},
                   "app_id parameter");
    expect_invalid(proxy, session,
                   {{"method", "GET"}, {"path", "/"}, {"socket", "/tmp/other.sock"}},
                   "socket parameter");
    expect_invalid(proxy, session,
                   {{"method", "GET"}, {"path", "/"}, {"url", "http://127.0.0.1/"}},
                   "url parameter");
    expect_invalid(proxy, session,
                   {{"method", "GET"}, {"path", "/"}, {"host", "127.0.0.1"}},
                   "host parameter");
    expect_invalid(proxy, session,
                   {{"method", "GET"}, {"path", "/"}, {"port", 80}}, "port parameter");
    expect_invalid(proxy, session,
                   {{"method", "GET"}, {"path", "/"}, {"token", "other"}},
                   "token parameter");
    expect_invalid(proxy, session, nlohmann::json::array(), "non-object params");

    // Nothing was ever sent to the socket.
    CHECK_EQ(backend.requests().size(), std::size_t{0});
}

void test_unconfigured_and_not_running() {
    Fixture fixture("unavailable");
    fixture.add_app("app.none", false);
    fixture.add_app("app.ghost", true);
    fixture.add_app("app.refused", true);
    BackendProxy proxy(*fixture.store, fixture.apps_root());

    const CallResult unconfigured =
        run_request(proxy, session_for("app.none"), get_params("/"));
    CHECK(!unconfigured.error.ok());
    CHECK(unconfigured.error.code == ErrorCode::not_found);
    CHECK(starts_with(unconfigured.error.message, "backend_not_configured: "));

    // Backend row exists but no socket file was ever created.
    const CallResult missing =
        run_request(proxy, session_for("app.ghost"), get_params("/"));
    CHECK(!missing.error.ok());
    CHECK(missing.error.code == ErrorCode::conflict);
    CHECK(starts_with(missing.error.message, "backend_not_running: "));

    // A socket file exists but nothing is listening: ECONNREFUSED.
    const std::string refused_path = fixture.prepare_socket_dir("app.refused");
    {
        const int fd = ::socket(AF_UNIX, SOCK_STREAM | SOCK_CLOEXEC, 0);
        CHECK(fd >= 0);
        sockaddr_un address{};
        address.sun_family = AF_UNIX;
        std::memcpy(address.sun_path, refused_path.data(), refused_path.size());
        CHECK(::bind(fd, reinterpret_cast<sockaddr*>(&address), sizeof(address)) == 0);
        ::close(fd);
    }
    const CallResult refused =
        run_request(proxy, session_for("app.refused"), get_params("/"));
    ::unlink(refused_path.c_str());
    CHECK(!refused.error.ok());
    CHECK(refused.error.code == ErrorCode::conflict);
    CHECK(starts_with(refused.error.message, "backend_not_running: "));
}

void test_body_framings() {
    Fixture fixture("framings");
    fixture.add_app("app.frames", true);
    TestBackend backend(fixture.prepare_socket_dir("app.frames"));
    BackendProxy proxy(*fixture.store, fixture.apps_root());
    const RuntimeSession session = session_for("app.frames");

    const CallResult chunked = run_request(proxy, session, get_params("/chunked"));
    CHECK(chunked.error.ok());
    CHECK_EQ(chunked.response.status, 200);
    CHECK_STR_EQ(chunked.response.raw_body, R"({"chunk":true})");
    CHECK(chunked.response.body.is_object());
    if (chunked.response.body.is_object()) {
        CHECK_EQ(chunked.response.body.value("chunk", false), true);
    }

    const CallResult close_delimited = run_request(proxy, session, get_params("/close"));
    CHECK(close_delimited.error.ok());
    CHECK_EQ(close_delimited.response.status, 200);
    CHECK_STR_EQ(close_delimited.response.raw_body, R"({"close":true})");
    CHECK(close_delimited.response.body.is_object());
    if (close_delimited.response.body.is_object()) {
        CHECK_EQ(close_delimited.response.body.value("close", false), true);
    }

    const CallResult invalid_json = run_request(proxy, session, get_params("/badjson"));
    CHECK(!invalid_json.error.ok());
    CHECK(starts_with(invalid_json.error.message, "backend_protocol_error: "));
}

void test_timeout() {
    Fixture fixture("timeout");
    fixture.add_app("app.slow", true);
    TestBackend backend(fixture.prepare_socket_dir("app.slow"));
    BackendProxy proxy(*fixture.store, fixture.apps_root(), 1, 8, 150);
    CHECK_EQ(proxy.request_timeout_ms(), 150);

    const auto started = Clock::now();
    const CallResult result =
        run_request(proxy, session_for("app.slow"), get_params("/slow"));
    const auto elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(
        Clock::now() - started);
    CHECK(!result.error.ok());
    CHECK(result.error.code == ErrorCode::unavailable);
    CHECK(starts_with(result.error.message, "backend_timeout:"));
    CHECK(elapsed < std::chrono::seconds(5));
}

void test_query_string_forwarding() {
    Fixture fixture("query");
    fixture.add_app("app.query", true);
    TestBackend backend(fixture.prepare_socket_dir("app.query"));
    BackendProxy proxy(*fixture.store, fixture.apps_root());
    const RuntimeSession session = session_for("app.query");

    const CallResult result =
        run_request(proxy, session, get_params("/notes?limit=2&tag=a%20b&flag"));
    CHECK(result.error.ok());
    CHECK_EQ(result.response.status, 200);

    const std::vector<RecordedRequest> requests = backend.requests();
    CHECK_EQ(requests.size(), std::size_t{1});
    if (!requests.empty()) {
        CHECK_STR_EQ(requests[0].target, "/notes?limit=2&tag=a%20b&flag");
    }
}

void test_queued_request_revalidated() {
    Fixture fixture("revalidate");
    fixture.add_app("app.revalidate", true);
    TestBackend backend(fixture.prepare_socket_dir("app.revalidate"));
    // One worker makes the second request wait behind /slow, exactly like a
    // queue drained after an invalidation.
    BackendProxy proxy(*fixture.store, fixture.apps_root(), 1);
    const RuntimeSession session = session_for("app.revalidate");

    struct Pending {
        std::mutex mutex;
        std::condition_variable finished;
        bool done = false;
        Error error;
        BackendHttpResponse response;
    };
    auto pending = [](Pending& slot) {
        return [&slot](Error error, BackendHttpResponse response) {
            std::lock_guard<std::mutex> lock(slot.mutex);
            slot.error = std::move(error);
            slot.response = std::move(response);
            slot.done = true;
            slot.finished.notify_one();
        };
    };
    auto wait = [](Pending& slot) {
        std::unique_lock<std::mutex> lock(slot.mutex);
        slot.finished.wait_for(lock, std::chrono::seconds(10), [&] { return slot.done; });
        CHECK(slot.done);
    };

    std::atomic<bool> valid{true};
    proxy.set_session_validator([&](std::string_view, RuntimeSession& out) {
        if (!valid.load()) {
            return Error::make(ErrorCode::permission_denied,
                               "runtime_session_invalid: runtime session is no longer valid");
        }
        out = session;
        return Error::none();
    });

    Pending slow;
    Pending queued;
    CHECK(proxy.submit(session, get_params("/slow"), pending(slow)));
    // Wait until the worker is actually processing the first request.
    for (int attempt = 0; attempt < 100 && backend.requests().empty(); ++attempt) {
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
    }
    CHECK_EQ(backend.requests().size(), std::size_t{1});
    CHECK(proxy.submit(session, get_params("/"), pending(queued)));
    // Invalidate the session while the second request is still queued.
    valid.store(false);
    wait(slow);
    wait(queued);

    CHECK(slow.error.ok());
    CHECK(!queued.error.ok());
    CHECK_STR_EQ(queued.error.message.substr(0, queued.error.message.find(':')),
                 "runtime_session_invalid");
    // The stale queued request never reached the backend.
    CHECK_EQ(backend.requests().size(), std::size_t{1});
}

void test_disabled_app_is_not_proxied() {
    Fixture fixture("disabled");
    fixture.add_app("app.disabled", true);
    TestBackend backend(fixture.prepare_socket_dir("app.disabled"));
    BackendProxy proxy(*fixture.store, fixture.apps_root());

    AppRecord app;
    bool found = false;
    CHECK(fixture.store->get_app("app.disabled", app, found).ok());
    CHECK(found);
    app.status = app_status::disabled;
    CHECK(fixture.store->upsert_app(app).ok());

    const CallResult result = run_request(proxy, session_for("app.disabled"), get_params("/"));
    CHECK(!result.error.ok());
    CHECK_STR_EQ(result.error.message.substr(0, result.error.message.find(':')),
                 "backend_app_disabled");
    CHECK_EQ(backend.requests().size(), std::size_t{0});
}

void test_cross_app_isolation() {
    Fixture fixture("isolation");
    fixture.add_app("app.a", true);
    fixture.add_app("app.b", true);
    TestBackend backend_a(fixture.prepare_socket_dir("app.a"));
    TestBackend backend_b(fixture.prepare_socket_dir("app.b"));
    BackendProxy proxy(*fixture.store, fixture.apps_root());
    const RuntimeSession session_a = session_for("app.a");

    // Any attempt to name another backend is rejected before connect.
    expect_invalid(proxy, session_a,
                   {{"method", "GET"}, {"path", "/"}, {"app_id", "app.b"}},
                   "cross-app app_id");
    expect_invalid(proxy, session_a,
                   {{"method", "GET"},
                    {"path", "/"},
                    {"socket", fixture.socket_path("app.b")}},
                   "cross-app socket");
    CHECK_EQ(backend_a.requests().size(), std::size_t{0});
    CHECK_EQ(backend_b.requests().size(), std::size_t{0});

    // The identity selects app A's socket even though app B is also running.
    const CallResult result = run_request(proxy, session_a, get_params("/"));
    CHECK(result.error.ok());
    CHECK_EQ(result.response.status, 200);
    CHECK_EQ(backend_a.requests().size(), std::size_t{1});
    CHECK_EQ(backend_b.requests().size(), std::size_t{0});

    const CallResult result_b =
        run_request(proxy, session_for("app.b"), get_params("/"));
    CHECK(result_b.error.ok());
    CHECK_EQ(backend_b.requests().size(), std::size_t{1});
    CHECK_EQ(backend_a.requests().size(), std::size_t{1});
}

} // namespace

int main() {
    test_get_and_header_filter();
    test_post_body_and_hop_headers();
    test_statuses_are_data();
    test_validation_before_connect();
    test_unconfigured_and_not_running();
    test_body_framings();
    test_timeout();
    test_query_string_forwarding();
    test_queued_request_revalidated();
    test_disabled_app_is_not_proxied();
    test_cross_app_isolation();
    return edgedesk::test::finish("backend_proxy_test");
}
