#include "rp_state_manager.h"

#include <functional>
#include <utility>

std::size_t RPStateManager::RpKeyHash::operator()(const RpKey& key) const {
    const std::size_t first = std::hash<int>{}(key.resource_id);
    const std::size_t second = std::hash<int>{}(key.rp_id);
    return first ^ (second + 0x9e3779b9U + (first << 6U) + (first >> 2U));
}

RPStateManager::RPStateManager(
    std::vector<RpCompatibilityEntry> compatibility)
    : compatibility_(std::move(compatibility)) {
    for (const auto& entry : compatibility_) {
        records_.try_emplace({entry.resource_id, entry.rp_id});
    }
}

bool RPStateManager::has_rp(int resource_id, int rp_id) const {
    std::lock_guard<std::mutex> lock(mutex_);
    return records_.find({resource_id, rp_id}) != records_.end();
}

bool RPStateManager::is_compatible(int resource_id, int rp_id,
                                   int function_id,
                                   int bitstream_id) const {
    std::lock_guard<std::mutex> lock(mutex_);
    return is_compatible_locked(resource_id, rp_id, function_id,
                                bitstream_id);
}

bool RPStateManager::is_compatible_locked(int resource_id, int rp_id,
                                          int function_id,
                                          int bitstream_id) const {
    for (const auto& entry : compatibility_) {
        if (entry.resource_id == resource_id && entry.rp_id == rp_id &&
            entry.function_id == function_id &&
            entry.bitstream_id == bitstream_id) {
            return true;
        }
    }
    return false;
}

RpStateSnapshot RPStateManager::snapshot(int resource_id, int rp_id) const {
    std::lock_guard<std::mutex> lock(mutex_);
    RpStateSnapshot result;
    result.resource_id = resource_id;
    result.rp_id = rp_id;
    auto it = records_.find({resource_id, rp_id});
    if (it == records_.end()) {
        result.state = RpState::ERROR;
        return result;
    }
    result.state = it->second.state;
    result.current_function_id = it->second.current_function_id;
    result.current_bitstream_id = it->second.current_bitstream_id;
    return result;
}

RpTransitionStatus RPStateManager::begin_reconfiguration(
    const ReconfigurationRequest& request) {
    std::lock_guard<std::mutex> lock(mutex_);
    auto it = records_.find({request.resource_id, request.rp_id});
    if (it == records_.end()) return RpTransitionStatus::UNKNOWN_RP;
    if (!is_compatible_locked(request.resource_id, request.rp_id,
                              request.function_id, request.bitstream_id)) {
        return RpTransitionStatus::UNSUPPORTED_MAPPING;
    }
    if (it->second.state == RpState::RECONFIGURING ||
        it->second.state == RpState::EXECUTING) {
        return RpTransitionStatus::BUSY;
    }
    if (it->second.state == RpState::ERROR) {
        return RpTransitionStatus::ERROR_STATE;
    }

    it->second.state = RpState::RECONFIGURING;
    it->second.pending_function_id = request.function_id;
    it->second.pending_bitstream_id = request.bitstream_id;
    return RpTransitionStatus::OK;
}

RpTransitionStatus RPStateManager::finish_reconfiguration(
    int resource_id, int rp_id, bool success) {
    std::lock_guard<std::mutex> lock(mutex_);
    auto it = records_.find({resource_id, rp_id});
    if (it == records_.end()) return RpTransitionStatus::UNKNOWN_RP;
    if (it->second.state != RpState::RECONFIGURING) {
        return RpTransitionStatus::INVALID_STATE;
    }

    if (success) {
        it->second.state = RpState::READY;
        it->second.current_function_id = it->second.pending_function_id;
        it->second.current_bitstream_id = it->second.pending_bitstream_id;
    } else {
        it->second.state = RpState::ERROR;
        it->second.current_function_id = -1;
        it->second.current_bitstream_id = -1;
    }
    it->second.pending_function_id = -1;
    it->second.pending_bitstream_id = -1;
    return RpTransitionStatus::OK;
}

RpTransitionStatus RPStateManager::begin_execution(
    const ExecutionTarget& target) {
    std::lock_guard<std::mutex> lock(mutex_);
    auto it = records_.find({target.resource_id, target.rp_id});
    if (it == records_.end()) return RpTransitionStatus::UNKNOWN_RP;
    if (!is_compatible_locked(target.resource_id, target.rp_id,
                              target.function_id,
                              target.configuration_id)) {
        return RpTransitionStatus::UNSUPPORTED_MAPPING;
    }
    if (it->second.state == RpState::RECONFIGURING ||
        it->second.state == RpState::EXECUTING) {
        return RpTransitionStatus::BUSY;
    }
    if (it->second.state == RpState::ERROR) {
        return RpTransitionStatus::ERROR_STATE;
    }
    if (it->second.state != RpState::READY) {
        return RpTransitionStatus::NOT_READY;
    }
    if (it->second.current_bitstream_id != target.configuration_id) {
        return RpTransitionStatus::CONFIGURATION_MISMATCH;
    }

    it->second.state = RpState::EXECUTING;
    return RpTransitionStatus::OK;
}

RpTransitionStatus RPStateManager::finish_execution(int resource_id,
                                                     int rp_id,
                                                     bool success) {
    std::lock_guard<std::mutex> lock(mutex_);
    auto it = records_.find({resource_id, rp_id});
    if (it == records_.end()) return RpTransitionStatus::UNKNOWN_RP;
    if (it->second.state != RpState::EXECUTING) {
        return RpTransitionStatus::INVALID_STATE;
    }
    it->second.state = success ? RpState::READY : RpState::ERROR;
    return RpTransitionStatus::OK;
}

RpTransitionStatus RPStateManager::recover(int resource_id, int rp_id) {
    std::lock_guard<std::mutex> lock(mutex_);
    auto it = records_.find({resource_id, rp_id});
    if (it == records_.end()) return RpTransitionStatus::UNKNOWN_RP;
    if (it->second.state != RpState::ERROR) {
        return RpTransitionStatus::INVALID_STATE;
    }
    it->second.state = RpState::UNCONFIGURED;
    it->second.current_function_id = -1;
    it->second.current_bitstream_id = -1;
    it->second.pending_function_id = -1;
    it->second.pending_bitstream_id = -1;
    return RpTransitionStatus::OK;
}
