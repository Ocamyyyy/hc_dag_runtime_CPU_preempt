// src/runtime_manager.cpp（Day5版）
#include "runtime_manager.h"
#include "scheduler.h"
#include "dispatcher.h"
#include <chrono>
#include <iostream>
#include <sstream>
#include <sched.h>
#include <pthread.h>

static int64_t get_time_us() {
    using namespace std::chrono;
    return duration_cast<microseconds>(
        steady_clock::now().time_since_epoch()).count();
}

RuntimeManager::RuntimeManager(int core)
    : core_(core), start_time_us_(get_time_us()) {}

RuntimeManager::~RuntimeManager() { stop(); }

int64_t RuntimeManager::now_us() const {
    return get_time_us() - start_time_us_;
}

void RuntimeManager::set_scheduler(std::shared_ptr<BaselineScheduler> s) {
    scheduler_ = s;
}
void RuntimeManager::set_dispatcher(std::shared_ptr<Dispatcher> d) {
    dispatcher_ = d;
}

std::optional<RuntimeJobOutcome> RuntimeManager::get_job_outcome(
    const std::string& job_id) const {
    std::lock_guard<std::mutex> lock(jobs_mutex_);
    auto it = job_outcomes_.find(job_id);
    if (it == job_outcomes_.end()) return std::nullopt;
    return it->second;
}

void RuntimeManager::post_event(RuntimeEvent evt) {
    { std::lock_guard<std::mutex> lk(eq_mutex_);
      eq_.push(std::move(evt)); }
    eq_cv_.notify_one();
}

void RuntimeManager::start() {
    running_ = true;
    start_time_us_ = get_time_us();
    thread_ = std::thread([this]{ event_loop(); });
    cpu_set_t cs; CPU_ZERO(&cs); CPU_SET(core_, &cs);
    pthread_setaffinity_np(thread_.native_handle(), sizeof(cs), &cs);
    std::cout << "[RTMgr] Started on core" << core_ << "\n";
}

void RuntimeManager::stop() {
    if (!running_.exchange(false)) return;
    RuntimeEvent shutdown_event;
    shutdown_event.type = EventType::SHUTDOWN;
    post_event(std::move(shutdown_event));
    if (thread_.joinable()) thread_.join();
    std::cout << "[RTMgr] Stopped. released=" << jobs_released_
              << " completed=" << jobs_completed_
              << " failed=" << jobs_failed_ << "\n";
}

void RuntimeManager::event_loop() {
    while (true) {
        RuntimeEvent evt;
        { std::unique_lock<std::mutex> lk(eq_mutex_);
          eq_cv_.wait(lk, [this]{ return !eq_.empty(); });
          evt = std::move(eq_.front()); eq_.pop(); }

        if (evt.type == EventType::SHUTDOWN)       return;
        if (evt.type == EventType::JOB_RELEASE)    handle_job_release(evt);
        if (evt.type == EventType::NODE_COMPLETE)  handle_node_complete(evt);
        if (evt.type == EventType::RESOURCE_IDLE)  handle_resource_idle(evt);
    }
}

void RuntimeManager::enqueue_node(NodeInstance* ni) {
    // CPU 节点：入 Scheduler 对应核的队列，等 try_dispatch 取走
    // GPU 节点：Day6 加入，直接 submit 给 GpuOwnerWorker
    if (ni->tmpl->node_type == NodeType::CPU) {
        scheduler_->enqueue(ni);
        return;
    }
    if (ni->tmpl->node_type == NodeType::FPGA) {
        BackendSubmission submission = dispatcher_->submit_fpga(ni);
        if (submission.status == SubmitStatus::ACCEPTED) {
            ni->state.store(NodeState::DISPATCHED);
            ni->trace.t_dispatch_us = get_time_us();
            ni->trace.t_queue_enter_us = ni->trace.t_dispatch_us;
            std::lock_guard<std::mutex> lock(jobs_mutex_);
            in_flight_executions_.emplace(
                submission.request_id,
                InFlightExecution{ni->job_id, ni->node_id});
        } else {
            fail_job_submission(ni, submission.status);
        }
    }
}

void RuntimeManager::fail_job_submission(NodeInstance* ni,
                                         SubmitStatus status) {
    std::lock_guard<std::mutex> lock(jobs_mutex_);
    auto job_it = active_jobs_.find(ni->job_id);
    if (job_it == active_jobs_.end()) return;

    ni->state.store(NodeState::FAILED);
    ni->trace.final_state = NodeState::FAILED;
    ni->trace.t_finish_us = get_time_us();
    job_it->second->state.store(JobState::FAILED);
    save_job_outcome(*job_it->second, JobState::FAILED);
    jobs_failed_++;
    std::cout << "[RTMgr] JOB_FAILED " << ni->job_id
              << " node=" << ni->node_id
              << " submit_status=" << static_cast<int>(status) << "\n";
    active_jobs_.erase(job_it);
}

void RuntimeManager::save_job_outcome(const JobInstance& job,
                                      JobState state) {
    RuntimeJobOutcome outcome;
    outcome.job_id = job.job_id;
    outcome.state = state;
    outcome.completed_nodes = job.completed_nodes.load();
    outcome.total_nodes = job.total_nodes;
    for (const auto& entry : job.node_instances) {
        outcome.node_traces.emplace(entry.first, entry.second.trace);
    }
    job_outcomes_[job.job_id] = std::move(outcome);
}

void RuntimeManager::handle_job_release(const RuntimeEvent& evt) {
    int64_t now = now_us();
    jobs_released_++;
    std::cout << "[RTMgr] JOB_RELEASE " << evt.job_id
              << " at +" << now << "us\n";

    auto job = std::make_unique<JobInstance>(
        evt.job_id, evt.task_tmpl, now, evt.task_tmpl->deadline_us);

    auto initial = get_initial_ready_nodes(*job);
    {
        std::lock_guard<std::mutex> lk(jobs_mutex_);
        active_jobs_.emplace(
            std::piecewise_construct,
            std::forward_as_tuple(evt.job_id),
            std::forward_as_tuple(std::move(job)));
    }

    for (auto* ni : initial) {
        ni->trace.t_ready_us = now;
        enqueue_node(ni);
    }

    try_dispatch();
}

void RuntimeManager::handle_node_complete(const RuntimeEvent& evt) {
    int64_t now = now_us();
    std::vector<NodeInstance*> newly_ready;
    std::unique_lock<std::mutex> lk(jobs_mutex_);
    auto it = active_jobs_.find(evt.job_id);
    if (it == active_jobs_.end()) return;

    auto& job = *it->second;
    auto node_it = job.node_instances.find(evt.node_id);
    if (node_it == job.node_instances.end()) return;
    NodeInstance& node = node_it->second;

    if (evt.execution_result.has_value()) {
        const ExecutionResult& result = *evt.execution_result;
        auto request_it = in_flight_executions_.find(result.request_id);
        if (request_it == in_flight_executions_.end() ||
            request_it->second.job_id != evt.job_id ||
            request_it->second.node_id != evt.node_id ||
            result.job_id != evt.job_id || result.node_id != evt.node_id) {
            std::cout << "[RTMgr] IGNORE_BACKEND_COMPLETION request="
                      << result.request_id << " job=" << evt.job_id
                      << " node=" << evt.node_id << "\n";
            return;
        }
        in_flight_executions_.erase(request_it);

        node.trace.t_accel_start_us = result.start_time_us;
        node.trace.t_accel_end_us = result.end_time_us;
        node.trace.t_finish_us = result.end_time_us;
        node.trace.accel_exec_us = result.actual_exec_us;
        node.trace.total_node_us = result.actual_exec_us;

        if (result.status != ExecutionStatus::SUCCEEDED) {
            node.state.store(NodeState::FAILED);
            node.trace.final_state = NodeState::FAILED;
            job.state.store(JobState::FAILED);
            save_job_outcome(job, JobState::FAILED);
            jobs_failed_++;
            std::cout << "[RTMgr] FPGA_FAILED request=" << result.request_id
                      << " node=" << evt.job_id << "/" << evt.node_id
                      << " error=" << result.error_code << "\n";
            active_jobs_.erase(it);
            return;
        }

        node.state.store(NodeState::COMPLETED);
        node.trace.final_state = NodeState::COMPLETED;
        std::cout << "[RTMgr] FPGA_COMPLETE request=" << result.request_id
                  << " node=" << evt.job_id << "/" << evt.node_id
                  << " actual=" << result.actual_exec_us << "us\n";
    } else if (node.tmpl->node_type == NodeType::FPGA) {
        return;
    } else {
        node.trace.final_state = NodeState::COMPLETED;
    }

    newly_ready = advance_dag(job, evt.node_id, now);

    if (job.is_complete()) {
        bool met = (now <= job.absolute_deadline_us);
        job.state.store(JobState::COMPLETED);
        save_job_outcome(job, JobState::COMPLETED);
        std::cout << "[RTMgr] JOB_COMPLETE " << evt.job_id
                  << " response=" << (now - job.release_time_us) << "us"
                  << " deadline_met=" << (met ? "YES" : "NO!") << "\n";
        jobs_completed_++;
        active_jobs_.erase(it);
    }
    lk.unlock();

    for (auto* ni : newly_ready) {
        enqueue_node(ni);
    }
    if (!newly_ready.empty()) try_dispatch();
}

void RuntimeManager::handle_resource_idle(const RuntimeEvent&) {
    try_dispatch();
}

void RuntimeManager::try_dispatch() {
    if (!scheduler_ || !dispatcher_) return;

    // 遍历每个 CPU Worker：空闲则取对应核队列的队头提交
    for (auto& [core, worker] : dispatcher_->get_cpu_workers()) {
        if (!worker->is_idle()) continue;

        // resource_id 与 core 号一致（注册时 register_cpu_worker(core, w)）
        NodeInstance* ni = scheduler_->peek_top(core);
        if (!ni) continue;

        scheduler_->pop_top(core);
        worker->submit(ni);
        std::cout << "[RTMgr] try_dispatch: " << ni->node_id
                  << " → core" << core << "\n";
    }
    // GPU 队列由 GpuOwnerWorker 内部管理，不在此处理
}

// ============================================================
// ReleaseTimer
// ============================================================
ReleaseTimer::ReleaseTimer(RuntimeManager* mgr, const TaskTemplate* tmpl, int n)
    : mgr_(mgr), tmpl_(tmpl), num_periods_(n) {}

void ReleaseTimer::start() {
    running_ = true;
    thread_ = std::thread([this]{ run(); });
}

void ReleaseTimer::stop() {
    running_ = false;
    if (thread_.joinable()) thread_.join();
}

void ReleaseTimer::run() {
    int count = 0;
    while (running_ && count < num_periods_) {
        std::ostringstream oss;
        oss << tmpl_->task_id << "_job" << (++counter_);
        RuntimeEvent release_event;
        release_event.type = EventType::JOB_RELEASE;
        release_event.job_id = oss.str();
        release_event.timestamp_us = mgr_->now_us();
        release_event.task_tmpl = tmpl_;
        mgr_->post_event(std::move(release_event));
        std::this_thread::sleep_for(
            std::chrono::microseconds(tmpl_->period_us));
        count++;
    }
}
