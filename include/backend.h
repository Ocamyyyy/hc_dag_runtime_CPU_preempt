#pragma once

#include "task_model.h"

#include <cstdint>
#include <functional>
#include <memory>
#include <string>

enum class ExecutionStatus {
    SUCCEEDED,
    FAILED,
    CANCELLED
};

enum class SubmitStatus {
    ACCEPTED,
    NOT_INITIALIZED,
    INVALID_REQUEST,
    SHUTTING_DOWN,
    CLOSED,
    RESOURCE_BUSY,
    UNSUPPORTED_TARGET,
    CONFIGURATION_MISMATCH,
    RP_ERROR
};

struct ExecutionTarget {
    NodeType node_type{NodeType::UNKNOWN};
    int resource_id{-1};
    int cpu_core{-1};
    int rp_id{-1};
    int function_id{-1};
    int configuration_id{-1};
};

struct ExecutionPayload {
    virtual ~ExecutionPayload() = default;
};

struct ExecutionRequest {
    uint64_t request_id{0};
    std::string job_id;
    std::string node_id;
    ExecutionTarget target;
    int64_t expected_exec_us{0};
    std::shared_ptr<const ExecutionPayload> payload;
};

struct ExecutionResult {
    uint64_t request_id{0};
    std::string job_id;
    std::string node_id;
    ExecutionTarget actual_target;
    ExecutionStatus status{ExecutionStatus::FAILED};
    int64_t start_time_us{0};
    int64_t end_time_us{0};
    int64_t actual_exec_us{0};
    int error_code{0};
    std::string error_message;
};

class ExecutionBackend {
public:
    using CompletionCallback = std::function<void(ExecutionResult)>;

    virtual ~ExecutionBackend() = default;

    virtual bool initialize() = 0;
    virtual SubmitStatus submit(ExecutionRequest request,
                                CompletionCallback on_complete) = 0;
    virtual void shutdown() = 0;

    virtual std::string backend_name() const = 0;
    virtual bool is_preemptable() const = 0;
};

using Backend = ExecutionBackend;

class MockBackend final : public ExecutionBackend {
public:
    using FailureInjector = std::function<int(const ExecutionRequest&)>;

    explicit MockBackend(FailureInjector failure_injector = {});
    ~MockBackend() override;

    MockBackend(const MockBackend&) = delete;
    MockBackend& operator=(const MockBackend&) = delete;

    bool initialize() override;
    SubmitStatus submit(ExecutionRequest request,
                        CompletionCallback on_complete) override;
    void shutdown() override;

    std::string backend_name() const override { return "MockBackend"; }
    bool is_preemptable() const override { return false; }

private:
    class Impl;
    std::unique_ptr<Impl> impl_;
};

using MockGpuBackend = MockBackend;

class CpuBackend : public ExecutionBackend {
public:
    bool initialize() override;
    SubmitStatus submit(ExecutionRequest request,
                        CompletionCallback on_complete) override;
    void shutdown() override;
    std::string backend_name() const override { return "CpuBackend"; }
    bool is_preemptable() const override { return true; }
};

class OpenClGpuBackend : public ExecutionBackend {
public:
    explicit OpenClGpuBackend(int platform_index = 0, int device_index = 0);
    ~OpenClGpuBackend() override;

    bool initialize() override;
    SubmitStatus submit(ExecutionRequest request,
                        CompletionCallback on_complete) override;
    void shutdown() override;
    std::string backend_name() const override { return "OpenClGpuBackend"; }
    bool is_preemptable() const override { return false; }

    std::string get_platform_info() const;
    std::string get_device_info() const;

private:
    void* cl_platform_{nullptr};
    void* cl_device_{nullptr};
    void* cl_context_{nullptr};
    void* cl_queue_{nullptr};
    void* cl_program_{nullptr};
    int platform_index_;
    int device_index_;
    bool initialized_{false};
};

class FpgaBackend;

class BackendFactory {
public:
    enum class BackendType { CPU, MOCK_GPU, OPENCL_GPU, FPGA };

    static std::unique_ptr<ExecutionBackend> create(BackendType type);
};
