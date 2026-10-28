#include "task_model.h"
#include "runtime_manager.h"
#include <iostream>
#include <thread>
#include <chrono>

TaskTemplate build_task_a() {
    TaskTemplate t;
    t.task_id = "TASK_A"; t.task_priority = 1;
    t.criticality = Criticality::HI;
    t.period_us = 100000; t.deadline_us = 100000;

    NodeTemplate A1, A2, A3, A4;
    A1.node_id="A1"; A1.node_type=NodeType::CPU;
    A1.assigned_cpu_core=1; A1.assigned_resource_id=1;
    A1.wcet_us=5000; A1.predecessors={}; A1.successors={"A2","A3"};

    A2.node_id="A2"; A2.node_type=NodeType::CPU;
    A2.assigned_cpu_core=1; A2.assigned_resource_id=1;
    A2.wcet_us=10000; A2.predecessors={"A1"}; A2.successors={"A4"};

    A3.node_id="A3"; A3.node_type=NodeType::GPU;  // 今天当 CPU 执行，验证结构
    A3.assigned_cpu_core=1; A3.assigned_resource_id=10;
    A3.wcet_us=15000; A3.pre_exec_us=1000; A3.accel_exec_us=13000; A3.post_exec_us=1000;
    A3.predecessors={"A1"}; A3.successors={"A4"};

    A4.node_id="A4"; A4.node_type=NodeType::CPU;
    A4.assigned_cpu_core=1; A4.assigned_resource_id=1;
    A4.wcet_us=5000; A4.predecessors={"A2","A3"}; A4.successors={};

    t.nodes={A1,A2,A3,A4};
    t.edges={{"A1","A2"},{"A1","A3"},{"A2","A4"},{"A3","A4"}};
    return t;
}

static int64_t get_time_us() {
    using namespace std::chrono;
    return duration_cast<microseconds>(
        steady_clock::now().time_since_epoch()).count();
}

// 绑定当前线程到指定核，返回是否成功
static bool bind_to_core(int core_id) {
    cpu_set_t cs;
    CPU_ZERO(&cs);
    CPU_SET(core_id, &cs);
    return pthread_setaffinity_np(pthread_self(), sizeof(cpu_set_t), &cs) == 0;
}

// 读取当前线程实际运行在哪个核上
static int get_current_core() {
    return sched_getcpu();
}

int main() {
    std::cout << "=== Day 4 Test: 事件循环 + 周期释放（3个周期）===\n\n";

    TaskTemplate tmpl = build_task_a();
    RuntimeManager mgr(0);

    // 节点执行器：同步执行后立即 post NODE_COMPLETE
    mgr.set_node_executor([&](NodeInstance* ni, RuntimeManager* m) {
        int target_core = ni->tmpl->assigned_cpu_core;

        // 绑核
        bool bind_ok = bind_to_core(target_core);
        if (!bind_ok) {
            std::cerr << "[WARN] node=" << ni->node_id
                    << " 绑核 core" << target_core << " 失败\n";
        }

        // 验证：绑核后读取实际运行核
        int actual_core = get_current_core();
        bool core_match = (actual_core == target_core);

        int64_t t0 = get_time_us();
        ni->state.store(NodeState::PRE_EXEC);
        ni->trace.t_pre_start_us = t0;

        std::this_thread::sleep_for(
            std::chrono::microseconds(ni->tmpl->wcet_us));

        int64_t t1 = get_time_us();
        ni->trace.t_finish_us = t1;
        ni->state.store(NodeState::COMPLETED);

        std::cout << "  [EXEC] " << ni->job_id << "/" << ni->node_id
                << " target_core=" << target_core
                << " actual_core=" << actual_core
                << " core_match=" << (core_match ? "OK" : "FAIL")
                  << " actual=" << (t1 - t0) << "us\n";

        // 通知事件循环该节点完成
        m->post_event({EventType::NODE_COMPLETE,
                       ni->job_id, ni->node_id, m->now_us()});
    });

    mgr.start();

    // 释放 3 个周期的 TASK_A
    ReleaseTimer timer(&mgr, &tmpl, 3);
    timer.start();

    // 等待足够时间：3 * 100ms + 缓冲
    std::this_thread::sleep_for(std::chrono::milliseconds(450));
    timer.stop();
    std::this_thread::sleep_for(std::chrono::milliseconds(100));
    mgr.stop();

    // ---- 今日验收 ----
    // 预期：3行 JOB_RELEASE，3行 JOB_COMPLETE，间隔约 100ms
    // 每个 job 内部 4 个 [EXEC] 行，按依赖顺序出现
    std::cout << "\n[Day4 验收] 3 个 job 均完成，时间戳间隔约 100ms → 对照日志确认\n";
    return 0;
}