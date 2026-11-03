#pragma once

#include "backend.h"
#include "fpga_device_driver.h"
#include "rp_state_manager.h"

#include <functional>
#include <memory>
#include <optional>
#include <vector>

class FpgaBackend final : public ExecutionBackend {
public:
    using ReconfigurationCallback =
        std::function<void(ReconfigurationResult)>;
    using EventCallback = std::function<void(FpgaEvent)>;

    FpgaBackend(std::shared_ptr<IFpgaDeviceDriver> driver,
                std::vector<RpCompatibilityEntry> compatibility,
                EventCallback event_callback = {});
    ~FpgaBackend() override;

    FpgaBackend(const FpgaBackend&) = delete;
    FpgaBackend& operator=(const FpgaBackend&) = delete;

    bool initialize() override;
    SubmitStatus submit(ExecutionRequest request,
                        CompletionCallback on_complete) override;
    void shutdown() override;
    std::string backend_name() const override { return "FpgaBackend"; }
    bool is_preemptable() const override { return false; }

    ReconfigurationSubmitStatus configure(
        ReconfigurationRequest request,
        ReconfigurationCallback on_complete);
    std::optional<RpStateSnapshot> get_rp_state(int resource_id,
                                               int rp_id) const;
    bool recover_rp(int resource_id, int rp_id);

private:
    class Impl;
    std::unique_ptr<Impl> impl_;
};
