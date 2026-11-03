#include "backend.h"

#include <chrono>
#include <condition_variable>
#include <deque>
#include <mutex>
#include <thread>
#include <unordered_set>
#include <utility>

namespace {

int64_t monotonic_time_us() {
    using namespace std::chrono;
    return duration_cast<microseconds>(
               steady_clock::now().time_since_epoch())
        .count();
}

}

class MockBackend::Impl {
public:
    struct PendingRequest {
        ExecutionRequest request;
        CompletionCallback callback;
    };

    explicit Impl(FailureInjector injector)
        : failure_injector(std::move(injector)) {}

    bool initialize() {
        std::lock_guard<std::mutex> lifecycle_lock(lifecycle_mutex);
        std::lock_guard<std::mutex> lock(mutex);
        if (closed) return false;
        if (initialized) return true;

        stopping = false;
        try {
            worker = std::thread([this] { worker_loop(); });
        } catch (...) {
            return false;
        }
        initialized = true;
        return true;
    }

    SubmitStatus submit(ExecutionRequest request,
                        CompletionCallback callback) {
        if (!callback || request.request_id == 0 || request.job_id.empty() ||
            request.node_id.empty() || request.target.resource_id < 0 ||
            request.expected_exec_us < 0) {
            return SubmitStatus::INVALID_REQUEST;
        }

        {
            std::lock_guard<std::mutex> lock(mutex);
            if (closed) return SubmitStatus::CLOSED;
            if (!initialized) return SubmitStatus::NOT_INITIALIZED;
            if (stopping) return SubmitStatus::SHUTTING_DOWN;
            if (!seen_request_ids.insert(request.request_id).second) {
                return SubmitStatus::INVALID_REQUEST;
            }
            queue.push_back({std::move(request), std::move(callback)});
        }
        condition.notify_one();
        return SubmitStatus::ACCEPTED;
    }

    void shutdown() {
        std::lock_guard<std::mutex> lifecycle_lock(lifecycle_mutex);
        {
            std::lock_guard<std::mutex> lock(mutex);
            if (closed) return;
            closed = true;
            stopping = true;
        }
        condition.notify_all();

        if (worker.joinable()) worker.join();

        std::lock_guard<std::mutex> lock(mutex);
        initialized = false;
    }

private:
    void worker_loop() {
        while (true) {
            PendingRequest pending;
            {
                std::unique_lock<std::mutex> lock(mutex);
                condition.wait(lock, [this] {
                    return stopping || !queue.empty();
                });
                if (queue.empty()) {
                    if (stopping) return;
                    continue;
                }
                pending = std::move(queue.front());
                queue.pop_front();
            }

            ExecutionResult result;
            result.request_id = pending.request.request_id;
            result.job_id = pending.request.job_id;
            result.node_id = pending.request.node_id;
            result.actual_target = pending.request.target;
            result.start_time_us = monotonic_time_us();

            if (pending.request.expected_exec_us > 0) {
                std::this_thread::sleep_for(
                    std::chrono::microseconds(pending.request.expected_exec_us));
            }

            result.end_time_us = monotonic_time_us();
            result.actual_exec_us = result.end_time_us - result.start_time_us;

            int failure_code = 0;
            if (failure_injector) {
                try {
                    failure_code = failure_injector(pending.request);
                } catch (...) {
                    failure_code = -1;
                }
            }

            if (failure_code == 0) {
                result.status = ExecutionStatus::SUCCEEDED;
            } else {
                result.status = ExecutionStatus::FAILED;
                result.error_code = failure_code;
                result.error_message = "mock execution failure";
            }

            try {
                pending.callback(std::move(result));
            } catch (...) {
            }
        }
    }

    FailureInjector failure_injector;
    std::mutex lifecycle_mutex;
    std::mutex mutex;
    std::condition_variable condition;
    std::deque<PendingRequest> queue;
    std::unordered_set<uint64_t> seen_request_ids;
    std::thread worker;
    bool initialized{false};
    bool stopping{false};
    bool closed{false};
};

MockBackend::MockBackend(FailureInjector failure_injector)
    : impl_(std::make_unique<Impl>(std::move(failure_injector))) {}

MockBackend::~MockBackend() {
    shutdown();
}

bool MockBackend::initialize() {
    return impl_->initialize();
}

SubmitStatus MockBackend::submit(ExecutionRequest request,
                                 CompletionCallback on_complete) {
    return impl_->submit(std::move(request), std::move(on_complete));
}

void MockBackend::shutdown() {
    impl_->shutdown();
}
