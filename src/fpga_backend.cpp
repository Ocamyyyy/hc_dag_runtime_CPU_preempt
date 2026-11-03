#include "fpga_backend.h"

#include <atomic>
#include <mutex>
#include <utility>

namespace {

SubmitStatus execution_submit_status(RpTransitionStatus status) {
    switch (status) {
        case RpTransitionStatus::OK:
            return SubmitStatus::ACCEPTED;
        case RpTransitionStatus::BUSY:
            return SubmitStatus::RESOURCE_BUSY;
        case RpTransitionStatus::CONFIGURATION_MISMATCH:
        case RpTransitionStatus::NOT_READY:
            return SubmitStatus::CONFIGURATION_MISMATCH;
        case RpTransitionStatus::ERROR_STATE:
            return SubmitStatus::RP_ERROR;
        case RpTransitionStatus::UNKNOWN_RP:
        case RpTransitionStatus::UNSUPPORTED_MAPPING:
            return SubmitStatus::UNSUPPORTED_TARGET;
        case RpTransitionStatus::INVALID_STATE:
            return SubmitStatus::INVALID_REQUEST;
    }
    return SubmitStatus::INVALID_REQUEST;
}

ReconfigurationSubmitStatus configuration_submit_status(
    RpTransitionStatus status) {
    switch (status) {
        case RpTransitionStatus::OK:
            return ReconfigurationSubmitStatus::ACCEPTED;
        case RpTransitionStatus::BUSY:
            return ReconfigurationSubmitStatus::RP_BUSY;
        case RpTransitionStatus::UNKNOWN_RP:
        case RpTransitionStatus::UNSUPPORTED_MAPPING:
            return ReconfigurationSubmitStatus::UNSUPPORTED_MAPPING;
        case RpTransitionStatus::ERROR_STATE:
            return ReconfigurationSubmitStatus::RP_ERROR;
        case RpTransitionStatus::NOT_READY:
        case RpTransitionStatus::CONFIGURATION_MISMATCH:
        case RpTransitionStatus::INVALID_STATE:
            return ReconfigurationSubmitStatus::INVALID_REQUEST;
    }
    return ReconfigurationSubmitStatus::INVALID_REQUEST;
}

}

class FpgaBackend::Impl {
public:
    Impl(std::shared_ptr<IFpgaDeviceDriver> device_driver,
         std::vector<RpCompatibilityEntry> compatibility,
         EventCallback callback)
        : driver(std::move(device_driver)),
          state_manager(std::move(compatibility)),
          event_callback(std::move(callback)) {}

    bool initialize() {
        std::lock_guard<std::mutex> lock(lifecycle_mutex);
        if (closed || !driver) return false;
        if (initialized) return true;
        initialized = driver->initialize();
        return initialized;
    }

    SubmitStatus submit(ExecutionRequest request,
                        CompletionCallback callback) {
        if (!callback || request.request_id == 0 || request.job_id.empty() ||
            request.node_id.empty() ||
            request.target.node_type != NodeType::FPGA ||
            request.target.resource_id < 0 || request.target.rp_id < 0 ||
            request.target.function_id < 0 ||
            request.target.configuration_id < 0 ||
            request.expected_exec_us < 0) {
            return SubmitStatus::INVALID_REQUEST;
        }

        std::lock_guard<std::mutex> lock(lifecycle_mutex);
        if (closed) return SubmitStatus::CLOSED;
        if (!initialized) return SubmitStatus::NOT_INITIALIZED;
        if (closing) return SubmitStatus::SHUTTING_DOWN;

        RpTransitionStatus transition =
            state_manager.begin_execution(request.target);
        SubmitStatus status = execution_submit_status(transition);
        if (status != SubmitStatus::ACCEPTED) return status;

        emit({FpgaEventType::SUBMIT,
              FpgaOperationDomain::EXECUTION,
              request.request_id,
              request.target.resource_id,
              request.target.rp_id,
              request.target.function_id,
              request.target.configuration_id});

        const ExecutionTarget rejected_target = request.target;
        const uint64_t rejected_request_id = request.request_id;
        ExecutionRequest result_identity = request;
        bool driver_accepted = driver->execute_async(
            std::move(request),
            [this, identity = std::move(result_identity),
             callback = std::move(callback)](FpgaDriverResult driver_result) mutable {
                state_manager.finish_execution(
                    identity.target.resource_id, identity.target.rp_id,
                    driver_result.success);

                ExecutionResult result;
                result.request_id = identity.request_id;
                result.job_id = identity.job_id;
                result.node_id = identity.node_id;
                result.actual_target = identity.target;
                result.status = driver_result.success
                                    ? ExecutionStatus::SUCCEEDED
                                    : ExecutionStatus::FAILED;
                result.start_time_us = driver_result.start_time_us;
                result.end_time_us = driver_result.end_time_us;
                result.actual_exec_us = driver_result.end_time_us -
                                        driver_result.start_time_us;
                result.error_code =
                    !driver_result.success && driver_result.error_code == 0
                        ? static_cast<int>(
                              FpgaErrorCode::DRIVER_EXECUTION_FAILED)
                        : driver_result.error_code;
                result.error_message = driver_result.error_message;

                emit({driver_result.success ? FpgaEventType::COMPLETE
                                            : FpgaEventType::FAIL,
                      FpgaOperationDomain::EXECUTION,
                      identity.request_id,
                      identity.target.resource_id,
                      identity.target.rp_id,
                      identity.target.function_id,
                      identity.target.configuration_id,
                      driver_result.start_time_us,
                      driver_result.end_time_us,
                      result.error_code});
                callback(std::move(result));
            });
        if (!driver_accepted) {
            state_manager.finish_execution(rejected_target.resource_id,
                                           rejected_target.rp_id, false);
            emit({FpgaEventType::FAIL,
                  FpgaOperationDomain::EXECUTION,
                  rejected_request_id,
                  rejected_target.resource_id,
                  rejected_target.rp_id,
                  rejected_target.function_id,
                  rejected_target.configuration_id,
                  0,
                  0,
                  static_cast<int>(FpgaErrorCode::DRIVER_REJECTED)});
            return SubmitStatus::SHUTTING_DOWN;
        }
        return SubmitStatus::ACCEPTED;
    }

    ReconfigurationSubmitStatus configure(
        ReconfigurationRequest request,
        ReconfigurationCallback callback) {
        if (!callback || request.configuration_request_id == 0 ||
            request.resource_id < 0 || request.rp_id < 0 ||
            request.function_id < 0 || request.bitstream_id < 0 ||
            request.expected_config_us < 0) {
            return ReconfigurationSubmitStatus::INVALID_REQUEST;
        }

        std::lock_guard<std::mutex> lock(lifecycle_mutex);
        if (closed) return ReconfigurationSubmitStatus::CLOSED;
        if (!initialized) {
            return ReconfigurationSubmitStatus::NOT_INITIALIZED;
        }
        if (closing) return ReconfigurationSubmitStatus::SHUTTING_DOWN;

        bool expected = false;
        if (!configuration_port_busy.compare_exchange_strong(expected, true)) {
            return ReconfigurationSubmitStatus::CONFIGURATION_PORT_BUSY;
        }

        RpTransitionStatus transition =
            state_manager.begin_reconfiguration(request);
        ReconfigurationSubmitStatus status =
            configuration_submit_status(transition);
        if (status != ReconfigurationSubmitStatus::ACCEPTED) {
            configuration_port_busy.store(false);
            return status;
        }

        emit({FpgaEventType::CONFIG,
              FpgaOperationDomain::CONFIGURATION,
              request.configuration_request_id,
              request.resource_id,
              request.rp_id,
              request.function_id,
              request.bitstream_id});

        const int rejected_resource_id = request.resource_id;
        const int rejected_rp_id = request.rp_id;
        const int rejected_function_id = request.function_id;
        const int rejected_bitstream_id = request.bitstream_id;
        const uint64_t rejected_request_id =
            request.configuration_request_id;
        ReconfigurationRequest result_identity = request;
        bool driver_accepted = driver->configure_async(
            std::move(request),
            [this, identity = std::move(result_identity),
             callback = std::move(callback)](FpgaDriverResult driver_result) mutable {
                state_manager.finish_reconfiguration(
                    identity.resource_id, identity.rp_id,
                    driver_result.success);
                configuration_port_busy.store(false);

                ReconfigurationResult result;
                result.configuration_request_id =
                    identity.configuration_request_id;
                result.resource_id = identity.resource_id;
                result.rp_id = identity.rp_id;
                result.function_id = identity.function_id;
                result.bitstream_id = identity.bitstream_id;
                result.status = driver_result.success
                                    ? ReconfigurationStatus::SUCCEEDED
                                    : ReconfigurationStatus::FAILED;
                result.start_time_us = driver_result.start_time_us;
                result.end_time_us = driver_result.end_time_us;
                result.actual_config_us = driver_result.end_time_us -
                                          driver_result.start_time_us;
                result.error_code =
                    !driver_result.success && driver_result.error_code == 0
                        ? static_cast<int>(FpgaErrorCode::DRIVER_CONFIG_FAILED)
                        : driver_result.error_code;
                result.error_message = driver_result.error_message;

                emit({driver_result.success ? FpgaEventType::COMPLETE
                                            : FpgaEventType::FAIL,
                      FpgaOperationDomain::CONFIGURATION,
                      identity.configuration_request_id,
                      identity.resource_id,
                      identity.rp_id,
                      identity.function_id,
                      identity.bitstream_id,
                      driver_result.start_time_us,
                      driver_result.end_time_us,
                      result.error_code});
                callback(std::move(result));
            });
        if (!driver_accepted) {
            state_manager.finish_reconfiguration(
                rejected_resource_id, rejected_rp_id, false);
            configuration_port_busy.store(false);
            emit({FpgaEventType::FAIL,
                  FpgaOperationDomain::CONFIGURATION,
                  rejected_request_id,
                  rejected_resource_id,
                  rejected_rp_id,
                  rejected_function_id,
                  rejected_bitstream_id,
                  0,
                  0,
                  static_cast<int>(FpgaErrorCode::DRIVER_REJECTED)});
            return ReconfigurationSubmitStatus::DRIVER_REJECTED;
        }
        return ReconfigurationSubmitStatus::ACCEPTED;
    }

    std::optional<RpStateSnapshot> get_rp_state(int resource_id,
                                                int rp_id) const {
        if (!state_manager.has_rp(resource_id, rp_id)) return std::nullopt;
        return state_manager.snapshot(resource_id, rp_id);
    }

    bool recover_rp(int resource_id, int rp_id) {
        std::lock_guard<std::mutex> lock(lifecycle_mutex);
        if (!initialized || closing || closed) return false;
        auto snapshot = state_manager.snapshot(resource_id, rp_id);
        if (snapshot.state != RpState::ERROR) return false;
        if (!driver->reset_rp(resource_id, rp_id)) return false;
        return state_manager.recover(resource_id, rp_id) ==
               RpTransitionStatus::OK;
    }

    void shutdown() {
        {
            std::lock_guard<std::mutex> lock(lifecycle_mutex);
            if (closed) return;
            closing = true;
        }
        if (driver) driver->shutdown();
        std::lock_guard<std::mutex> lock(lifecycle_mutex);
        initialized = false;
        closing = false;
        closed = true;
        configuration_port_busy.store(false);
    }

private:
    void emit(FpgaEvent event) const {
        if (!event_callback) return;
        try {
            event_callback(std::move(event));
        } catch (...) {
        }
    }

    std::shared_ptr<IFpgaDeviceDriver> driver;
    RPStateManager state_manager;
    EventCallback event_callback;
    mutable std::mutex lifecycle_mutex;
    std::atomic<bool> configuration_port_busy{false};
    bool initialized{false};
    bool closing{false};
    bool closed{false};
};

FpgaBackend::FpgaBackend(
    std::shared_ptr<IFpgaDeviceDriver> driver,
    std::vector<RpCompatibilityEntry> compatibility,
    EventCallback event_callback)
    : impl_(std::make_unique<Impl>(std::move(driver),
                                  std::move(compatibility),
                                  std::move(event_callback))) {}

FpgaBackend::~FpgaBackend() {
    shutdown();
}

bool FpgaBackend::initialize() {
    return impl_->initialize();
}

SubmitStatus FpgaBackend::submit(ExecutionRequest request,
                                 CompletionCallback on_complete) {
    return impl_->submit(std::move(request), std::move(on_complete));
}

void FpgaBackend::shutdown() {
    impl_->shutdown();
}

ReconfigurationSubmitStatus FpgaBackend::configure(
    ReconfigurationRequest request,
    ReconfigurationCallback on_complete) {
    return impl_->configure(std::move(request), std::move(on_complete));
}

std::optional<RpStateSnapshot> FpgaBackend::get_rp_state(
    int resource_id, int rp_id) const {
    return impl_->get_rp_state(resource_id, rp_id);
}

bool FpgaBackend::recover_rp(int resource_id, int rp_id) {
    return impl_->recover_rp(resource_id, rp_id);
}
