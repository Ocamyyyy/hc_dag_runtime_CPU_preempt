#pragma once

#include "fpga_types.h"

#include <functional>
#include <memory>

struct FpgaDriverResult {
    bool success{false};
    int64_t start_time_us{0};
    int64_t end_time_us{0};
    int error_code{0};
    std::string error_message;
};

class IFpgaDeviceDriver {
public:
    using CompletionCallback = std::function<void(FpgaDriverResult)>;

    virtual ~IFpgaDeviceDriver() = default;
    virtual bool initialize() = 0;
    virtual bool configure_async(ReconfigurationRequest request,
                                 CompletionCallback callback) = 0;
    virtual bool execute_async(ExecutionRequest request,
                               CompletionCallback callback) = 0;
    virtual bool reset_rp(int resource_id, int rp_id) = 0;
    virtual void shutdown() = 0;
};

class FakeFpgaDeviceDriver final : public IFpgaDeviceDriver {
public:
    using ConfigurationFailureInjector =
        std::function<int(const ReconfigurationRequest&)>;
    using ExecutionFailureInjector =
        std::function<int(const ExecutionRequest&)>;

    explicit FakeFpgaDeviceDriver(
        ConfigurationFailureInjector configuration_failure = {},
        ExecutionFailureInjector execution_failure = {});
    ~FakeFpgaDeviceDriver() override;

    FakeFpgaDeviceDriver(const FakeFpgaDeviceDriver&) = delete;
    FakeFpgaDeviceDriver& operator=(const FakeFpgaDeviceDriver&) = delete;

    bool initialize() override;
    bool configure_async(ReconfigurationRequest request,
                         CompletionCallback callback) override;
    bool execute_async(ExecutionRequest request,
                       CompletionCallback callback) override;
    bool reset_rp(int resource_id, int rp_id) override;
    void shutdown() override;

private:
    class Impl;
    std::unique_ptr<Impl> impl_;
};
