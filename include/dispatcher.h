// include/dispatcher.h
#pragma once
#include "task_model.h"
#include "runtime_manager.h"
#include <queue>
#include <mutex>
#include <condition_variable>
#include <thread>
#include <atomic>
#include <functional>
#include <memory>
#include <unordered_map>

struct WorkerRequest {
    NodeInstance* node_inst;
    int           task_priority;
    int64_t       enqueue_time_us;

    bool operator>(const WorkerRequest& o) const {
        if (task_priority != o.task_priority)
            return task_priority > o.task_priority;
        return enqueue_time_us > o.enqueue_time_us;
    }
};

class CpuWorker {
public:
    CpuWorker(int cpu_core, RuntimeManager* mgr);
    ~CpuWorker();

    void submit(NodeInstance* ni);
    void start();
    void stop();

    int  get_core() const { return cpu_core_; }
    bool is_idle()  const { return is_idle_.load(); }

private:
    void worker_loop();

    int              cpu_core_;
    RuntimeManager*  mgr_;

    std::priority_queue<WorkerRequest,
                        std::vector<WorkerRequest>,
                        std::greater<WorkerRequest>> queue_;
    std::mutex              q_mutex_;
    std::condition_variable q_cv_;
    std::thread             thread_;
    std::atomic<bool>       running_{false};
    std::atomic<bool>       is_idle_{true};
};

class Dispatcher {
public:
    explicit Dispatcher(RuntimeManager* mgr);

    void register_cpu_worker(int core, std::shared_ptr<CpuWorker> w);

    // try_dispatch 调用：遍历所有 CPU Worker
    // 返回本次实际派发的节点数
    const std::unordered_map<int, std::shared_ptr<CpuWorker>>&
        get_cpu_workers() const { return cpu_workers_; }

private:
    RuntimeManager* mgr_;
    std::unordered_map<int, std::shared_ptr<CpuWorker>> cpu_workers_;
};