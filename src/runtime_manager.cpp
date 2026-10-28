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
    post_event({EventType::SHUTDOWN});
    if (thread_.joinable()) thread_.join();
    std::cout << "[RTMgr] Stopped. released=" << jobs_released_
              << " completed=" << jobs_completed_ << "\n";
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
    }
    // GPU 分支 Day6 补充
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
    std::lock_guard<std::mutex> lk(jobs_mutex_);
    auto it = active_jobs_.find(evt.job_id);
    if (it == active_jobs_.end()) return;

    auto& job = *it->second;
    auto newly_ready = advance_dag(job, evt.node_id, now);

    for (auto* ni : newly_ready) {
        enqueue_node(ni);
    }

    if (job.is_complete()) {
        bool met = (now <= job.absolute_deadline_us);
        std::cout << "[RTMgr] JOB_COMPLETE " << evt.job_id
                  << " response=" << (now - job.release_time_us) << "us"
                  << " deadline_met=" << (met ? "YES" : "NO!") << "\n";
        jobs_completed_++;
        active_jobs_.erase(it);
    }
    // 不在这里调 try_dispatch
    // Worker 会紧接着发 RESOURCE_IDLE，届时再调用
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
        mgr_->post_event({EventType::JOB_RELEASE, oss.str(), "",
                          mgr_->now_us(), tmpl_});
        std::this_thread::sleep_for(
            std::chrono::microseconds(tmpl_->period_us));
        count++;
    }
}