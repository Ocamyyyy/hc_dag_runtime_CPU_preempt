// src/scheduler.cpp
#include "scheduler.h"
#include <iostream>

void BaselineScheduler::enqueue(NodeInstance* ni) {
    int rid = ni->tmpl->assigned_resource_id;
    std::lock_guard<std::mutex> lk(mutex_);
    queues_[rid].push({ni, ni->trace.task_priority, ni->trace.t_ready_us});
    std::cout << "[Sched] ENQUEUE " << ni->node_id
              << " job=" << ni->job_id
              << " → resource" << rid
              << " priority=" << ni->trace.task_priority << "\n";
}

NodeInstance* BaselineScheduler::peek_top(int resource_id) {
    std::lock_guard<std::mutex> lk(mutex_);
    auto it = queues_.find(resource_id);
    if (it == queues_.end() || it->second.empty()) return nullptr;
    return it->second.top().node_inst;
}

void BaselineScheduler::pop_top(int resource_id) {
    std::lock_guard<std::mutex> lk(mutex_);
    auto it = queues_.find(resource_id);
    if (it == queues_.end() || it->second.empty()) return;
    std::cout << "[Sched] DISPATCH " << it->second.top().node_inst->node_id
              << " job=" << it->second.top().node_inst->job_id
              << " → resource" << resource_id << "\n";
    it->second.pop();
}

bool BaselineScheduler::empty(int resource_id) const {
    std::lock_guard<std::mutex> lk(mutex_);
    auto it = queues_.find(resource_id);
    return it == queues_.end() || it->second.empty();
}