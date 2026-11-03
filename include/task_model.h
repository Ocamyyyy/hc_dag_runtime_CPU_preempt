#pragma once
/**
 * task_model.h
 * 任务模型层（Task Modeling Layer） - Layer 1
 *
 * 对应开发日：Day 2（代码骨架与基础数据结构）
 *             Day 3（DAG 实例化与依赖管理）
 *
 * 职责：
 *   - 定义 DAG 任务模板（TaskTemplate）
 *   - 定义节点模板（NodeTemplate）
 *   - 定义资源模型（ResourceModel）
 *   - 定义任务实例（JobInstance）和节点实例（NodeInstance）
 *   - 模板与实例严格分离
 */

#include <string>
#include <vector>
#include <unordered_map>
#include <atomic>
#include <cstdint>

// ============================================================
// 枚举类型、节点状态机
// ============================================================

enum class NodeType {
    CPU,
    GPU,
    FPGA,       // 预留
    UNKNOWN
};

enum class Criticality {
    HI,         // 硬实时
    LO          // 软实时
};

enum class NodeState {
    PENDING,    // 等待前驱
    READY,      // 前驱满足，待调度
    DISPATCHED, // 已分发给 worker
    PRE_EXEC,   // CPU 前处理阶段
    ACCEL_WAIT, // 等待加速器队列
    ACCEL_EXEC, // 加速器执行中（不可抢占临界区）
    POST_EXEC,  // CPU 后处理阶段
    COMPLETED,  // 完成
    FAILED      // 失败（预留）
};

enum class JobState {
    ACTIVE,
    COMPLETED,
    FAILED,
    DEADLINE_MISSED
};

// ============================================================
// 资源模型（统一抽象 CPU/GPU/FPGA）
// ============================================================
struct ResourceModel {
    int         resource_id;
    NodeType    resource_type;
    bool        is_shared;          // GPU/FPGA 为共享资源
    bool        is_preemptable;     // CPU 可抢占，GPU/FPGA 不可抢占
    int         concurrency;        // 并发度（当前 GPU/FPGA = 1）
    // 注：无 owner_cpu_core 字段。Owner Worker 的绑核由各节点的
    // assigned_cpu_core 决定（节点级绑核），与 CPU Worker 对称。

    // 预留：未来 FPGA 重配置与能耗
    int         reconfig_time_us;   // FPGA 重配置时间（预留）
    double      idle_power_mw;      // 空闲功耗（预留）
    double      active_power_mw;    // 活跃功耗（预留）
};

// ============================================================
// 节点模板（静态描述，不随实例变化）
// ============================================================
struct NodeTemplate {
    std::string         node_id;
    std::string         task_id;        // 所属任务
    NodeType            node_type;

    /**
     * assigned_cpu_core：节点级 CPU 核绑定（对所有节点类型均有意义）
     *
     * CPU 节点：CPU Worker 在此核上执行
     * GPU/FPGA 节点：Owner Worker 在处理本节点时动态切换到此核，
     *   整个三段式（CPU pre + 等待accel完成 + CPU post）都在此核运行
     *
     * 例：A3(GPU) assigned_cpu_core=1 → Owner Worker 切换到 core1 执行 A3
     *     B3(GPU) assigned_cpu_core=2 → Owner Worker 切换到 core2 执行 B3
     */
    int                 assigned_cpu_core;
    int                 assigned_resource_id;   // 目标资源编号（GPU/FPGA 设备ID）
    int                 assigned_region_id{-1};
    int                 function_id{-1};
    int                 configuration_id{-1};
    int                 wcet_us;                // 最坏执行时间估计（微秒）

    // 执行三段式估计（GPU/FPGA节点）
    int                 pre_exec_us;    // CPU 前处理估计时间
    int                 accel_exec_us;  // 加速器执行估计时间
    int                 post_exec_us;   // CPU 后处理估计时间

    std::vector<std::string>  predecessors;   // 前驱节点 ID 列表
    std::vector<std::string>  successors;     // 后继节点 ID 列表
};

// ============================================================
// 任务模板（静态 DAG 定义）
// ============================================================
struct TaskTemplate {
    std::string         task_id;
    int                 task_priority;          // 数值越小优先级越高（1=最高）
    Criticality         criticality;
    int64_t             period_us;              // 周期/最小到达时间（微秒）
    int64_t             deadline_us;            // 相对截止期（微秒）

    std::vector<NodeTemplate>                   nodes;
    std::vector<std::pair<std::string,std::string>> edges; // <from, to>

    // 按 node_id 快速查找节点模板
    const NodeTemplate* find_node(const std::string& node_id) const;
};

// ============================================================
// Trace 数据结构（节点级时间戳记录）
// ============================================================
struct NodeTrace {
    std::string node_id;
    std::string job_id;
    int         task_priority;
    NodeType    resource_type;
    int         assigned_core;

    int64_t     t_ready_us;         // 节点就绪时刻
    int64_t     t_dispatch_us;      // 调度决策时刻
    int64_t     t_pre_start_us;     // CPU 前处理开始
    int64_t     t_pre_end_us;       // CPU 前处理结束
    int64_t     t_queue_enter_us;   // 进入加速器队列（GPU/FPGA）
    int64_t     t_accel_start_us;   // 加速器执行开始
    int64_t     t_accel_end_us;     // 加速器执行结束
    int64_t     t_post_start_us;    // CPU 后处理开始
    int64_t     t_post_end_us;      // CPU 后处理结束
    int64_t     t_finish_us;        // 节点完成时刻

    int64_t     queue_wait_us;      // 加速器队列等待时间
    int64_t     accel_exec_us;      // 加速器实际执行时间
    int64_t     total_node_us;      // 节点总响应时间

    NodeState   final_state;
};

struct JobTrace {
    std::string job_id;
    std::string task_id;
    int         task_priority;
    int64_t     t_release_us;
    int64_t     t_finish_us;
    int64_t     absolute_deadline_us;
    bool        deadline_met;
    int64_t     response_time_us;
};

// ============================================================
// 节点实例（运行时状态，每次 job 独立一份）
// ============================================================
struct NodeInstance {
    std::string             node_id;
    std::string             job_id;
    const NodeTemplate*     tmpl;           // 指向对应模板（只读）

    std::atomic<int>        remaining_predecessors; // 前驱计数（原子操作）
    std::atomic<NodeState>  state;

    NodeTrace               trace;          // 运行时 trace 记录

    // 不可拷贝（含 atomic）
    // NodeInstance(const NodeInstance&) = delete;
    // NodeInstance& operator=(const NodeInstance&) = delete;
    // NodeInstance(NodeInstance&&) = default;
    NodeInstance(const NodeInstance&)            = delete;
    NodeInstance& operator=(const NodeInstance&) = delete;
    NodeInstance(NodeInstance&&)                 = delete;  // 改为 delete，明确不支持
    NodeInstance& operator=(NodeInstance&&)      = delete;

    explicit NodeInstance(const std::string& nid, const std::string& jid,
                          const NodeTemplate* t, int pred_count);
};

// ============================================================
// 任务实例（JobInstance）
// ============================================================
struct JobInstance {
    std::string                             job_id;
    std::string                             task_id;
    const TaskTemplate*                     tmpl;       // 指向对应模板

    int64_t                                 release_time_us;
    int64_t                                 absolute_deadline_us;
    std::atomic<JobState>                   state;

    // 节点实例表，key = node_id
    std::unordered_map<std::string, NodeInstance> node_instances;

    int                                     total_nodes;
    std::atomic<int>                        completed_nodes;

    JobTrace                                trace;

    // 不可拷贝
    JobInstance(const JobInstance&) = delete;
    JobInstance& operator=(const JobInstance&) = delete;

    JobInstance(const std::string& jid, const TaskTemplate* t,
                int64_t release_us, int64_t deadline_us);

    bool is_complete() const {
        return completed_nodes.load() == total_nodes;
    }
};

// ============================================================
// 调度器输入输出
// ============================================================
struct ResourceStatus {
    int     resource_id;
    bool    is_busy;
    int     queue_depth;   // owner worker 当前队列长度
};

struct DispatchDecision {
    NodeInstance*   node_inst;
    int             target_resource_id;
    /**
     * target_cpu_core：节点级绑核，从 node_inst->tmpl->assigned_cpu_core 直接取得
     * 对 CPU 节点：CPU Worker 绑核目标
     * 对 GPU/FPGA 节点：Owner Worker 处理本节点时动态切换到此核
     */
    int             target_cpu_core;
    int64_t         dispatch_time_us;
};
