#include "fpga_device_driver.h"

#include <chrono>
#include <mutex>
#include <thread>
#include <utility>
#include <vector>

namespace {

int64_t monotonic_time_us() {
    using namespace std::chrono;
    return duration_cast<microseconds>(
               steady_clock::now().time_since_epoch())
        .count();
}

}

class FakeFpgaDeviceDriver::Impl {
public:
    Impl(ConfigurationFailureInjector configuration_injector,
         ExecutionFailureInjector execution_injector)
        : configuration_failure(std::move(configuration_injector)),
          execution_failure(std::move(execution_injector)) {}

    bool initialize() {
        std::lock_guard<std::mutex> lock(mutex);
        if (closed) return false;
        initialized = true;
        return true;
    }

    bool configure_async(ReconfigurationRequest request,
                         CompletionCallback callback) {
        if (!callback) return false;
        int failure_code = configuration_failure
                               ? configuration_failure(request)
                               : 0;
        std::lock_guard<std::mutex> lock(mutex);
        if (!initialized || closing || closed) return false;
        try {
            workers.emplace_back(
                [request = std::move(request),
                 callback = std::move(callback), failure_code]() mutable {
                    FpgaDriverResult result;
                    result.start_time_us = monotonic_time_us();
                    if (request.expected_config_us > 0) {
                        std::this_thread::sleep_for(std::chrono::microseconds(
                            request.expected_config_us));
                    }
                    result.end_time_us = monotonic_time_us();
                    result.success = failure_code == 0;
                    result.error_code = failure_code;
                    if (!result.success) {
                        result.error_message =
                            "fake FPGA configuration failure";
                    }
                    try {
                        callback(std::move(result));
                    } catch (...) {
                    }
                });
        } catch (...) {
            return false;
        }
        return true;
    }

    bool execute_async(ExecutionRequest request,
                       CompletionCallback callback) {
        if (!callback) return false;
        int failure_code = execution_failure ? execution_failure(request) : 0;
        std::lock_guard<std::mutex> lock(mutex);
        if (!initialized || closing || closed) return false;
        try {
            workers.emplace_back(
                [request = std::move(request),
                 callback = std::move(callback), failure_code]() mutable {
                    FpgaDriverResult result;
                    result.start_time_us = monotonic_time_us();
                    if (request.expected_exec_us > 0) {
                        std::this_thread::sleep_for(std::chrono::microseconds(
                            request.expected_exec_us));
                    }
                    result.end_time_us = monotonic_time_us();
                    result.success = failure_code == 0;
                    result.error_code = failure_code;
                    if (!result.success) {
                        result.error_message = "fake FPGA execution failure";
                    }
                    try {
                        callback(std::move(result));
                    } catch (...) {
                    }
                });
        } catch (...) {
            return false;
        }
        return true;
    }

    bool reset_rp(int resource_id, int rp_id) {
        std::lock_guard<std::mutex> lock(mutex);
        return initialized && !closing && !closed && resource_id >= 0 &&
               rp_id >= 0;
    }

    void shutdown() {
        std::vector<std::thread> pending_workers;
        {
            std::lock_guard<std::mutex> lock(mutex);
            if (closed) return;
            closing = true;
            pending_workers.swap(workers);
        }
        for (auto& worker : pending_workers) {
            if (worker.joinable()) worker.join();
        }
        std::lock_guard<std::mutex> lock(mutex);
        initialized = false;
        closing = false;
        closed = true;
    }

private:
    ConfigurationFailureInjector configuration_failure;
    ExecutionFailureInjector execution_failure;
    std::mutex mutex;
    std::vector<std::thread> workers;
    bool initialized{false};
    bool closing{false};
    bool closed{false};
};

FakeFpgaDeviceDriver::FakeFpgaDeviceDriver(
    ConfigurationFailureInjector configuration_failure,
    ExecutionFailureInjector execution_failure)
    : impl_(std::make_unique<Impl>(std::move(configuration_failure),
                                  std::move(execution_failure))) {}

FakeFpgaDeviceDriver::~FakeFpgaDeviceDriver() {
    shutdown();
}

bool FakeFpgaDeviceDriver::initialize() {
    return impl_->initialize();
}

bool FakeFpgaDeviceDriver::configure_async(
    ReconfigurationRequest request, CompletionCallback callback) {
    return impl_->configure_async(std::move(request), std::move(callback));
}

bool FakeFpgaDeviceDriver::execute_async(
    ExecutionRequest request, CompletionCallback callback) {
    return impl_->execute_async(std::move(request), std::move(callback));
}

bool FakeFpgaDeviceDriver::reset_rp(int resource_id, int rp_id) {
    return impl_->reset_rp(resource_id, rp_id);
}

void FakeFpgaDeviceDriver::shutdown() {
    impl_->shutdown();
}
