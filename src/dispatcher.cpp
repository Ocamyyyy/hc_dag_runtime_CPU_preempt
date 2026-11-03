// src/dispatcher.cpp
#include "dispatcher.h"
#include <iostream>
#include <sched.h>
#include <pthread.h>
#include <chrono>

static int64_t get_time_us() {
    using namespace std::chrono;
    return duration_cast<microseconds>(
        steady_clock::now().time_since_epoch()).count();
}

static void bind_to_core(int core) {
    cpu_set_t cs; CPU_ZERO(&cs); CPU_SET(core, &cs);
    pthread_setaffinity_np(pthread_self(), sizeof(cs), &cs);
}

// ============================================================
// CpuWorker
// ============================================================
CpuWorker::CpuWorker(int cpu_core, RuntimeManager* mgr)
    : cpu_core_(cpu_core), mgr_(mgr) {}

CpuWorker::~CpuWorker() { stop(); }

void CpuWorker::start() {
    running_ = true;
    thread_ = std::thread([this]{ worker_loop(); });
}

void CpuWorker::stop() {
    if (!running_.exchange(false)) return;
    q_cv_.notify_all();
    if (thread_.joinable()) thread_.join();
}

void CpuWorker::submit(NodeInstance* ni) {
    WorkerRequest req;
    req.node_inst       = ni;
    req.task_priority   = ni->trace.task_priority;
    req.enqueue_time_us = get_time_us();
    { std::lock_guard<std::mutex> lk(q_mutex_);
      queue_.push(std::move(req)); }
    q_cv_.notify_one();
}

void CpuWorker::worker_loop() {
    // 启动时绑核一次，之后永远在这个核上
    bind_to_core(cpu_core_);
    std::cout << "[CpuWorker] core" << cpu_core_ << " ready\n";

    while (true) {
        WorkerRequest req;
        {
            std::unique_lock<std::mutex> lk(q_mutex_);
            q_cv_.wait(lk, [this]{
                return !queue_.empty() || !running_.load();
            });
            if (!running_.load() && queue_.empty()) break;
            req = std::move(const_cast<WorkerRequest&>(queue_.top()));
            queue_.pop();
        }

        is_idle_.store(false);
        NodeInstance* ni = req.node_inst;
        ni->state.store(NodeState::PRE_EXEC);
        ni->trace.t_pre_start_us = get_time_us();

        std::this_thread::sleep_for(
            std::chrono::microseconds(ni->tmpl->wcet_us));

        int64_t t1 = get_time_us();
        ni->trace.t_pre_end_us = ni->trace.t_finish_us = t1;
        ni->state.store(NodeState::COMPLETED);

        std::cout << "[CpuWorker] core" << cpu_core_
                  << " DONE " << ni->job_id << "/" << ni->node_id
                  << " actual=" << (t1 - ni->trace.t_pre_start_us) << "us\n";

        // 先发 NODE_COMPLETE（依赖推进，新节点入 Scheduler 队列）
        // 再发 RESOURCE_IDLE（触发 try_dispatch 取下一个任务）
        // 顺序很重要：保证新节点入队在 try_dispatch 之前
        RuntimeEvent complete_event;
        complete_event.type = EventType::NODE_COMPLETE;
        complete_event.job_id = ni->job_id;
        complete_event.node_id = ni->node_id;
        complete_event.timestamp_us = mgr_->now_us();
        mgr_->post_event(std::move(complete_event));

        RuntimeEvent idle_event;
        idle_event.type = EventType::RESOURCE_IDLE;
        idle_event.timestamp_us = mgr_->now_us();
        mgr_->post_event(std::move(idle_event));

        is_idle_.store(true);
    }
    std::cout << "[CpuWorker] core" << cpu_core_ << " exiting\n";
}

// ============================================================
// Dispatcher
// ============================================================
Dispatcher::Dispatcher(RuntimeManager* mgr) : mgr_(mgr) {}

void Dispatcher::register_cpu_worker(int core, std::shared_ptr<CpuWorker> w) {
    cpu_workers_[core] = std::move(w);
}

void Dispatcher::register_execution_backend(
    int resource_id, std::shared_ptr<ExecutionBackend> backend) {
    backends_[resource_id] = std::move(backend);
}

BackendSubmission Dispatcher::submit_fpga(NodeInstance* ni) {
    const int resource_id = ni->tmpl->assigned_resource_id;
    auto backend_it = backends_.find(resource_id);
    if (backend_it == backends_.end() || !backend_it->second) {
        return {SubmitStatus::NOT_INITIALIZED, 0};
    }

    ExecutionRequest request;
    request.request_id = next_request_id_.fetch_add(1);
    request.job_id = ni->job_id;
    request.node_id = ni->node_id;
    request.target.node_type = NodeType::FPGA;
    request.target.resource_id = resource_id;
    request.target.cpu_core = ni->tmpl->assigned_cpu_core;
    request.target.rp_id = ni->tmpl->assigned_region_id;
    request.target.function_id = ni->tmpl->function_id;
    request.target.configuration_id = ni->tmpl->configuration_id;
    request.expected_exec_us = ni->tmpl->accel_exec_us > 0
                                   ? ni->tmpl->accel_exec_us
                                   : ni->tmpl->wcet_us;

    const uint64_t request_id = request.request_id;
    SubmitStatus status = backend_it->second->submit(
        std::move(request),
        [manager = mgr_](ExecutionResult result) {
            RuntimeEvent event;
            event.type = EventType::NODE_COMPLETE;
            event.job_id = result.job_id;
            event.node_id = result.node_id;
            event.timestamp_us = result.end_time_us;
            event.execution_result = std::move(result);
            manager->post_event(std::move(event));
        });

    if (status == SubmitStatus::ACCEPTED) {
        std::cout << "[Dispatcher] FPGA SUBMIT request=" << request_id
                  << " node=" << ni->job_id << "/" << ni->node_id
                  << " resource=" << resource_id
                  << " rp=" << ni->tmpl->assigned_region_id
                  << " function=" << ni->tmpl->function_id
                  << " configuration=" << ni->tmpl->configuration_id << "\n";
    }
    return {status, request_id};
}
