#pragma once

#include "fpga_types.h"

#include <cstddef>
#include <mutex>
#include <unordered_map>
#include <vector>

class RPStateManager {
public:
    explicit RPStateManager(std::vector<RpCompatibilityEntry> compatibility);

    bool is_compatible(int resource_id, int rp_id, int function_id,
                       int bitstream_id) const;
    bool has_rp(int resource_id, int rp_id) const;
    RpStateSnapshot snapshot(int resource_id, int rp_id) const;

    RpTransitionStatus begin_reconfiguration(
        const ReconfigurationRequest& request);
    RpTransitionStatus finish_reconfiguration(int resource_id, int rp_id,
                                              bool success);
    RpTransitionStatus begin_execution(const ExecutionTarget& target);
    RpTransitionStatus finish_execution(int resource_id, int rp_id,
                                       bool success);
    RpTransitionStatus recover(int resource_id, int rp_id);

private:
    struct RpKey {
        int resource_id;
        int rp_id;

        bool operator==(const RpKey& other) const {
            return resource_id == other.resource_id && rp_id == other.rp_id;
        }
    };

    struct RpKeyHash {
        std::size_t operator()(const RpKey& key) const;
    };

    struct RpRecord {
        RpState state{RpState::UNCONFIGURED};
        int current_function_id{-1};
        int current_bitstream_id{-1};
        int pending_function_id{-1};
        int pending_bitstream_id{-1};
    };

    bool is_compatible_locked(int resource_id, int rp_id, int function_id,
                              int bitstream_id) const;

    std::vector<RpCompatibilityEntry> compatibility_;
    std::unordered_map<RpKey, RpRecord, RpKeyHash> records_;
    mutable std::mutex mutex_;
};
