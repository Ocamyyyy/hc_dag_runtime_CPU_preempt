#include "fpga_backend.h"

#include <atomic>
#include <chrono>
#include <future>
#include <iostream>
#include <memory>
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

std::vector<RpCompatibilityEntry> compatibility_map() {
    return {
        {20, 10, 1, 101},
        {20, 10, 2, 102},
        {20, 11, 1, 201},
    };
}

ReconfigurationRequest configuration_request(uint64_t request_id, int rp_id,
                                             int function_id,
                                             int bitstream_id,
                                             int64_t duration_us = 1000) {
    ReconfigurationRequest request;
    request.configuration_request_id = request_id;
    request.resource_id = 20;
    request.rp_id = rp_id;
    request.function_id = function_id;
    request.bitstream_id = bitstream_id;
    request.expected_config_us = duration_us;
    return request;
}

ExecutionRequest execution_request(uint64_t request_id, int rp_id,
                                   int function_id, int bitstream_id,
                                   int64_t duration_us = 1000) {
    ExecutionRequest request;
    request.request_id = request_id;
    request.job_id = "job-" + std::to_string(request_id);
    request.node_id = "node-" + std::to_string(request_id);
    request.target.node_type = NodeType::FPGA;
    request.target.resource_id = 20;
    request.target.rp_id = rp_id;
    request.target.function_id = function_id;
    request.target.configuration_id = bitstream_id;
    request.expected_exec_us = duration_us;
    return request;
}

void test_normal_configuration_and_execution() {
    std::mutex event_mutex;
    std::vector<FpgaEvent> events;
    auto driver = std::make_shared<FakeFpgaDeviceDriver>();
    FpgaBackend backend(
        driver, compatibility_map(),
        [&](FpgaEvent event) {
            std::lock_guard<std::mutex> lock(event_mutex);
            events.push_back(std::move(event));
        });

    CHECK(backend.initialize());
    auto initial_rp10 = backend.get_rp_state(20, 10);
    auto initial_rp11 = backend.get_rp_state(20, 11);
    CHECK(initial_rp10.has_value());
    CHECK(initial_rp11.has_value());
    CHECK(initial_rp10->state == RpState::UNCONFIGURED);
    CHECK(initial_rp11->state == RpState::UNCONFIGURED);

    std::promise<ReconfigurationResult> configuration_promise;
    auto configuration_future = configuration_promise.get_future();
    std::atomic<int> configuration_callbacks{0};
    CHECK(backend.configure(
              configuration_request(1, 10, 1, 101, 20000),
              [&](ReconfigurationResult result) {
                  configuration_callbacks.fetch_add(1);
                  configuration_promise.set_value(std::move(result));
              }) == ReconfigurationSubmitStatus::ACCEPTED);
    CHECK(backend.get_rp_state(20, 10)->state == RpState::RECONFIGURING);
    CHECK(backend.configure(configuration_request(2, 11, 1, 201),
                            [](ReconfigurationResult) {}) ==
          ReconfigurationSubmitStatus::CONFIGURATION_PORT_BUSY);
    CHECK(backend.submit(execution_request(1, 10, 1, 101),
                         [](ExecutionResult) {}) ==
          SubmitStatus::RESOURCE_BUSY);

    CHECK(configuration_future.wait_for(std::chrono::seconds(1)) ==
          std::future_status::ready);
    const ReconfigurationResult configuration_result =
        configuration_future.get();
    CHECK(configuration_result.status == ReconfigurationStatus::SUCCEEDED);
    CHECK(configuration_result.configuration_request_id == 1);
    CHECK(configuration_result.resource_id == 20);
    CHECK(configuration_result.rp_id == 10);
    CHECK(configuration_result.function_id == 1);
    CHECK(configuration_result.bitstream_id == 101);
    CHECK(configuration_callbacks.load() == 1);
    auto ready = backend.get_rp_state(20, 10);
    CHECK(ready->state == RpState::READY);
    CHECK(ready->current_function_id == 1);
    CHECK(ready->current_bitstream_id == 101);

    CHECK(backend.submit(execution_request(2, 10, 2, 102),
                         [](ExecutionResult) {}) ==
          SubmitStatus::CONFIGURATION_MISMATCH);
    CHECK(backend.submit(execution_request(3, 10, 3, 103),
                         [](ExecutionResult) {}) ==
          SubmitStatus::UNSUPPORTED_TARGET);

    std::promise<ExecutionResult> execution_promise;
    auto execution_future = execution_promise.get_future();
    std::atomic<int> execution_callbacks{0};
    CHECK(backend.submit(
              execution_request(4, 10, 1, 101, 20000),
              [&](ExecutionResult result) {
                  execution_callbacks.fetch_add(1);
                  execution_promise.set_value(std::move(result));
              }) == SubmitStatus::ACCEPTED);
    CHECK(backend.get_rp_state(20, 10)->state == RpState::EXECUTING);
    CHECK(backend.submit(execution_request(5, 10, 1, 101),
                         [](ExecutionResult) {}) ==
          SubmitStatus::RESOURCE_BUSY);
    CHECK(backend.configure(configuration_request(3, 10, 2, 102),
                            [](ReconfigurationResult) {}) ==
          ReconfigurationSubmitStatus::RP_BUSY);

    CHECK(execution_future.wait_for(std::chrono::seconds(1)) ==
          std::future_status::ready);
    const ExecutionResult execution_result = execution_future.get();
    CHECK(execution_result.status == ExecutionStatus::SUCCEEDED);
    CHECK(execution_result.request_id == 4);
    CHECK(execution_result.actual_target.resource_id == 20);
    CHECK(execution_result.actual_target.rp_id == 10);
    CHECK(execution_result.actual_target.function_id == 1);
    CHECK(execution_result.actual_target.configuration_id == 101);
    CHECK(execution_callbacks.load() == 1);
    CHECK(backend.get_rp_state(20, 10)->state == RpState::READY);

    {
        std::lock_guard<std::mutex> lock(event_mutex);
        CHECK(events.size() == 4);
        CHECK(events[0].type == FpgaEventType::CONFIG);
        CHECK(events[0].domain == FpgaOperationDomain::CONFIGURATION);
        CHECK(events[1].type == FpgaEventType::COMPLETE);
        CHECK(events[1].domain == FpgaOperationDomain::CONFIGURATION);
        CHECK(events[2].type == FpgaEventType::SUBMIT);
        CHECK(events[2].domain == FpgaOperationDomain::EXECUTION);
        CHECK(events[3].type == FpgaEventType::COMPLETE);
        CHECK(events[3].domain == FpgaOperationDomain::EXECUTION);
        CHECK(events[0].request_id == 1);
        CHECK(events[2].request_id == 4);
    }

    CHECK(backend.configure(configuration_request(4, 10, 1, 999),
                            [](ReconfigurationResult) {}) ==
          ReconfigurationSubmitStatus::UNSUPPORTED_MAPPING);
    backend.shutdown();
}

void test_configuration_failure_and_recovery() {
    auto driver = std::make_shared<FakeFpgaDeviceDriver>(
        [](const ReconfigurationRequest& request) {
            return request.configuration_request_id == 90 ? 501 : 0;
        });
    FpgaBackend backend(driver, compatibility_map());
    CHECK(backend.initialize());

    std::promise<ReconfigurationResult> failure_promise;
    auto failure_future = failure_promise.get_future();
    CHECK(backend.configure(
              configuration_request(90, 10, 1, 101),
              [&](ReconfigurationResult result) {
                  failure_promise.set_value(std::move(result));
              }) == ReconfigurationSubmitStatus::ACCEPTED);
    CHECK(failure_future.wait_for(std::chrono::seconds(1)) ==
          std::future_status::ready);
    const ReconfigurationResult failed = failure_future.get();
    CHECK(failed.status == ReconfigurationStatus::FAILED);
    CHECK(failed.error_code == 501);
    CHECK(backend.get_rp_state(20, 10)->state == RpState::ERROR);
    CHECK(backend.submit(execution_request(91, 10, 1, 101),
                         [](ExecutionResult) {}) == SubmitStatus::RP_ERROR);

    CHECK(backend.recover_rp(20, 10));
    CHECK(backend.get_rp_state(20, 10)->state == RpState::UNCONFIGURED);

    std::promise<ReconfigurationResult> recovered_promise;
    auto recovered_future = recovered_promise.get_future();
    CHECK(backend.configure(
              configuration_request(91, 10, 1, 101),
              [&](ReconfigurationResult result) {
                  recovered_promise.set_value(std::move(result));
              }) == ReconfigurationSubmitStatus::ACCEPTED);
    CHECK(recovered_future.wait_for(std::chrono::seconds(1)) ==
          std::future_status::ready);
    CHECK(recovered_future.get().status ==
          ReconfigurationStatus::SUCCEEDED);
    CHECK(backend.get_rp_state(20, 10)->state == RpState::READY);
    backend.shutdown();
}

void test_execution_failure_and_recovery() {
    auto driver = std::make_shared<FakeFpgaDeviceDriver>(
        FakeFpgaDeviceDriver::ConfigurationFailureInjector{},
        [](const ExecutionRequest& request) {
            return request.request_id == 90 ? 601 : 0;
        });
    FpgaBackend backend(driver, compatibility_map());
    CHECK(backend.initialize());

    std::promise<ReconfigurationResult> configuration_promise;
    auto configuration_future = configuration_promise.get_future();
    CHECK(backend.configure(
              configuration_request(1, 10, 1, 101),
              [&](ReconfigurationResult result) {
                  configuration_promise.set_value(std::move(result));
              }) == ReconfigurationSubmitStatus::ACCEPTED);
    CHECK(configuration_future.wait_for(std::chrono::seconds(1)) ==
          std::future_status::ready);
    CHECK(configuration_future.get().status ==
          ReconfigurationStatus::SUCCEEDED);

    std::promise<ExecutionResult> execution_promise;
    auto execution_future = execution_promise.get_future();
    CHECK(backend.submit(
              execution_request(90, 10, 1, 101),
              [&](ExecutionResult result) {
                  execution_promise.set_value(std::move(result));
              }) == SubmitStatus::ACCEPTED);
    CHECK(execution_future.wait_for(std::chrono::seconds(1)) ==
          std::future_status::ready);
    const ExecutionResult failed = execution_future.get();
    CHECK(failed.status == ExecutionStatus::FAILED);
    CHECK(failed.error_code == 601);
    CHECK(backend.get_rp_state(20, 10)->state == RpState::ERROR);
    CHECK(backend.recover_rp(20, 10));
    CHECK(backend.get_rp_state(20, 10)->state == RpState::UNCONFIGURED);
    backend.shutdown();
}

void test_shutdown_drains_configuration_callback() {
    auto driver = std::make_shared<FakeFpgaDeviceDriver>();
    FpgaBackend backend(driver, compatibility_map());
    CHECK(backend.initialize());

    std::atomic<int> callback_count{0};
    std::atomic<bool> callback_succeeded{false};
    CHECK(backend.configure(
              configuration_request(200, 10, 1, 101, 20000),
              [&](ReconfigurationResult result) {
                  callback_succeeded.store(
                      result.status == ReconfigurationStatus::SUCCEEDED);
                  callback_count.fetch_add(1);
              }) == ReconfigurationSubmitStatus::ACCEPTED);
    backend.shutdown();
    CHECK(callback_count.load() == 1);
    CHECK(callback_succeeded.load());
    std::this_thread::sleep_for(std::chrono::milliseconds(5));
    CHECK(callback_count.load() == 1);
    CHECK(backend.configure(configuration_request(201, 10, 1, 101),
                            [](ReconfigurationResult) {}) ==
          ReconfigurationSubmitStatus::CLOSED);
    CHECK(backend.submit(execution_request(201, 10, 1, 101),
                         [](ExecutionResult) {}) == SubmitStatus::CLOSED);
}

}

int main() {
    test_normal_configuration_and_execution();
    test_configuration_failure_and_recovery();
    test_execution_failure_and_recovery();
    test_shutdown_drains_configuration_callback();

    if (failures != 0) {
        std::cerr << failures << " check(s) failed\n";
        return 1;
    }
    std::cout << "FpgaBackend and RP state tests passed\n";
    return 0;
}
