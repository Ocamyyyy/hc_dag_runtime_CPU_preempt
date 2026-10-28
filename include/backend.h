#pragma once
/**
 * backend.h
 * 设备适配层（Backend Adapter Layer） - Layer 4
 *
 * 对应开发日：Day 2（接口定义）/ Day 5（CpuBackend）/
 *             Day 6（MockGpuBackend）/ Day 9（OpenClGpuBackend）
 *
 * 职责：
 *   - 定义统一 Backend 抽象接口
 *   - CpuBackend：CPU 节点执行 + 核绑定
 *   - MockGpuBackend：用 usleep 模拟 GPU 执行（Day 1-7 使用）
 *   - OpenClGpuBackend：真实 OpenCL 接入（Day 8-9 实现）
 *   - FpgaBackend：预留接口
 *
 * 设计原则：
 *   上层（Owner Worker / CPU Worker）只调用 execute_node()
 *   不感知底层是 Mock 还是真实设备
 */

#include "task_model.h"
#include <functional>
#include <memory>
#include <string>

// ============================================================
// 执行结果
// ============================================================
struct ExecuteResult {
    bool        success;
    int64_t     actual_exec_us;     // 实际执行时间（微秒）
    std::string error_msg;          // 失败时的错误信息
};

// ============================================================
// Backend 统一抽象接口
// ============================================================
class Backend {
public:
    virtual ~Backend() = default;

    /**
     * 初始化后端（平台/设备探测、context 建立等）
     * 返回 true = 成功
     */
    virtual bool initialize() = 0;

    /**
     * 销毁后端资源
     */
    virtual void shutdown() = 0;

    /**
     * 执行节点（核心接口）
     *
     * 对于 CPU 节点：
     *   - 绑核后直接执行 node_work()
     *
     * 对于 GPU/FPGA 节点（由 Owner Worker 调用）：
     *   - pre-section（CPU端参数准备）
     *   - 提交加速器任务（不可抢占临界区）
     *   - post-section（CPU端结果处理）
     *
     * @param node_inst  节点实例（含 trace 字段，需填写时间戳）
     * @param on_complete  完成回调（SUCCESS/FAIL）
     */
    virtual void execute_node(
        NodeInstance* node_inst,
        std::function<void(ExecuteResult)> on_complete) = 0;

    /**
     * 后端类型标识（用于日志）
     */
    virtual std::string backend_name() const = 0;

    /**
     * 是否支持抢占
     */
    virtual bool is_preemptable() const = 0;
};

// ============================================================
// CPU Backend（Day 5 实现）
// ============================================================
class CpuBackend : public Backend {
public:
    bool        initialize() override;
    void        shutdown() override;
    void        execute_node(NodeInstance* node_inst,
                             std::function<void(ExecuteResult)> on_complete) override;
    std::string backend_name() const override { return "CpuBackend"; }
    bool        is_preemptable() const override { return true; }

private:
    // 绑定当前线程到指定 CPU 核
    static bool bind_thread_to_core(int core_id);

    // 模拟 CPU 工作负载（实验阶段用 busy-wait 或 usleep）
    static void do_cpu_work(int wcet_us);
};

// ============================================================
// Mock GPU Backend（Day 6 实现，Day 1-7 主力）
// ============================================================
class MockGpuBackend : public Backend {
public:
    bool        initialize() override;
    void        shutdown() override;
    void        execute_node(NodeInstance* node_inst,
                             std::function<void(ExecuteResult)> on_complete) override;
    std::string backend_name() const override { return "MockGpuBackend"; }
    bool        is_preemptable() const override { return false; }

private:
    // 模拟 GPU kernel 执行（usleep + 时间戳记录）
    void simulate_gpu_kernel(NodeInstance* node_inst, ExecuteResult& result);
};

// ============================================================
// OpenCL GPU Backend（Day 9 实现）
// ============================================================
class OpenClGpuBackend : public Backend {
public:
    explicit OpenClGpuBackend(int platform_index = 0, int device_index = 0);
    ~OpenClGpuBackend();

    bool        initialize() override;
    void        shutdown() override;
    void        execute_node(NodeInstance* node_inst,
                             std::function<void(ExecuteResult)> on_complete) override;
    std::string backend_name() const override { return "OpenClGpuBackend"; }
    bool        is_preemptable() const override { return false; }

    // OpenCL 平台信息（调试用）
    std::string get_platform_info() const;
    std::string get_device_info() const;

private:
    // OpenCL 对象（使用 void* 避免在未安装 OpenCL 时编译失败）
    // 实际实现时替换为 cl_platform_id / cl_device_id 等类型
    void*   cl_platform_;   // cl_platform_id
    void*   cl_device_;     // cl_device_id
    void*   cl_context_;    // cl_context
    void*   cl_queue_;      // cl_command_queue
    void*   cl_program_;    // cl_program（预编译 kernel）

    int     platform_index_;
    int     device_index_;
    bool    initialized_;

    // 最小 kernel（向量加法，用于接入验证）
    static constexpr const char* VALIDATION_KERNEL_SRC = R"(
        __kernel void vector_add(__global const float* a,
                                  __global const float* b,
                                  __global float* c,
                                  const int n) {
            int i = get_global_id(0);
            if (i < n) c[i] = a[i] + b[i];
        }
    )";

    bool compile_kernel();
    void execute_opencl_kernel(NodeInstance* node_inst, ExecuteResult& result);
};

// ============================================================
// FPGA Backend（预留接口，Day 10 后扩展）
// ============================================================
class FpgaBackend : public Backend {
public:
    bool        initialize() override;
    void        shutdown() override;
    void        execute_node(NodeInstance* node_inst,
                             std::function<void(ExecuteResult)> on_complete) override;
    std::string backend_name() const override { return "FpgaBackend(stub)"; }
    bool        is_preemptable() const override { return false; }

    // FPGA 专有接口（预留）
    virtual void reconfigure(int bitstream_id);
    virtual int  get_reconfig_time_us() const;
};

// ============================================================
// Backend 工厂
// ============================================================
class BackendFactory {
public:
    enum class BackendType { CPU, MOCK_GPU, OPENCL_GPU, FPGA };

    static std::unique_ptr<Backend> create(BackendType type);
};
