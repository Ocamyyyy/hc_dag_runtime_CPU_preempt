#pragma once

#include "backend.h"

#include <cstdint>
#include <string>

enum class FpgaOperationDomain {
    CONFIGURATION,
    EXECUTION
};

enum class FpgaEventType {
    CONFIG,
    SUBMIT,
    COMPLETE,
    FAIL
};

enum class RpState {
    UNCONFIGURED,
    RECONFIGURING,
    READY,
    EXECUTING,
    ERROR
};

enum class RpTransitionStatus {
    OK,
    UNKNOWN_RP,
    UNSUPPORTED_MAPPING,
    BUSY,
    NOT_READY,
    CONFIGURATION_MISMATCH,
    ERROR_STATE,
    INVALID_STATE
};

enum class ReconfigurationSubmitStatus {
    ACCEPTED,
    NOT_INITIALIZED,
    INVALID_REQUEST,
    SHUTTING_DOWN,
    CLOSED,
    CONFIGURATION_PORT_BUSY,
    RP_BUSY,
    UNSUPPORTED_MAPPING,
    RP_ERROR,
    DRIVER_REJECTED
};

enum class ReconfigurationStatus {
    SUCCEEDED,
    FAILED,
    CANCELLED
};

enum class FpgaErrorCode {
    NONE = 0,
    INVALID_REQUEST = 1000,
    UNKNOWN_RP = 1001,
    UNSUPPORTED_MAPPING = 1002,
    RP_BUSY = 1003,
    RP_NOT_READY = 1004,
    CONFIGURATION_MISMATCH = 1005,
    RP_ERROR = 1006,
    DRIVER_CONFIG_FAILED = 1100,
    DRIVER_EXECUTION_FAILED = 1101,
    DRIVER_REJECTED = 1102
};

struct RpCompatibilityEntry {
    int resource_id{-1};
    int rp_id{-1};
    int function_id{-1};
    int bitstream_id{-1};
};

struct RpStateSnapshot {
    int resource_id{-1};
    int rp_id{-1};
    RpState state{RpState::UNCONFIGURED};
    int current_function_id{-1};
    int current_bitstream_id{-1};
};

struct ReconfigurationRequest {
    uint64_t configuration_request_id{0};
    int resource_id{-1};
    int rp_id{-1};
    int function_id{-1};
    int bitstream_id{-1};
    int64_t expected_config_us{0};
};

struct ReconfigurationResult {
    uint64_t configuration_request_id{0};
    int resource_id{-1};
    int rp_id{-1};
    int function_id{-1};
    int bitstream_id{-1};
    ReconfigurationStatus status{ReconfigurationStatus::FAILED};
    int64_t start_time_us{0};
    int64_t end_time_us{0};
    int64_t actual_config_us{0};
    int error_code{0};
    std::string error_message;
};

struct FpgaEvent {
    FpgaEventType type{FpgaEventType::FAIL};
    FpgaOperationDomain domain{FpgaOperationDomain::EXECUTION};
    uint64_t request_id{0};
    int resource_id{-1};
    int rp_id{-1};
    int function_id{-1};
    int bitstream_id{-1};
    int64_t start_time_us{0};
    int64_t end_time_us{0};
    int error_code{0};
};
