// src/day3_test.cpp
#include "task_model.h"
#include <iostream>
#include <thread>
#include <chrono>
#include <queue>

static int64_t now_us() {
    using namespace std::chrono;
    return duration_cast<microseconds>(
        steady_clock::now().time_since_epoch()).count();
}

// 声明（定义在 task_model.cpp）
std::vector<NodeInstance*> get_initial_ready_nodes(JobInstance&);
std::vector<NodeInstance*> advance_dag(JobInstance&, const std::string&, int64_t);

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

// 简单串行执行一个节点，返回完成时刻
int64_t execute_node_sync(NodeInstance& ni) {
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

    ni.state.store(NodeState::PRE_EXEC);
    ni.trace.t_pre_start_us = now_us();
    std::this_thread::sleep_for(
        std::chrono::microseconds(ni.tmpl->wcet_us));
    int64_t t_finish = now_us();
    ni.trace.t_pre_end_us = ni.trace.t_finish_us = t_finish;
    ni.state.store(NodeState::COMPLETED);
    std::cout << "[EXEC] " << ni.node_id
    << " target_core=" << target_core
              << " actual_core=" << actual_core
              << " core_match=" << (core_match ? "OK" : "FAIL")
              << " done at +" << (t_finish - ni.trace.t_ready_us) << "us\n";
    return t_finish;
}

int main() {
    std::cout << "=== Day 3 Test: 依赖计数 + DAG 推进 ===\n\n";

    TaskTemplate tmpl = build_task_a();
    int64_t release = now_us();
    JobInstance job("TASK_A_job1", &tmpl, release, tmpl.deadline_us);

    // 用 BFS 队列模拟事件驱动推进（单线程）
    std::queue<NodeInstance*> ready_queue;

    // 1. 获取初始就绪节点（无前驱）
    auto initial = get_initial_ready_nodes(job);
    for (auto* ni : initial) {
        ni->trace.t_ready_us = release;
        std::cout << "[INIT READY] " << ni->node_id << "\n";
        ready_queue.push(ni);
    }

    // 2. BFS 循环处理 ready 节点
    while (!ready_queue.empty()) {
        NodeInstance* ni = ready_queue.front();
        ready_queue.pop();

        int64_t t_finish = execute_node_sync(*ni);

        // 推进后继，将新就绪节点加入队列
        auto newly_ready = advance_dag(job, ni->node_id, t_finish);
        for (auto* next : newly_ready) {
            ready_queue.push(next);
        }
    }

    // ---- 今日验收 ----
    // 关键验证点：A4 必须在 A2 和 A3 都完成后才进入 READY
    // 预期日志：
    //   [INIT READY] A1
    //   [EXEC] A1 done
    //     [DAG] A1 → A2 READY
    //     [DAG] A1 → A3 READY
    //   [EXEC] A2 done
    //     [DAG] A2 → A4 still waiting (1 predecessor(s) left)
    //   [EXEC] A3 done
    //     [DAG] A3 → A4 READY (remaining was 1)
    //   [EXEC] A4 done

    std::cout << "\n=== 验收检查 ===\n";
    auto& a4 = job.node_instances.at("A4");
    bool a4_ok = (a4.state.load() == NodeState::COMPLETED);
    bool all_done = job.is_complete();
    std::cout << "A4 completed: " << (a4_ok ? "YES" : "NO") << "\n";
    std::cout << "All nodes done: " << (all_done ? "YES" : "NO") << "\n";

    // 验证 A4 是最后完成的（t_finish > A2 和 A3）
    auto& a2 = job.node_instances.at("A2");
    auto& a3 = job.node_instances.at("A3");
    bool order_ok = (a4.trace.t_finish_us > a2.trace.t_finish_us) &&
                    (a4.trace.t_finish_us > a3.trace.t_finish_us);
    std::cout << "A4 finished after A2 and A3: " << (order_ok ? "YES ← PASS" : "NO ← FAIL") << "\n";

    return 0;
}