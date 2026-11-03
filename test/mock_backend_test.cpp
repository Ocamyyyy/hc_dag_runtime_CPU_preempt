#include "backend.h"

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <future>
#include <iostream>
#include <mutex>
#include <thread>
#include <vector>

namespace {

int failures = 0;

#define CHECK(condition)                                                     \
    do {                                                                     \
        if (!(condition)) {                                                  \
            std::cerr << "CHECK failed at line " << __LINE__ << ": "       \
                      << #condition << '\n';                                \
            ++failures;                                                      \
        }                                                                    \
    } while (false)

ExecutionRequest make_request(uint64_t request_id, int resource_id,
                              int64_t expected_exec_us = 1000) {
    ExecutionRequest request;
    request.request_id = request_id;
    request.job_id = "job-" + std::to_string(request_id);
    request.node_id = "node-" + std::to_string(request_id);
    request.target.node_type = NodeType::FPGA;
    request.target.resource_id = resource_id;
    request.target.cpu_core = 2;
    request.target.rp_id = 3;
    request.target.function_id = 4;
    request.target.configuration_id = 40;
    request.expected_exec_us = expected_exec_us;
    return request;
}

void test_initialize_and_successful_async_submit() {
    MockBackend backend;
    CHECK(backend.initialize());
    CHECK(backend.initialize());

    std::promise<ExecutionResult> promise;
    auto future = promise.get_future();
    std::atomic<int> callback_count{0};
    const std::thread::id caller_thread = std::this_thread::get_id();
    std::thread::id callback_thread;

    const SubmitStatus status = backend.submit(
        make_request(1, 7, 2000),
        [&](ExecutionResult result) {
            callback_thread = std::this_thread::get_id();
            callback_count.fetch_add(1);
            promise.set_value(std::move(result));
        });

    CHECK(status == SubmitStatus::ACCEPTED);
    CHECK(future.wait_for(std::chrono::seconds(1)) ==
          std::future_status::ready);
    const ExecutionResult result = future.get();

    CHECK(callback_count.load() == 1);
    CHECK(callback_thread != caller_thread);
    CHECK(result.status == ExecutionStatus::SUCCEEDED);
    CHECK(result.request_id == 1);
    CHECK(result.job_id == "job-1");
    CHECK(result.node_id == "node-1");
    CHECK(result.actual_target.resource_id == 7);
    CHECK(result.actual_target.rp_id == 3);
    CHECK(result.actual_target.function_id == 4);
    CHECK(result.actual_target.configuration_id == 40);
    CHECK(result.end_time_us >= result.start_time_us);
    CHECK(result.actual_exec_us ==
          result.end_time_us - result.start_time_us);

    backend.shutdown();
    CHECK(callback_count.load() == 1);
}

void test_simulated_failure() {
    MockBackend backend([](const ExecutionRequest& request) {
        return request.request_id == 2 ? 42 : 0;
    });
    CHECK(backend.initialize());

    std::promise<ExecutionResult> promise;
    auto future = promise.get_future();
    CHECK(backend.submit(make_request(2, 8),
                         [&](ExecutionResult result) {
                             promise.set_value(std::move(result));
                         }) == SubmitStatus::ACCEPTED);

    CHECK(future.wait_for(std::chrono::seconds(1)) ==
          std::future_status::ready);
    const ExecutionResult result = future.get();
    CHECK(result.status == ExecutionStatus::FAILED);
    CHECK(result.error_code == 42);
    CHECK(!result.error_message.empty());
    backend.shutdown();
}

void test_rejection_never_calls_callback() {
    MockBackend backend;
    std::atomic<int> callback_count{0};
    auto callback = [&](ExecutionResult) { callback_count.fetch_add(1); };

    CHECK(backend.submit(make_request(3, 9), callback) ==
          SubmitStatus::NOT_INITIALIZED);
    CHECK(callback_count.load() == 0);

    CHECK(backend.initialize());
    backend.shutdown();
    CHECK(backend.submit(make_request(4, 9), callback) ==
          SubmitStatus::CLOSED);
    CHECK(callback_count.load() == 0);
}

void test_shutdown_during_execution() {
    MockBackend backend;
    CHECK(backend.initialize());

    std::atomic<int> callback_count{0};
    std::atomic<bool> callback_finished{false};
    CHECK(backend.submit(
              make_request(5, 10, 20000),
              [&](ExecutionResult result) {
                  CHECK(result.status == ExecutionStatus::SUCCEEDED);
                  callback_count.fetch_add(1);
                  callback_finished.store(true);
              }) == SubmitStatus::ACCEPTED);

    backend.shutdown();
    CHECK(callback_finished.load());
    CHECK(callback_count.load() == 1);
    std::this_thread::sleep_for(std::chrono::milliseconds(5));
    CHECK(callback_count.load() == 1);
}

void test_multiple_requests() {
    MockBackend backend;
    CHECK(backend.initialize());

    constexpr int request_count = 8;
    std::mutex mutex;
    std::condition_variable condition;
    std::vector<ExecutionResult> results;

    for (int index = 0; index < request_count; ++index) {
        const uint64_t request_id = static_cast<uint64_t>(100 + index);
        const int resource_id = 20 + index;
        CHECK(backend.submit(
                  make_request(request_id, resource_id, 200),
                  [&](ExecutionResult result) {
                      {
                          std::lock_guard<std::mutex> lock(mutex);
                          results.push_back(std::move(result));
                      }
                      condition.notify_one();
                  }) == SubmitStatus::ACCEPTED);
    }

    {
        std::unique_lock<std::mutex> lock(mutex);
        CHECK(condition.wait_for(lock, std::chrono::seconds(2), [&] {
            return results.size() == request_count;
        }));
    }

    backend.shutdown();
    CHECK(results.size() == request_count);
    for (int index = 0; index < request_count; ++index) {
        CHECK(results[index].request_id ==
              static_cast<uint64_t>(100 + index));
        CHECK(results[index].actual_target.resource_id == 20 + index);
        CHECK(results[index].status == ExecutionStatus::SUCCEEDED);
    }
}

}

int main() {
    test_initialize_and_successful_async_submit();
    test_simulated_failure();
    test_rejection_never_calls_callback();
    test_shutdown_during_execution();
    test_multiple_requests();

    if (failures != 0) {
        std::cerr << failures << " check(s) failed\n";
        return 1;
    }
    std::cout << "MockBackend tests passed\n";
    return 0;
}
