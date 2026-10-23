#include "app/supervisor_service.hpp"

#include <algorithm>
#include <chrono>
#include <condition_variable>
#include <deque>
#include <mutex>
#include <thread>
#include <utility>
#include <vector>

namespace edgedesk {
namespace {

SupervisorResult unavailable(std::string message) {
    SupervisorResult result;
    result.status = 503;
    result.code = "supervisor_unavailable";
    result.message = std::move(message);
    return result;
}

SupervisorResult map_rpc_error(int code, std::string message, std::string slug) {
    SupervisorResult result;
    result.code = slug;
    result.message = message.empty() ? "supervisor request failed" : std::move(message);
    switch (code) {
        case -32602:
            result.status = 400;
            if (result.code.empty()) result.code = "invalid_service_config";
            break;
        case -32002:
            result.status = 404;
            if (result.code.empty()) result.code = "service_not_found";
            break;
        case -32003:
            result.status = 409;
            if (result.code.empty()) result.code = "service_running";
            break;
        case -32004:
            result.status = 403;
            if (result.code.empty()) result.code = "permission_denied";
            break;
        case -32601:
            result.status = 400;
            if (result.code.empty()) result.code = "invalid_request";
            break;
        case -32005:
            result.status = 503;
            if (result.code.empty()) result.code = "supervisor_error";
            break;
        default:
            result.status = 502;
            if (result.code.empty()) result.code = "supervisor_error";
            break;
    }
    return result;
}

} // namespace

struct SupervisorService::Impl {
    std::unique_ptr<supervisor::SupervisorClient> client;
    std::size_t queue_limit = 32;
    mutable std::mutex mutex;
    std::condition_variable wakeup;
    std::deque<std::function<void()>> queue;
    std::vector<std::thread> workers;
    bool stopping = false;

    Impl(std::unique_ptr<supervisor::SupervisorClient> owner, std::size_t workers_count,
         std::size_t limit)
        : client(std::move(owner)), queue_limit(std::max<std::size_t>(1, limit)) {
        const std::size_t count = std::max<std::size_t>(1, workers_count);
        workers.reserve(count);
        for (std::size_t index = 0; index < count; ++index) {
            workers.emplace_back([this] { run(); });
        }
    }

    ~Impl() {
        {
            std::lock_guard<std::mutex> lock(mutex);
            stopping = true;
            queue.clear();
        }
        wakeup.notify_all();
        for (auto& worker : workers) {
            if (worker.joinable()) worker.join();
        }
    }

    void run() {
        for (;;) {
            std::function<void()> job;
            {
                std::unique_lock<std::mutex> lock(mutex);
                wakeup.wait(lock, [this] { return stopping || !queue.empty(); });
                if (stopping && queue.empty()) return;
                job = std::move(queue.front());
                queue.pop_front();
            }
            try {
                job();
            } catch (...) {
                // A completion callback must never terminate the process.
            }
        }
    }

    bool submit(std::function<void()> job) {
        {
            std::lock_guard<std::mutex> lock(mutex);
            if (stopping || queue.size() >= queue_limit) return false;
            queue.push_back(std::move(job));
        }
        wakeup.notify_one();
        return true;
    }

    std::size_t pending() const {
        std::lock_guard<std::mutex> lock(mutex);
        return queue.size();
    }

    SupervisorResult call_rpc(std::string method, nlohmann::json params) {
        SupervisorResult result;
        if (!client) {
            result.status = 503;
            result.code = "supervisor_unavailable";
            result.message = "process supervisor is not configured";
            return result;
        }
        nlohmann::json value;
        supervisor::SupervisorRpcError rpc_error;
        const Error error = client->call(method, params, value, rpc_error);
        if (!error.ok()) {
            result.status = error.code == ErrorCode::unavailable ? 503 : 502;
            result.code = error.code == ErrorCode::unavailable ? "supervisor_unavailable"
                                                              : "supervisor_error";
            result.message = error.message;
            return result;
        }
        if (rpc_error.code != 0) {
            return map_rpc_error(rpc_error.code, std::move(rpc_error.message),
                                 std::move(rpc_error.slug));
        }
        result.status = 200;
        result.value = std::move(value);
        return result;
    }

    void execute(std::string method, nlohmann::json params, Callback callback) {
        if (!client) {
            if (callback) callback(unavailable("process supervisor is not configured"));
            return;
        }
        SupervisorResult result = call_rpc(std::move(method), std::move(params));
        if (callback) callback(std::move(result));
    }

    void execute_stop_and_lock(const std::string& id, Callback callback) {
        const auto finish = [&callback](SupervisorResult result) {
            if (callback) callback(std::move(result));
        };
        const auto absent_result = [&id]() {
            SupervisorResult result;
            result.status = 200;
            result.value = {{"id", id}, {"state", "STOPPED"}, {"absent", true}};
            return result;
        };
        const auto not_found = [](const SupervisorResult& result) {
            return result.status == 404 || result.code == "service_not_found";
        };

        SupervisorResult current = call_rpc("service.get", {{"id", id}});
        if (not_found(current)) {
            finish(absent_result());
            return;
        }
        if (current.status < 200 || current.status >= 300) {
            finish(std::move(current));
            return;
        }

        // Disarm autostart before touching the process: if the Supervisor dies
        // mid-disable, the next boot must not start a disabled application.
        SupervisorResult lock_error = call_rpc(
            "service.update", {{"id", id}, {"autostart", false}});
        if (not_found(lock_error)) {
            finish(absent_result());
            return;
        }
        const bool locked = lock_error.status >= 200 && lock_error.status < 300;

        // Always submit stop, including STOPPED/FAILED/BACKOFF: the Supervisor
        // persists desired_state=stopped and cancels a pending backoff restart.
        SupervisorResult stopped = call_rpc("service.stop", {{"id", id}});
        if (not_found(stopped)) {
            finish(absent_result());
            return;
        }
        if (stopped.status < 200 || stopped.status >= 300) {
            if (stopped.code != "service_stopping") {
                finish(std::move(stopped));
                return;
            }
        } else {
            const std::string operation = stopped.value.value("operation_id", std::string());
            if (!operation.empty()) {
                const auto deadline =
                    std::chrono::steady_clock::now() + std::chrono::seconds(30);
                for (;;) {
                    SupervisorResult operation_state =
                        call_rpc("service.operation.get", {{"operation_id", operation}});
                    if (operation_state.status < 200 || operation_state.status >= 300) {
                        finish(std::move(operation_state));
                        return;
                    }
                    const std::string operation_result =
                        operation_state.value.value("state", std::string());
                    if (operation_result == "failed") {
                        SupervisorResult failure;
                        failure.status = 502;
                        failure.code = "supervisor_error";
                        failure.message = "the Supervisor could not stop the service";
                        if (operation_state.value.contains("error") &&
                            operation_state.value["error"].is_object()) {
                            const auto& detail = operation_state.value["error"];
                            failure.code = detail.value("slug", failure.code);
                            failure.message = detail.value("message", failure.message);
                        }
                        finish(std::move(failure));
                        return;
                    }
                    if (operation_result == "succeeded") break;
                    if (std::chrono::steady_clock::now() >= deadline) {
                        SupervisorResult timeout;
                        timeout.status = 504;
                        timeout.code = "supervisor_timeout";
                        timeout.message = "timed out waiting for the service to stop";
                        finish(std::move(timeout));
                        return;
                    }
                    std::this_thread::sleep_for(std::chrono::milliseconds(50));
                }
            }
        }

        SupervisorResult after = call_rpc("service.get", {{"id", id}});
        if (not_found(after)) {
            finish(absent_result());
            return;
        }
        if (after.status < 200 || after.status >= 300) {
            finish(std::move(after));
            return;
        }
        const std::string state = after.value.value("state", std::string("UNKNOWN"));
        if (state != "STOPPED" && state != "FAILED") {
            SupervisorResult failure;
            failure.status = 409;
            failure.code = state == "UNKNOWN" ? "service_unknown" : "service_stopping";
            failure.message = "the service is not confirmed stopped (state " + state + ")";
            finish(std::move(failure));
            return;
        }
        if (!locked) {
            // The process is gone but autostart could not be cleared. Report
            // failure so the caller does not mark a boot-startable app disabled.
            finish(std::move(lock_error));
            return;
        }
        finish(std::move(after));
    }
};

SupervisorService::SupervisorService(std::unique_ptr<supervisor::SupervisorClient> client,
                                     std::size_t workers, std::size_t queue_limit)
    : impl_(std::make_unique<Impl>(std::move(client), workers, queue_limit)) {}

SupervisorService::~SupervisorService() = default;

bool SupervisorService::configured() const { return impl_->client != nullptr; }

bool SupervisorService::submit(std::string method, nlohmann::json params,
                               Callback callback) {
    return impl_->submit(
        [this, method = std::move(method), params = std::move(params),
         callback = std::move(callback)]() mutable {
            impl_->execute(std::move(method), std::move(params), std::move(callback));
        });
}

bool SupervisorService::submit_stop_and_lock(std::string service_id, Callback callback) {
    return impl_->submit([this, service_id = std::move(service_id),
                          callback = std::move(callback)]() mutable {
        impl_->execute_stop_and_lock(service_id, std::move(callback));
    });
}

std::size_t SupervisorService::pending() const { return impl_->pending(); }

int SupervisorService::request_timeout_ms() const {
    return impl_->client ? impl_->client->request_timeout_ms() : 0;
}

} // namespace edgedesk
