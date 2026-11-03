#include "backend.h"
#include "dispatcher.h"
#include "runtime_manager.h"
#include "scheduler.h"

#include <chrono>
#include <iostream>
#include <memory>
#include <mutex>
#include <optional>
#include <thread>

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

class DuplicateCompletionBackend final : public ExecutionBackend {
public:
    explicit DuplicateCompletionBackend(MockBackend::FailureInjector injector)
        : mock_(std::move(injector)) {}

    bool initialize() override { return mock_.initialize(); }

    SubmitStatus submit(ExecutionRequest request,
                        CompletionCallback callback) override {
        return mock_.submit(
            std::move(request),
            [callback = std::move(callback)](ExecutionResult result) mutable {
                ExecutionResult duplicate = result;
                callback(std::move(result));
                callback(std::move(duplicate));
            });
    }

    void shutdown() override { mock_.shutdown(); }
    std::string backend_name() const override {
        return "DuplicateCompletionBackend";
    }
    bool is_preemptable() const override { return false; }

private:
    MockBackend mock_;
};

TaskTemplate build_cpu_fpga_cpu_task() {
    TaskTemplate task;
    task.task_id = "CPU_FPGA_CPU";
    task.task_priority = 1;
    task.criticality = Criticality::HI;
    task.period_us = 100000;
    task.deadline_us = 100000;

    NodeTemplate a1{};
    a1.node_id = "A1";
    a1.task_id = task.task_id;
    a1.node_type = NodeType::CPU;
    a1.assigned_cpu_core = 1;
    a1.assigned_resource_id = 1;
    a1.wcet_us = 1000;
    a1.pre_exec_us = 0;
    a1.accel_exec_us = 0;
    a1.post_exec_us = 0;
    a1.successors = {"A2"};

    NodeTemplate a2{};
    a2.node_id = "A2";
    a2.task_id = task.task_id;
    a2.node_type = NodeType::FPGA;
    a2.assigned_cpu_core = 1;
    a2.assigned_resource_id = 20;
    a2.assigned_region_id = 10;
    a2.function_id = 1;
    a2.configuration_id = 1;
    a2.wcet_us = 2000;
    a2.pre_exec_us = 0;
    a2.accel_exec_us = 2000;
    a2.post_exec_us = 0;
    a2.predecessors = {"A1"};
    a2.successors = {"A3"};

    NodeTemplate a3{};
    a3.node_id = "A3";
    a3.task_id = task.task_id;
    a3.node_type = NodeType::CPU;
    a3.assigned_cpu_core = 1;
    a3.assigned_resource_id = 1;
    a3.wcet_us = 1000;
    a3.pre_exec_us = 0;
    a3.accel_exec_us = 0;
    a3.post_exec_us = 0;
    a3.predecessors = {"A2"};

    task.nodes = {a1, a2, a3};
    task.edges = {{"A1", "A2"}, {"A2", "A3"}};
    return task;
}

std::optional<RuntimeJobOutcome> wait_for_outcome(RuntimeManager& manager,
                                                  const std::string& job_id) {
    const auto deadline = std::chrono::steady_clock::now() +
                          std::chrono::seconds(2);
    while (std::chrono::steady_clock::now() < deadline) {
        auto outcome = manager.get_job_outcome(job_id);
        if (outcome.has_value()) return outcome;
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }
    return std::nullopt;
}

void stop_runtime(const std::shared_ptr<ExecutionBackend>& backend,
                  const std::shared_ptr<CpuWorker>& worker,
                  RuntimeManager& manager) {
    backend->shutdown();
    worker->stop();
    manager.stop();
}

void test_successful_cpu_fpga_cpu_chain() {
    std::cout << "[Integration] success scenario\n";
    TaskTemplate task = build_cpu_fpga_cpu_task();
    std::mutex captured_mutex;
    std::optional<ExecutionRequest> captured_request;
    auto backend = std::make_shared<DuplicateCompletionBackend>(
        [&](const ExecutionRequest& request) {
            std::lock_guard<std::mutex> lock(captured_mutex);
            captured_request = request;
            return 0;
        });
    CHECK(backend->initialize());

    RuntimeManager manager(0);
    auto scheduler = std::make_shared<BaselineScheduler>();
    auto dispatcher = std::make_shared<Dispatcher>(&manager);
    auto worker = std::make_shared<CpuWorker>(1, &manager);
    dispatcher->register_cpu_worker(1, worker);
    dispatcher->register_execution_backend(20, backend);
    manager.set_scheduler(scheduler);
    manager.set_dispatcher(dispatcher);
    manager.start();
    worker->start();

    RuntimeEvent success_release;
    success_release.type = EventType::JOB_RELEASE;
    success_release.job_id = "success-job";
    success_release.task_tmpl = &task;
    manager.post_event(std::move(success_release));
    auto outcome = wait_for_outcome(manager, "success-job");
    CHECK(outcome.has_value());

    if (outcome.has_value()) {
        CHECK(outcome->state == JobState::COMPLETED);
        CHECK(outcome->completed_nodes == 3);
        CHECK(outcome->total_nodes == 3);
        CHECK(outcome->node_traces.at("A1").final_state ==
              NodeState::COMPLETED);
        CHECK(outcome->node_traces.at("A2").final_state ==
              NodeState::COMPLETED);
        CHECK(outcome->node_traces.at("A3").final_state ==
              NodeState::COMPLETED);
        CHECK(outcome->node_traces.at("A3").t_pre_start_us >=
              outcome->node_traces.at("A2").t_accel_end_us);
        CHECK(outcome->node_traces.at("A2").accel_exec_us > 0);
    }

    {
        std::lock_guard<std::mutex> lock(captured_mutex);
        CHECK(captured_request.has_value());
        if (captured_request.has_value()) {
            CHECK(captured_request->job_id == "success-job");
            CHECK(captured_request->node_id == "A2");
            CHECK(captured_request->target.resource_id == 20);
            CHECK(captured_request->target.rp_id == 10);
            CHECK(captured_request->target.function_id == 1);
            CHECK(captured_request->target.configuration_id == 1);
        }
    }

    stop_runtime(backend, worker, manager);
}

void test_failed_fpga_stops_dag() {
    std::cout << "[Integration] failure scenario\n";
    TaskTemplate task = build_cpu_fpga_cpu_task();
    auto backend = std::make_shared<MockBackend>(
        [](const ExecutionRequest&) { return 77; });
    CHECK(backend->initialize());

    RuntimeManager manager(0);
    auto scheduler = std::make_shared<BaselineScheduler>();
    auto dispatcher = std::make_shared<Dispatcher>(&manager);
    auto worker = std::make_shared<CpuWorker>(1, &manager);
    dispatcher->register_cpu_worker(1, worker);
    dispatcher->register_execution_backend(20, backend);
    manager.set_scheduler(scheduler);
    manager.set_dispatcher(dispatcher);
    manager.start();
    worker->start();

    RuntimeEvent failure_release;
    failure_release.type = EventType::JOB_RELEASE;
    failure_release.job_id = "failure-job";
    failure_release.task_tmpl = &task;
    manager.post_event(std::move(failure_release));
    auto outcome = wait_for_outcome(manager, "failure-job");
    CHECK(outcome.has_value());

    if (outcome.has_value()) {
        CHECK(outcome->state == JobState::FAILED);
        CHECK(outcome->completed_nodes == 1);
        CHECK(outcome->total_nodes == 3);
        CHECK(outcome->node_traces.at("A1").final_state ==
              NodeState::COMPLETED);
        CHECK(outcome->node_traces.at("A2").final_state ==
              NodeState::FAILED);
        CHECK(outcome->node_traces.at("A3").final_state ==
              NodeState::PENDING);
        CHECK(outcome->node_traces.at("A3").t_pre_start_us == -1);
    }

    stop_runtime(backend, worker, manager);
}

}

int main() {
    test_successful_cpu_fpga_cpu_chain();
    test_failed_fpga_stops_dag();

    if (failures != 0) {
        std::cerr << failures << " check(s) failed\n";
        return 1;
    }
    std::cout << "CPU->FPGA->CPU integration tests passed\n";
    return 0;
}
