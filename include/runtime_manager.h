// include/runtime_manager.h（Day5版）
#pragma once
#include "backend.h"
#include "task_model.h"
#include <queue>
#include <mutex>
#include <condition_variable>
#include <thread>
#include <atomic>
#include <unordered_map>
#include <memory>
#include <optional>

enum class EventType {
    JOB_RELEASE,
    NODE_COMPLETE,
    RESOURCE_IDLE,
    SHUTDOWN
};

struct RuntimeEvent {
    EventType           type;
    std::string         job_id;
    std::string         node_id;
    int64_t             timestamp_us  = 0;
    const TaskTemplate* task_tmpl     = nullptr;
    std::optional<ExecutionResult> execution_result;
};

struct RuntimeJobOutcome {
    std::string job_id;
    JobState state{JobState::ACTIVE};
    int completed_nodes{0};
    int total_nodes{0};
    std::unordered_map<std::string, NodeTrace> node_traces;
};

std::vector<NodeInstance*> get_initial_ready_nodes(JobInstance&);
std::vector<NodeInstance*> advance_dag(JobInstance&, const std::string&, int64_t);

class BaselineScheduler;
class Dispatcher;

class RuntimeManager {
public:
    explicit RuntimeManager(int core = 0);
    ~RuntimeManager();

    void post_event(RuntimeEvent evt);
    void start();
    void stop();
    int64_t now_us() const;

    void set_scheduler(std::shared_ptr<BaselineScheduler> s);
    void set_dispatcher(std::shared_ptr<Dispatcher> d);
    std::optional<RuntimeJobOutcome> get_job_outcome(
        const std::string& job_id) const;

private:
    void event_loop();
    void handle_job_release(const RuntimeEvent&);
    void handle_node_complete(const RuntimeEvent&);
    void handle_resource_idle(const RuntimeEvent&);

    // 遍历所有 CPU Worker，将空闲 Worker 对应队列的队头节点提交
    void try_dispatch();

    // 根据节点类型路由：CPU 节点入 Scheduler 队列，GPU 节点直接 submit
    void enqueue_node(NodeInstance* ni);
    void fail_job_submission(NodeInstance* ni, SubmitStatus status);
    void save_job_outcome(const JobInstance& job, JobState state);

    int                  core_;
    std::thread          thread_;
    std::atomic<bool>    running_{false};

    std::queue<RuntimeEvent>   eq_;
    std::mutex                 eq_mutex_;
    std::condition_variable    eq_cv_;

    std::unordered_map<std::string, std::unique_ptr<JobInstance>> active_jobs_;
    std::unordered_map<std::string, RuntimeJobOutcome> job_outcomes_;
    struct InFlightExecution {
        std::string job_id;
        std::string node_id;
    };
    std::unordered_map<uint64_t, InFlightExecution> in_flight_executions_;
    mutable std::mutex jobs_mutex_;

    int64_t          start_time_us_;
    std::atomic<int> jobs_released_{0};
    std::atomic<int> jobs_completed_{0};
    std::atomic<int> jobs_failed_{0};

    std::shared_ptr<BaselineScheduler>  scheduler_;
    std::shared_ptr<Dispatcher>         dispatcher_;
};

class ReleaseTimer {
public:
    ReleaseTimer(RuntimeManager* mgr, const TaskTemplate* tmpl, int num_periods = 3);
    void start();
    void stop();
private:
    void run();
    RuntimeManager*      mgr_;
    const TaskTemplate*  tmpl_;
    int                  num_periods_;
    std::thread          thread_;
    std::atomic<bool>    running_{false};
    int                  counter_{0};
};
