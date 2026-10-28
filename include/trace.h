#pragma once
/**
 * trace.h
 * Trace/日志链路（TraceCollector）
 *
 * 对应开发日：Day 2（骨架）/ Day 7（完整输出）
 *
 * 职责：
 *   - 统一记录全流程运行数据（NodeTrace / JobTrace）
 *   - 线程安全写入
 *   - 支持导出 CSV / 文本格式
 *   - 为后续 RL reward 设计和阻塞分析提供数据源
 *
 * 记录的关键数据：
 *   - DAG 到达时间
 *   - 节点 ready / dispatch / start / finish 时间
 *   - CPU 前处理 / 加速器等待 / 加速器执行 / CPU 后处理时间
 *   - Owner Worker 队列长度快照
 *   - deadline 命中情况
 *   - 资源 busy/idle 状态变化
 */

#include "task_model.h"
#include <vector>
#include <string>
#include <mutex>
#include <fstream>
#include <memory>

// ============================================================
// 资源状态快照（周期性或事件触发时记录）
// ============================================================
struct ResourceSnapshot {
    int64_t     timestamp_us;
    int         resource_id;
    bool        is_busy;
    int         queue_depth;
};

// ============================================================
// Trace Collector
// ============================================================
class TraceCollector {
public:
    explicit TraceCollector(const std::string& output_dir = "./trace_output");

    // -------------------------------------------------------
    // 节点 Trace 接口（由 Worker/Backend 填写时间戳后调用）
    // -------------------------------------------------------
    void record_node_trace(const NodeTrace& trace);
    void record_job_trace(const JobTrace& trace);
    void record_resource_snapshot(const ResourceSnapshot& snap);

    // 快捷接口：记录节点状态转换时间戳
    void on_node_ready(NodeInstance* ni, int64_t ts_us);
    void on_node_dispatch(NodeInstance* ni, int64_t ts_us);
    void on_node_pre_start(NodeInstance* ni, int64_t ts_us);
    void on_node_pre_end(NodeInstance* ni, int64_t ts_us);
    void on_node_queue_enter(NodeInstance* ni, int64_t ts_us);
    void on_node_accel_start(NodeInstance* ni, int64_t ts_us);
    void on_node_accel_end(NodeInstance* ni, int64_t ts_us);
    void on_node_post_start(NodeInstance* ni, int64_t ts_us);
    void on_node_finish(NodeInstance* ni, int64_t ts_us);

    // -------------------------------------------------------
    // 导出接口
    // -------------------------------------------------------

    /**
     * 导出节点 trace 到 CSV
     * 列：job_id, node_id, task_priority, resource_type, assigned_core,
     *     t_ready, t_dispatch, t_pre_start, t_pre_end,
     *     t_queue_enter, t_accel_start, t_accel_end,
     *     t_post_start, t_finish,
     *     queue_wait_us, accel_exec_us, total_node_us
     */
    void export_node_traces_csv(const std::string& filename) const;

    /**
     * 导出 job trace 到 CSV
     * 列：job_id, task_id, priority, t_release, t_finish,
     *     absolute_deadline, deadline_met, response_time_us
     */
    void export_job_traces_csv(const std::string& filename) const;

    /**
     * 打印人类可读的摘要（控制台）
     */
    void print_summary() const;

    /**
     * 打印简易时序图（ASCII 甘特图）
     */
    void print_timeline() const;

    /**
     * 生成统计报告
     */
    struct Statistics {
        int     total_jobs;
        int     deadline_met_count;
        int     deadline_miss_count;
        double  deadline_meet_ratio;
        int64_t avg_response_time_us;
        int64_t max_response_time_us;
        int64_t avg_gpu_queue_wait_us;
        int64_t max_gpu_queue_wait_us;
    };
    Statistics compute_statistics() const;
    void print_statistics() const;

    // -------------------------------------------------------
    // 清理
    // -------------------------------------------------------
    void clear();

private:
    std::string                     output_dir_;

    std::vector<NodeTrace>          node_traces_;
    std::vector<JobTrace>           job_traces_;
    std::vector<ResourceSnapshot>   resource_snapshots_;

    mutable std::mutex              mutex_;

    // 创建输出目录
    void ensure_output_dir() const;
};
