#include "test_util.hpp"

#include "core/util.hpp"
#include "terminal/terminal_manager.hpp"
#include "terminal/terminal_session.hpp"

#include <atomic>
#include <filesystem>
#include <fstream>
#include <chrono>
#include <condition_variable>
#include <mutex>
#include <string>
#include <thread>
#include <unistd.h>

using namespace edgedesk;

namespace {

constexpr int kTimeoutMs = 8000;

bool bash_available() { return ::access("/bin/bash", X_OK) == 0; }

struct OutputCollector {
    std::mutex mutex;
    std::condition_variable wakeup;
    std::string buffer;
    int exit_code = -1;
    bool exited = false;

    void append(const char* data, std::size_t length) {
        std::lock_guard<std::mutex> lock(mutex);
        buffer.append(data, length);
        wakeup.notify_all();
    }

    void on_exit(int code) {
        std::lock_guard<std::mutex> lock(mutex);
        exit_code = code;
        exited = true;
        wakeup.notify_all();
    }

    bool wait_for(const std::string& needle, int timeout_ms = kTimeoutMs) {
        const auto deadline = std::chrono::steady_clock::now() + std::chrono::milliseconds(timeout_ms);
        std::unique_lock<std::mutex> lock(mutex);
        while (buffer.find(needle) == std::string::npos) {
            if (wakeup.wait_until(lock, deadline) == std::cv_status::timeout) {
                return buffer.find(needle) != std::string::npos;
            }
        }
        return true;
    }

    bool wait_for_exit(int timeout_ms = kTimeoutMs) {
        const auto deadline = std::chrono::steady_clock::now() + std::chrono::milliseconds(timeout_ms);
        std::unique_lock<std::mutex> lock(mutex);
        while (!exited) {
            if (wakeup.wait_until(lock, deadline) == std::cv_status::timeout) return exited;
        }
        return true;
    }

    std::string snapshot() {
        std::lock_guard<std::mutex> lock(mutex);
        return buffer;
    }
};

std::shared_ptr<TerminalSession> create_session(OutputCollector& collector, std::size_t backlog = 64 * 1024,
                                                const std::string& shell = "/bin/bash",
                                                const std::string& cwd = "") {
    TerminalSession::CreateArgs args;
    args.id = "test-session";
    args.title = "test";
    args.cwd = cwd;
    args.spawn.shell = shell;
    args.spawn.arguments = {"-l"};
    args.spawn.cwd = cwd;
    args.spawn.cols = 100;
    args.spawn.rows = 30;
    args.backlog_bytes = backlog;
    Error error;
    auto session = TerminalSession::create(std::move(args), error);
    if (!session) {
        std::fprintf(stderr, "terminal session error: %s\n", error.message.c_str());
        return nullptr;
    }
    session->subscribe(TerminalSession::Subscriber{
        [&collector](const char* data, std::size_t length) { collector.append(data, length); },
        [&collector](int code) { collector.on_exit(code); }});
    return session;
}

void test_login_bash() {
    OutputCollector collector;
    auto session = create_session(collector);
    CHECK(session != nullptr);
    if (!session) return;

    // Real Bash, interactive login shell semantics.
    session->send_raw("echo READY-$$\r");
    CHECK(collector.wait_for("READY-"));
    session->send_raw("shopt -q login_shell && echo LOGIN_SHELL_YES || echo LOGIN_SHELL_NO\r");
    CHECK(collector.wait_for("LOGIN_SHELL_YES"));
    session->send_raw("echo \"BASH=${BASH_VERSION}\"\r");
    CHECK(collector.wait_for("BASH="));
    session->send_raw("echo \"HOME_IS=${HOME}\"\r");
    CHECK(collector.wait_for("HOME_IS=/"));

    // ANSI / UTF-8 survive the PTY.
    session->send_raw("echo -e '\\033[31mCOLOR_OK\\033[0m'\r");
    CHECK(collector.wait_for("COLOR_OK"));
    session->send_raw("echo 中文测试\r");
    CHECK(collector.wait_for("中文测试"));

    CHECK(session->running());

    // Resize goes through TIOCSWINSZ; the kernel updates the PTY size
    // immediately, but bash may be busy, so poll a few times before failing.
    Error error = session->resize(120, 40);
    CHECK(error.ok());
    const auto resized_info = session->info();
    CHECK_EQ(resized_info.cols, static_cast<unsigned short>(120));
    CHECK_EQ(resized_info.rows, static_cast<unsigned short>(40));
    bool saw_size = false;
    for (int attempt = 0; attempt < 6 && !saw_size; ++attempt) {
        session->send_raw("echo \"SIZE=$(stty size)\"\r");
        saw_size = collector.wait_for("SIZE=40 120", 2000);
    }
    if (!saw_size) {
        const auto tail = collector.snapshot();
        std::fprintf(stderr, "resize diagnostic, output tail: %s\n",
                     tail.substr(tail.size() > 400 ? tail.size() - 400 : 0).c_str());
    }
    CHECK(saw_size);

    // The session survives a subscriber disconnect (reconnect gets the replay).
    const auto replay = session->replay_buffer();
    CHECK(replay.find("READY-") != std::string::npos);

    session->request_close();
    CHECK(collector.wait_for_exit());
    CHECK(!session->running());
}

void test_guard_blocks_without_executing() {
    OutputCollector collector;
    auto session = create_session(collector);
    CHECK(session != nullptr);
    if (!session) return;

    session->send_raw("echo BEFORE_GUARD\r");
    CHECK(collector.wait_for("BEFORE_GUARD"));

    std::string reason;
    const auto outcome = session->submit_input("rm -rf /\r", reason);
    CHECK_EQ(static_cast<int>(outcome), static_cast<int>(TerminalSession::InputOutcome::guard_blocked));
    CHECK(!reason.empty());
    CHECK(session->info().pending_confirmation);

    // Nothing executed and the line is cleared instead of confirmed.
    CHECK(session->cancel_pending());
    CHECK(!session->info().pending_confirmation);
    session->send_raw("echo AFTER_CANCEL\r");
    CHECK(collector.wait_for("AFTER_CANCEL"));
    CHECK(collector.snapshot().find("AFTER_CANCEL") != std::string::npos);

    session->request_close();
    CHECK(collector.wait_for_exit());
}

void test_login_profile_loading() {
    // A private HOME with ~/.profile proves that `bash -l` sources the login
    // profile (what /etc/profile + user profile behave like on a device).
    const auto home = std::filesystem::temp_directory_path() / ("edgedesk-home-" + std::to_string(::getpid()));
    std::filesystem::remove_all(home);
    std::filesystem::create_directories(home);
    {
        std::ofstream profile(home / ".profile");
        profile << "export EDGEDESK_PROFILE_MARKER=profile-loaded\n";
    }

    OutputCollector collector;
    TerminalSession::CreateArgs args;
    args.id = "profile-session";
    args.title = "profile";
    args.spawn.shell = "/bin/bash";
    args.spawn.arguments = {"-l"};
    args.spawn.extra_env.emplace_back("HOME", home.string());
    args.backlog_bytes = 64 * 1024;
    Error error;
    auto session = TerminalSession::create(std::move(args), error);
    CHECK(session != nullptr);
    if (session) {
        session->subscribe(TerminalSession::Subscriber{
            [&collector](const char* data, std::size_t length) { collector.append(data, length); },
            [&collector](int code) { collector.on_exit(code); }});
        session->send_raw("echo \"PROFILE=${EDGEDESK_PROFILE_MARKER:-missing}\"\r");
        CHECK(collector.wait_for("PROFILE=profile-loaded"));
        session->request_close();
        CHECK(collector.wait_for_exit());
    }
    std::filesystem::remove_all(home);
}

void test_shell_missing_is_reported() {
    TerminalManagerOptions options;
    options.bash_path = "/nonexistent/bash";
    TerminalManager manager(options);
    CHECK(!manager.shell_available());
    TerminalManager::CreateRequest request;
    std::shared_ptr<TerminalSession> session;
    Error error = manager.create(request, session);
    CHECK(!error.ok());
    CHECK_EQ(static_cast<int>(error.code), static_cast<int>(ErrorCode::unsupported));
}

void test_manager_lifecycle() {
    TerminalManagerOptions options;
    options.max_sessions = 2;
    options.idle_timeout_seconds = 0; // disabled for the test
    options.backlog_bytes = 32 * 1024;
    TerminalManager manager(options);
    CHECK(manager.shell_available());
    if (!manager.shell_available()) return;

    TerminalManager::CreateRequest request;
    request.cols = 80;
    request.rows = 24;
    std::shared_ptr<TerminalSession> first;
    std::shared_ptr<TerminalSession> second;
    std::shared_ptr<TerminalSession> third;
    CHECK(manager.create(request, first).ok());
    CHECK(manager.create(request, second).ok());
    Error error = manager.create(request, third);
    CHECK(!error.ok());
    CHECK_EQ(static_cast<int>(error.code), static_cast<int>(ErrorCode::limit_exceeded));
    CHECK_EQ(manager.running_count(), std::size_t{2});

    CHECK(manager.find(first->id()) == first);
    CHECK(manager.find("missing") == nullptr);
    CHECK_EQ(manager.list().size(), std::size_t{2});

    // Explicit close removes the session from the registry.
    CHECK(manager.request_close(first->id()));
    CHECK(manager.find(first->id()) == nullptr);
    CHECK(!manager.request_close(first->id()));

    // A closed slot can be reused.
    std::this_thread::sleep_for(std::chrono::milliseconds(300));
    CHECK(manager.create(request, third).ok());
    CHECK_EQ(manager.running_count(), std::size_t{2});

    // Sweep must not drop sessions that are used (subscribers or recent IO).
    manager.sweep(now_unix_millis());
    CHECK_EQ(manager.list().size(), std::size_t{2});

    manager.shutdown();
    CHECK_EQ(manager.session_count(), std::size_t{0});
}

void test_idle_sweep() {
    TerminalManagerOptions options;
    options.max_sessions = 4;
    options.idle_timeout_seconds = 1;
    TerminalManager manager(options);
    if (!manager.shell_available()) return;

    TerminalManager::CreateRequest request;
    std::shared_ptr<TerminalSession> session;
    CHECK(manager.create(request, session).ok());

    // Detached and older than the timeout -> closed by the sweep.
    manager.sweep(now_unix_millis() + 5000);
    std::this_thread::sleep_for(std::chrono::milliseconds(400));
    CHECK_EQ(manager.running_count(), std::size_t{0});

    manager.shutdown();
}

void test_backlog_is_bounded() {
    OutputCollector collector;
    auto session = create_session(collector, 16 * 1024);
    CHECK(session != nullptr);
    if (!session) return;
    // Produce more than the backlog.
    session->send_raw("for i in $(seq 1 2000); do echo 0123456789abcdef0123456789abcdef; done\r");
    CHECK(collector.wait_for("0123456789abcdef", 15000));
    std::this_thread::sleep_for(std::chrono::milliseconds(500));
    CHECK(session->buffered_bytes() <= 16 * 1024);
    session->request_close();
    CHECK(collector.wait_for_exit(15000));
}

} // namespace

int main() {
    if (!bash_available()) {
        std::printf("terminal_session_test: SKIPPED (no /bin/bash)\n");
        return 0;
    }
    test_login_bash();
    test_guard_blocks_without_executing();
    test_login_profile_loading();
    test_shell_missing_is_reported();
    test_manager_lifecycle();
    test_idle_sweep();
    test_backlog_is_bounded();
    return edgedesk::test::finish("terminal_session_test");
}
