/**
 * @file nested_example.cpp
 * @brief Example demonstrating a 6-generation-deep nested state machine
 *
 * Hierarchy (each level is a sub-state machine of the parent's active state):
 *
 *   Gen 1: System        (SystemState::BOOTING → RUNNING → SHUTDOWN)
 *     Gen 2: Core        (CoreState::INITIALIZING → ACTIVE → TERMINATING)
 *       Gen 3: Manager   (MgrState::STARTING → MANAGING → STOPPING)
 *         Gen 4: Worker  (WorkerState::SPAWNING → WORKING → DONE)
 *           Gen 5: Task  (TaskState::PENDING → EXECUTING → COMPLETE)
 *             Gen 6: Step (StepState::BEGIN → PROCESSING → FINISHED)
 *
 * Each child SM is attached via withSubStates() and advances independently.
 * statePath() shows the full depth, e.g. "Running/Active/Managing/Working/Executing/Processing"
 */

#include "StateMachine.hpp"
#include <iostream>
#include <string>

// ── Generation 1: System ──────────────────────────────────────────
enum class SystemState { BOOTING, RUNNING, SHUTDOWN };

class SystemBootState : public StateMachine<SystemState>::State {
public:
    bool enter() override {
        std::cout << "[Gen1:System] Booting..." << std::endl;
        return true;
    }
    void update() override {
        std::cout << "[Gen1:System] update() — transitioning to Running" << std::endl;
        changeToState(SystemState::RUNNING, "Boot complete");
    }
};

class SystemRunningState : public StateMachine<SystemState>::State {
public:
    bool enter() override {
        std::cout << "[Gen1:System] Running" << std::endl;
        return true;
    }
    void update() override {
        static int t = 0;
        std::cout << "[Gen1:System] update() — tick " << ++t << "/20" << std::endl;
        if (t >= 20) { t = 0; changeToState(SystemState::SHUTDOWN, "Shutdown requested"); }
    }
    void exit() override {
        std::cout << "[Gen1:System] exit() — Stopping" << std::endl;
    }
};

class SystemShutdownState : public StateMachine<SystemState>::State {
public:
    bool enter() override {
        std::cout << "[Gen1:System] Shutdown" << std::endl;
        return true;
    }
};

// ── Generation 2: Core ────────────────────────────────────────────
enum class CoreState { INITIALIZING, ACTIVE, TERMINATING };

class CoreInitState : public StateMachine<CoreState>::State {
public:
    bool enter() override {
        std::cout << "  [Gen2:Core] Initializing" << std::endl;
        return true;
    }
    void update() override {
        std::cout << "  [Gen2:Core] update() — transitioning to Active" << std::endl;
        changeToState(CoreState::ACTIVE, "Core ready");
    }
};

class CoreActiveState : public StateMachine<CoreState>::State {
public:
    bool enter() override {
        std::cout << "  [Gen2:Core] Active" << std::endl;
        return true;
    }
    void update() override {
        static int t = 0;
        std::cout << "  [Gen2:Core] update() — tick " << ++t << "/16" << std::endl;
        if (t >= 16) { t = 0; changeToState(CoreState::TERMINATING, "Core terminating"); }
    }
};

class CoreTerminatingState : public StateMachine<CoreState>::State {
public:
    bool enter() override {
        std::cout << "  [Gen2:Core] Terminating" << std::endl;
        return true;
    }
};

// ── Generation 3: Manager ─────────────────────────────────────────
enum class MgrState { STARTING, MANAGING, STOPPING };

class MgrStartingState : public StateMachine<MgrState>::State {
public:
    bool enter() override {
        std::cout << "    [Gen3:Manager] Starting" << std::endl;
        return true;
    }
    void update() override {
        std::cout << "    [Gen3:Manager] update() — transitioning to Managing" << std::endl;
        changeToState(MgrState::MANAGING, "Manager ready");
    }
};

class MgrManagingState : public StateMachine<MgrState>::State {
public:
    bool enter() override {
        std::cout << "    [Gen3:Manager] Managing" << std::endl;
        return true;
    }
    void update() override {
        static int t = 0;
        std::cout << "    [Gen3:Manager] update() — tick " << ++t << "/12" << std::endl;
        if (t >= 12) { t = 0; changeToState(MgrState::STOPPING, "Manager stopping"); }
    }
};

class MgrStoppingState : public StateMachine<MgrState>::State {
public:
    bool enter() override {
        std::cout << "    [Gen3:Manager] Stopping" << std::endl;
        return true;
    }
};

// ── Generation 4: Worker ──────────────────────────────────────────
enum class WorkerState { SPAWNING, WORKING, DONE };

class WorkerSpawningState : public StateMachine<WorkerState>::State {
public:
    bool enter() override {
        std::cout << "      [Gen4:Worker] Spawning" << std::endl;
        return true;
    }
    void update() override {
        std::cout << "      [Gen4:Worker] update() — transitioning to Working" << std::endl;
        changeToState(WorkerState::WORKING, "Worker spawned");
    }
};

class WorkerWorkingState : public StateMachine<WorkerState>::State {
public:
    bool enter() override {
        std::cout << "      [Gen4:Worker] Working" << std::endl;
        return true;
    }
    void update() override {
        static int t = 0;
        std::cout << "      [Gen4:Worker] update() — tick " << ++t << "/8" << std::endl;
        if (t >= 8) { t = 0; changeToState(WorkerState::DONE, "Worker finished"); }
    }
};

class WorkerDoneState : public StateMachine<WorkerState>::State {
public:
    bool enter() override {
        std::cout << "      [Gen4:Worker] Done" << std::endl;
        return true;
    }
};

// ── Generation 5: Task ────────────────────────────────────────────
enum class TaskState { PENDING, EXECUTING, COMPLETE };

class TaskPendingState : public StateMachine<TaskState>::State {
public:
    bool enter() override {
        std::cout << "        [Gen5:Task] Pending" << std::endl;
        return true;
    }
    void update() override {
        std::cout << "        [Gen5:Task] update() — transitioning to Executing" << std::endl;
        changeToState(TaskState::EXECUTING, "Task started");
    }
};

class TaskExecutingState : public StateMachine<TaskState>::State {
public:
    bool enter() override {
        std::cout << "        [Gen5:Task] Executing" << std::endl;
        return true;
    }
    void update() override {
        static int t = 0;
        std::cout << "        [Gen5:Task] update() — tick " << ++t << "/5" << std::endl;
        if (t >= 5) { t = 0; changeToState(TaskState::COMPLETE, "Task complete"); }
    }
};

class TaskCompleteState : public StateMachine<TaskState>::State {
public:
    bool enter() override {
        std::cout << "        [Gen5:Task] Complete" << std::endl;
        return true;
    }
};

// ── Generation 6: Step ────────────────────────────────────────────
enum class StepState { BEGIN, PROCESSING, FINISHED };

class StepBeginState : public StateMachine<StepState>::State {
public:
    bool enter() override {
        std::cout << "          [Gen6:Step] Begin" << std::endl;
        return true;
    }
    void update() override {
        std::cout << "          [Gen6:Step] update() — transitioning to Processing" << std::endl;
        changeToState(StepState::PROCESSING, "Step started");
    }
};

class StepProcessingState : public StateMachine<StepState>::State {
public:
    bool enter() override {
        std::cout << "          [Gen6:Step] Processing" << std::endl;
        return true;
    }
    void update() override {
        static int t = 0;
        std::cout << "          [Gen6:Step] update() — tick " << ++t << "/3" << std::endl;
        if (t >= 3) { t = 0; changeToState(StepState::FINISHED, "Step finished"); }
    }
};

class StepFinishedState : public StateMachine<StepState>::State {
public:
    bool enter() override {
        std::cout << "          [Gen6:Step] Finished" << std::endl;
        return true;
    }
    void update() override {
        static int t = 0;
        std::cout << "          [Gen6:Step] update() — tick " << ++t << "/2" << std::endl;
        if (t >= 2) { t = 0; changeToState(StepState::BEGIN, "Next step"); }
    }
};

// ── Main ──────────────────────────────────────────────────────────

int main() {
    std::cout << "\n=== 6-Generation Nested State Machine ===\n" << std::endl;

    // ── Build sub-machines bottom-up (Gen 6 → Gen 1) ───────────────
    // Each level is constructed independently — no nested lambdas, no
    // .template syntax — then attached to its parent with attachSub().

    // Gen 6: Step
    auto step = sm::make<StepState>(StepState::BEGIN, "Step");
    step->addState<StepBeginState>(StepState::BEGIN, "Begin")
        .addState<StepProcessingState>(StepState::PROCESSING, "Processing")
        .addState<StepFinishedState>(StepState::FINISHED, "Finished");

    // Gen 5: Task (attaches Step inside Task::EXECUTING)
    auto task = sm::make<TaskState>(TaskState::PENDING, "Task");
    task->addState<TaskPendingState>(TaskState::PENDING, "Pending")
        .addState<TaskExecutingState>(TaskState::EXECUTING, "Executing")
        .addState<TaskCompleteState>(TaskState::COMPLETE, "Complete")
        .attachSub(TaskState::EXECUTING, std::move(step));

    // Gen 4: Worker (attaches Task inside Worker::WORKING)
    auto worker = sm::make<WorkerState>(WorkerState::SPAWNING, "Worker");
    worker->addState<WorkerSpawningState>(WorkerState::SPAWNING, "Spawning")
        .addState<WorkerWorkingState>(WorkerState::WORKING, "Working")
        .addState<WorkerDoneState>(WorkerState::DONE, "Done")
        .attachSub(WorkerState::WORKING, std::move(task));

    // Gen 3: Manager (attaches Worker inside Manager::MANAGING)
    auto mgr = sm::make<MgrState>(MgrState::STARTING, "Manager");
    mgr->addState<MgrStartingState>(MgrState::STARTING, "Starting")
        .addState<MgrManagingState>(MgrState::MANAGING, "Managing")
        .addState<MgrStoppingState>(MgrState::STOPPING, "Stopping")
        .attachSub(MgrState::MANAGING, std::move(worker));

    // Gen 2: Core (attaches Manager inside Core::ACTIVE)
    auto core = sm::make<CoreState>(CoreState::INITIALIZING, "Core");
    core->addState<CoreInitState>(CoreState::INITIALIZING, "Initializing")
        .addState<CoreActiveState>(CoreState::ACTIVE, "Active")
        .addState<CoreTerminatingState>(CoreState::TERMINATING, "Terminating")
        .attachSub(CoreState::ACTIVE, std::move(mgr));

    // Gen 1: System (attaches Core inside System::RUNNING)
    StateMachine<SystemState> system(SystemState::BOOTING, "System");
    system.addState<SystemBootState>(SystemState::BOOTING, "Booting")
        .addState<SystemRunningState>(SystemState::RUNNING, "Running")
        .addState<SystemShutdownState>(SystemState::SHUTDOWN, "Shutdown")
        .withLogLevel(StateMachine<SystemState>::LogLevel::WARN)
        .attachSub(SystemState::RUNNING, std::move(core));

    // Validate the entire hierarchy
    if (!system.validate()) {
        std::cout << "Configuration invalid!" << std::endl;
        return 1;
    }
    std::cout << "Configuration valid. Starting...\n" << std::endl;

    system.start();

    // Run until the top-level system reaches SHUTDOWN
    for (int i = 0; i < 60; ++i) {
        std::cout << "--- Tick " << (i + 1) << " ---" << std::endl;
        system.update();
        std::cout << "  Path: " << system.statePath() << std::endl;

        if (system.getCurrentStateId() == SystemState::SHUTDOWN) {
            std::cout << "\nSystem has shut down." << std::endl;
            break;
        }
    }

    std::cout << "\n=== 6-generation nested example complete ===" << std::endl;
    return 0;
}
