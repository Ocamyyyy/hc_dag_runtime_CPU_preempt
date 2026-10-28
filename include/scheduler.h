#pragma once
#include "task_model.h"
#include <queue>
#include <mutex>
#include <unordered_map>
#include <vector>

struct ReadyNodeEntry {
    NodeInstance* node_inst;
    int           task_priority;
    int64_t       ready_time_us;

    bool operator>(const ReadyNodeEntry& o) const {
        if (task_priority != o.task_priority)
            return task_priority > o.task_priority;
        return ready_time_us > o.ready_time_us;
    }
};

using PerResourceQueue = std::priority_queue<
    ReadyNodeEntry,
    std::vector<ReadyNodeEntry>,
    std::greater<ReadyNodeEntry>>;

class BaselineScheduler {
public:
    // 节点就绪时入对应 resource_id 的队列
    // 仅 CPU 节点调用，GPU 节点直接 submit 给 GpuOwnerWorker
    void enqueue(NodeInstance* ni);

    // try_dispatch 调用：看某个 resource_id 的队头
    NodeInstance* peek_top(int resource_id);

    // 确认取出
    void pop_top(int resource_id);

    bool empty(int resource_id) const;

private:
    // key = assigned_resource_id（与 CpuWorker 注册的 core 号一致）
    std::unordered_map<int, PerResourceQueue> queues_;
    mutable std::mutex mutex_;
};