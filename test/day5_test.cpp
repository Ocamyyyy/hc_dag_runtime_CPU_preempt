// src/day5_test.cpp
#include "task_model.h"
#include "runtime_manager.h"
#include "scheduler.h"
#include "dispatcher.h"
#include <iostream>
#include <memory>
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

TaskTemplate build_task_b() {
    TaskTemplate t;
    t.task_id = "TASK_B";
    t.task_priority = 2;
    t.criticality = Criticality::LO;
    t.period_us = 160000;
    t.deadline_us = 160000;

    NodeTemplate B1, B2, B3, B4, B5;

    B1.node_id = "B1";
    B1.node_type = NodeType::CPU;
    B1.assigned_cpu_core = 2;
    B1.assigned_resource_id = 2;
    B1.wcet_us = 8000;
    B1.pre_exec_us = 0;
    B1.accel_exec_us = 0;
    B1.post_exec_us = 0;
    B1.predecessors = {};
    B1.successors = {"B2", "B3"};

    B2.node_id = "B2";
    B2.node_type = NodeType::CPU;
    B2.assigned_cpu_core = 2;
    B2.assigned_resource_id = 2;
    B2.wcet_us = 6000;
    B2.pre_exec_us = 0;
    B2.accel_exec_us = 0;
    B2.post_exec_us = 0;
    B2.predecessors = {"B1"};
    B2.successors = {"B4"};

    B3.node_id = "B3";
    B3.node_type = NodeType::GPU;   // 当前阶段可先当 CPU 执行，验证结构
    B3.assigned_cpu_core = 2;
    B3.assigned_resource_id = 10;   // 与 A3 竞争同一 GPU
    B3.wcet_us = 20000;
    B3.pre_exec_us = 1000;
    B3.accel_exec_us = 18000;
    B3.post_exec_us = 1000;
    B3.predecessors = {"B1"};
    B3.successors = {"B5"};

    B4.node_id = "B4";
    B4.node_type = NodeType::CPU;
    B4.assigned_cpu_core = 2;
    B4.assigned_resource_id = 2;
    B4.wcet_us = 12000;
    B4.pre_exec_us = 0;
    B4.accel_exec_us = 0;
    B4.post_exec_us = 0;
    B4.predecessors = {"B2"};
    B4.successors = {"B5"};

    B5.node_id = "B5";
    B5.node_type = NodeType::CPU;
    B5.assigned_cpu_core = 2;
    B5.assigned_resource_id = 2;
    B5.wcet_us = 5000;
    B5.pre_exec_us = 0;
    B5.accel_exec_us = 0;
    B5.post_exec_us = 0;
    B5.predecessors = {"B3", "B4"};
    B5.successors = {};

    t.nodes = {B1, B2, B3, B4, B5};
    t.edges = {
        {"B1", "B2"},
        {"B1", "B3"},
        {"B2", "B4"},
        {"B3", "B5"},
        {"B4", "B5"}
    };

    return t;
}

int main() {
    std::cout << "=== Day 5 Test: 多队列调度 + CPU Worker 绑核 ===\n\n";
    std::cout << "预期：TASK_A(p=1) 的节点优先于 TASK_B(p=2) 被 DISPATCH\n\n";

    TaskTemplate tmpl_a = build_task_a();
    TaskTemplate tmpl_b = build_task_b();

    RuntimeManager mgr(0);
    auto sched = std::make_shared<BaselineScheduler>();
    auto disp  = std::make_shared<Dispatcher>(&mgr);

    // core1 对应 resource_id=1（TASK_A），core2 对应 resource_id=2（TASK_B）
    auto w1 = std::make_shared<CpuWorker>(1, &mgr);
    auto w2 = std::make_shared<CpuWorker>(2, &mgr);
    disp->register_cpu_worker(1, w1);
    disp->register_cpu_worker(2, w2);

    mgr.set_scheduler(sched);
    mgr.set_dispatcher(disp);

    mgr.start();
    w1->start();
    w2->start();

    // 同时释放两个 DAG，各 2 个周期
    ReleaseTimer ta(&mgr, &tmpl_a, 2);
    ReleaseTimer tb(&mgr, &tmpl_b, 2);
    ta.start();
    tb.start();

    std::this_thread::sleep_for(std::chrono::milliseconds(600));
    ta.stop(); tb.stop();
    std::this_thread::sleep_for(std::chrono::milliseconds(150));
    w1->stop(); w2->stop();
    mgr.stop();

    // ---- 今日验收（对照日志）----
    // 1. [Sched] ENQUEUE：A节点进 resource1 队列，B节点进 resource2 队列
    // 2. [RTMgr] try_dispatch：每个 Worker 空闲时取自己队列的队头
    // 3. [CpuWorker] DONE：A节点全在 core1，B节点全在 core2
    // 4. 所有 job 打印 JOB_COMPLETE
    // 注意：由于 A 和 B 各自使用独立队列，本测试中优先级的作用体现在
    //       同一个核上有多个节点排队时（如 TASK_A 连续释放两个 job）
    std::cout << "\n[Day5 验收] A节点→core1，B节点→core2，"
              << "同核多节点时高优先级先执行\n";
    return 0;
}