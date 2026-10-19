#include "app/supervisor_service.hpp"

#include <algorithm>
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

    void execute(std::string method, nlohmann::json params, Callback callback) {
        if (!client) {
            if (callback) callback(unavailable("process supervisor is not configured"));
            return;
        }
        nlohmann::json value;
        supervisor::SupervisorRpcError rpc_error;
        const Error error = client->call(method, params, value, rpc_error);
        if (!error.ok()) {
            SupervisorResult result;
            result.status = error.code == ErrorCode::unavailable ? 503 : 502;
            result.code = error.code == ErrorCode::unavailable ? "supervisor_unavailable"
                                                              : "supervisor_error";
            result.message = error.message;
            if (callback) callback(std::move(result));
            return;
        }
        if (rpc_error.code != 0) {
            if (callback) {
                callback(map_rpc_error(rpc_error.code, std::move(rpc_error.message),
                                       std::move(rpc_error.slug)));
            }
            return;
        }
        SupervisorResult result;
        result.status = 200;
        result.value = std::move(value);
        if (callback) callback(std::move(result));
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

std::size_t SupervisorService::pending() const { return impl_->pending(); }

int SupervisorService::request_timeout_ms() const {
    return impl_->client ? impl_->client->request_timeout_ms() : 0;
}

} // namespace edgedesk
