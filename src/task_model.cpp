/**
 * task_model.cpp
 * 任务模型层实现
 *
 * 对应开发日：Day 2（基础数据结构）/ Day 3（DAG实例化与依赖管理）
 */

#include "task_model.h"
#include <stdexcept>
#include <algorithm>

// ============================================================
// TaskTemplate
// ============================================================
const NodeTemplate* TaskTemplate::find_node(const std::string& node_id) const {
    for (const auto& n : nodes) {
        if (n.node_id == node_id) return &n;
    }
    return nullptr;
}

// ============================================================
// NodeInstance 构造
// ============================================================
NodeInstance::NodeInstance(const std::string& nid,
                           const std::string& jid,
                           const NodeTemplate* t,
                           int pred_count)
    : node_id(nid)
    , job_id(jid)
    , tmpl(t)
    , remaining_predecessors(pred_count)
    , state(NodeState::PENDING)
{
    // 初始化 trace
    trace = {};
    trace.node_id = nid;
    trace.job_id  = jid;
    trace.task_priority = 0; // 由 JobInstance 创建时填入
    trace.resource_type = t->node_type;
    trace.assigned_core = t->assigned_cpu_core;

    // 时间戳初始化为 -1（未记录标志）
    trace.t_ready_us = trace.t_dispatch_us = -1;
    trace.t_pre_start_us = trace.t_pre_end_us = -1;
    trace.t_queue_enter_us = trace.t_accel_start_us = trace.t_accel_end_us = -1;
    trace.t_post_start_us = trace.t_post_end_us = trace.t_finish_us = -1;
    trace.queue_wait_us = trace.accel_exec_us = trace.total_node_us = 0;
    trace.final_state = NodeState::PENDING;
}

// ============================================================
// JobInstance 构造（Day 3 核心：实例化 + 依赖计数初始化）
// ============================================================
JobInstance::JobInstance(const std::string& jid,
                         const TaskTemplate* t,
                         int64_t release_us,
                         int64_t rel_deadline_us)
    : job_id(jid)
    , task_id(t->task_id)
    , tmpl(t)
    , release_time_us(release_us)
    , absolute_deadline_us(release_us + rel_deadline_us)
    , state(JobState::ACTIVE)
    , total_nodes(static_cast<int>(t->nodes.size()))
    , completed_nodes(0)
{
    // 1. 建立节点实例，计算前驱数量
    // for (const auto& nt : t->nodes) {
    //     int pred_count = static_cast<int>(nt.predecessors.size());
    //     NodeInstance ni(nt.node_id, jid, &nt, pred_count);
    //     ni.trace.task_priority = t->task_priority;
    //     node_instances.emplace(nt.node_id, std::move(ni));
    // }
    // 不要先构造 ni 再 move，直接把参数传给 emplace 让它原地构造
    for (const auto& nt : t->nodes) {
        int pred_count = static_cast<int>(nt.predecessors.size());
        auto [it, ok] = node_instances.emplace(
            std::piecewise_construct,
            std::forward_as_tuple(nt.node_id),           // key 的构造参数
            std::forward_as_tuple(nt.node_id, jid, &nt, pred_count)  // value 的构造参数
        );
        it->second.trace.task_priority = t->task_priority;
    }

    // 2. 初始化 job trace
    trace = {};
    trace.job_id         = jid;
    trace.task_id        = t->task_id;
    trace.task_priority  = t->task_priority;
    trace.t_release_us   = release_us;
    trace.t_finish_us    = -1;
    trace.absolute_deadline_us = absolute_deadline_us;
    trace.deadline_met   = false;
    trace.response_time_us = 0;
}

/**
 * 获取当前所有"就绪"节点（前驱计数为0且状态为PENDING）
 * 在 JOB_RELEASE 时调用，返回无前驱节点列表
 *
 * Day 3：DAG 状态推进的起点
 */
std::vector<NodeInstance*> get_initial_ready_nodes(JobInstance& job) {
    std::vector<NodeInstance*> ready;
    for (auto& [nid, ni] : job.node_instances) {
        if (ni.remaining_predecessors.load() == 0) {
            ni.state.store(NodeState::READY);
            ready.push_back(&ni);
        }
    }
    return ready;
}

/**
 * 节点完成后，更新后继节点的前驱计数
 * 返回新进入 READY 状态的节点列表
 *
 * Day 3：DAG 依赖推进核心逻辑
 */
std::vector<NodeInstance*> advance_dag(JobInstance& job,
                                        const std::string& completed_node_id,
                                        int64_t finish_time_us)
{
    std::vector<NodeInstance*> newly_ready;

    // 找到完成节点的后继
    const NodeTemplate* tmpl = job.tmpl->find_node(completed_node_id);
    if (!tmpl) return newly_ready;

    for (const auto& succ_id : tmpl->successors) {
        auto it = job.node_instances.find(succ_id);
        if (it == job.node_instances.end()) continue;

        NodeInstance& succ_ni = it->second;

        // 原子递减前驱计数
        int remaining = succ_ni.remaining_predecessors.fetch_sub(1) - 1;

        if (remaining == 0) {
            // 所有前驱完成 → 后继就绪
            succ_ni.state.store(NodeState::READY);
            succ_ni.trace.t_ready_us = finish_time_us;
            newly_ready.push_back(&succ_ni);
        }
    }

    // 更新 job 完成计数
    job.completed_nodes.fetch_add(1);

    return newly_ready;
}
