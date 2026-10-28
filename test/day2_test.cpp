// src/day2_test.cpp
#include "task_model.h"
#include <iostream>
#include <chrono>
#include <thread>
#include <unordered_map>
#include <sched.h>
#include <pthread.h>

// 获取当前时间（微秒）
static int64_t now_us() {
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

void execute_cpu_node(NodeInstance& ni) {
    int target_core = ni.tmpl->assigned_cpu_core;

    // 绑核
    bool bind_ok = bind_to_core(target_core);
    if (!bind_ok) {
        std::cerr << "[WARN] node=" << ni.node_id
                  << " 绑核 core" << target_core << " 失败\n";
    }

    // 验证：绑核后读取实际运行核
    int actual_core = get_current_core();
    bool core_match = (actual_core == target_core);

    int64_t t0 = now_us();
    ni.state.store(NodeState::PRE_EXEC);
    ni.trace.t_pre_start_us = t0;

    std::this_thread::sleep_for(
        std::chrono::microseconds(ni.tmpl->wcet_us));

    int64_t t1 = now_us();
    ni.trace.t_pre_end_us  = t1;
    ni.trace.t_finish_us   = t1;
    ni.trace.total_node_us = t1 - ni.trace.t_ready_us;
    ni.state.store(NodeState::COMPLETED);
    ni.trace.final_state = NodeState::COMPLETED;

    std::cout << "[EXEC] node=" << ni.node_id
              << " target_core=" << target_core
              << " actual_core=" << actual_core
              << " core_match=" << (core_match ? "OK" : "FAIL")
              << " actual=" << (t1 - t0) << "us\n";
}

// 构建 TASK_A 模板（与最终 main.cpp 一致）
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

int main() {
    std::cout << "=== Day 2 Test: 单线程顺序执行 + 核绑定验证 ===\n\n";

    // Day 2 是单线程测试，所有节点都在主线程上顺序执行。
    // 每次调用 execute_cpu_node 前会切换到对应核，
    // 验证点是每个节点执行时 sched_getcpu() 返回值与 assigned_cpu_core 一致。
    // Day 5 多线程后每个 Worker 线程启动时绑核一次即可，不再每个节点切换。

    TaskTemplate tmpl = build_task_a();
    int64_t release = now_us();
    JobInstance job("TASK_A_job1", &tmpl, release, tmpl.deadline_us);

    int pass_count = 0;
    int total_count = 0;

    for (const std::string& nid : {"A1", "A2", "A3", "A4"}) {
        auto& ni = job.node_instances.at(nid);
        ni.trace.t_ready_us = now_us();
        ni.state.store(NodeState::READY);

        int before_core = get_current_core();
        execute_cpu_node(ni);
        total_count++;

        // execute_cpu_node 内部已打印 core_match，这里做汇总统计
        if (sched_getcpu() == ni.tmpl->assigned_cpu_core) pass_count++;
    }

    int64_t finish = now_us();
    bool deadline_met = (finish <= job.absolute_deadline_us);

    std::cout << "\n=== 验收结果 ===\n";
    std::cout << "核绑定验证: " << pass_count << "/" << total_count
              << (pass_count == total_count ? " → PASS" : " → FAIL") << "\n";
    std::cout << "response=" << (finish - release) << "us"
              << "  deadline=" << tmpl.deadline_us << "us"
              << "  met=" << (deadline_met ? "YES" : "NO") << "\n";

    return (pass_count == total_count) ? 0 : 1;
}