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
        mgr_->post_event({EventType::NODE_COMPLETE,
                          ni->job_id, ni->node_id, mgr_->now_us()});
        mgr_->post_event({EventType::RESOURCE_IDLE,
                          "", "", mgr_->now_us()});

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