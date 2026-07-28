// ============================================================================
// StateMachine Test Suite
// ============================================================================
// This file contains comprehensive tests for the StateMachine implementation,
// including basic functionality, complex scenarios, multi-threading stress
// tests, and edge cases.
// ============================================================================

#include <StateMachine/StateMachine.hpp>
#include <algorithm>
#include <atomic>
#include <chrono>
#include <gtest/gtest.h>
#include <memory>
#include <string>
#include <sys/wait.h>
#include <thread>
#include <unistd.h>
#include <utility>
#include <vector>

// ============================================================================
// TEST STATE DEFINITIONS
// ============================================================================

// Define states for testing
enum class TestStateID { IDLE, RUNNING, PAUSED, ERROR, FALLBACK };

// Base test state
struct TestState : public StateMachine<TestStateID>::State {
  std::string name;
  int enterCount = 0;
  int exitCount = 0;
  int updateCount = 0;
  bool shouldFailEnter = false;

  TestState(std::string n) : name(n) {}

  bool enter() override {
    enterCount++;
    return !shouldFailEnter;
  }

  void exit() override { exitCount++; }

  void update() override { updateCount++; }
};

class StateMachineTest : public ::testing::Test {
protected:
  void SetUp() override {
    sm = std::make_unique<StateMachine<TestStateID>>(TestStateID::IDLE,
                                                     "TestMachine");
  }

  std::unique_ptr<StateMachine<TestStateID>> sm;
};

// ============================================================================
// BASIC FUNCTIONALITY TESTS
// ============================================================================

// Test basic initialization and start
TEST_F(StateMachineTest, InitializationAndStart) {
  auto idleState = std::make_unique<TestState>("Idle");
  auto *idlePtr = idleState.get();

  sm->addState(TestStateID::IDLE, "Idle", std::move(idleState));

  EXPECT_FALSE(sm->isReady());
  EXPECT_EQ(sm->getStateCount(), 1);

  sm->start();

  EXPECT_TRUE(sm->isReady());
  EXPECT_EQ(sm->getCurrentStateId(), TestStateID::IDLE);
  EXPECT_EQ(sm->getCurrentStateName(), "Idle");
  EXPECT_EQ(idlePtr->enterCount, 1);
}

// Test state transitions
TEST_F(StateMachineTest, StateTransitions) {
  auto idleState = std::make_unique<TestState>("Idle");
  auto runningState = std::make_unique<TestState>("Running");

  auto *idlePtr = idleState.get();
  auto *runningPtr = runningState.get();

  sm->addState(TestStateID::IDLE, "Idle", std::move(idleState))
      .addState(TestStateID::RUNNING, "Running", std::move(runningState));

  sm->start();

  bool changed = sm->changeState(TestStateID::RUNNING, "Start running");

  EXPECT_TRUE(changed);
  EXPECT_EQ(sm->getCurrentStateId(), TestStateID::RUNNING);
  EXPECT_EQ(idlePtr->exitCount, 1);
  EXPECT_EQ(runningPtr->enterCount, 1);

  // Check history
  auto history = sm->getStateHistory();
  EXPECT_EQ(history.size(), 2); // Initial IDLE + RUNNING
  EXPECT_EQ(history[0], TestStateID::IDLE);
  EXPECT_EQ(history[1], TestStateID::RUNNING);
}

// Test context type safety
TEST_F(StateMachineTest, ContextTypeSafety) {
  struct MyContext {
    int value = 42;
  };

  auto context = std::make_shared<MyContext>();
  sm->withContext(context);

  auto retrieved = sm->getContext<MyContext>();
  EXPECT_EQ(retrieved->value, 42);

  EXPECT_THROW(sm->getContext<std::string>(), std::runtime_error);
}

// Test fallback mechanism (Deadlock fix verification)
TEST_F(StateMachineTest, FallbackOnFailure) {
  auto idleState = std::make_unique<TestState>("Idle");
  auto errorState = std::make_unique<TestState>("Error");
  errorState->shouldFailEnter = true; // Simulate failure

  auto fallbackState = std::make_unique<TestState>("Fallback");

  sm->addState(TestStateID::IDLE, "Idle", std::move(idleState))
      .addState(TestStateID::ERROR, "Error", std::move(errorState))
      .addState(TestStateID::FALLBACK, "Fallback", std::move(fallbackState));

  sm->withFallback(TestStateID::FALLBACK);
  sm->start();

  // Try to go to error state, enter() fails, should trigger fallback
  // This previously caused a deadlock due to non-recursive mutex
  bool success = sm->changeState(TestStateID::ERROR, "Trigger error");

  EXPECT_FALSE(success); // changeState returns false when the requested state
                         // fails, even if the fallback succeeds
  EXPECT_EQ(sm->getCurrentStateId(), TestStateID::FALLBACK);
}

// Test self-transition within enter (Re-entry deadlock fix)
TEST_F(StateMachineTest, ReentrantTransition) {
  struct SelfTransitionState : public TestState {
    SelfTransitionState() : TestState("SelfTrans") {}

    bool enter() override {
      TestState::enter();
      // Immediate transition request from within enter()
      if (enterCount == 1) {
        changeToState(TestStateID::RUNNING, "Auto-advance");
      }
      return true;
    }
  };

  auto startState = std::make_unique<SelfTransitionState>();
  auto runningState = std::make_unique<TestState>("Running");

  sm->addState(TestStateID::IDLE, "Start", std::move(startState))
      .addState(TestStateID::RUNNING, "Running", std::move(runningState));

  sm->start(); // Starts in IDLE, enters IDLE, triggers change to RUNNING

  EXPECT_EQ(sm->getCurrentStateId(), TestStateID::RUNNING);
}

// Test concurrency / Race condition fix
TEST_F(StateMachineTest, ConcurrentUpdateAndTransition) {
  auto idleState = std::make_unique<TestState>("Idle");
  auto runningState = std::make_unique<TestState>("Running");

  // Make update slow to increase race window
  struct SlowState : public TestState {
    SlowState(std::string n) : TestState(n) {}
    std::atomic<bool> insideUpdate{false};

    void update() override {
      insideUpdate = true;
      std::this_thread::sleep_for(std::chrono::milliseconds(10));
      TestState::update();
      insideUpdate = false;
    }
  };

  auto slowState = std::make_unique<SlowState>("Slow");

  sm->addState(TestStateID::IDLE, "Slow", std::move(slowState))
      .addState(TestStateID::RUNNING, "Running", std::move(runningState));

  sm->start();

  std::atomic<bool> stop{false};

  // Thread 1: Calls update in loop
  std::thread updater([&]() {
    while (!stop) {
      sm->update();
      std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }
  });

  // Thread 2: Triggers state change
  std::this_thread::sleep_for(std::chrono::milliseconds(5)); // Let update start
  sm->changeState(TestStateID::RUNNING, "Race check");

  stop = true;
  updater.join();

  // If we're here without segfaults or deadlocks, we're good.
  // The previous race condition meant update() could run on a destroyed state
  // or during exit() With the fix, update() holds the lock during execution.

  EXPECT_EQ(sm->getCurrentStateId(), TestStateID::RUNNING);
}

// Test callbacks
TEST_F(StateMachineTest, Callbacks) {
  auto idleState = std::make_unique<TestState>("Idle");
  auto runningState = std::make_unique<TestState>("Running");

  int callbackCount = 0;
  sm->onStateChanged([&](const TestStateID &from, const TestStateID &to,
                         std::string_view fromName, std::string_view toName,
                         std::string_view reason) {
    (void)fromName;
    (void)toName;
    (void)reason;
    callbackCount++;
    EXPECT_EQ(from, TestStateID::IDLE);
    EXPECT_EQ(to, TestStateID::RUNNING);
  });

  sm->addState(TestStateID::IDLE, "Idle", std::move(idleState))
      .addState(TestStateID::RUNNING, "Running", std::move(runningState));

  sm->start();
  sm->changeState(TestStateID::RUNNING);

  EXPECT_EQ(callbackCount, 1);
}

// ============================================================================
// COMPLEX MISSION SIMULATION
// ============================================================================
// Real-time mission simulation with multiple states and autonomous transitions

// Mission states
enum class MissionStateID {
  IDLE,
  ARMING,
  TAKEOFF,
  WAYPOINTS,
  RTL,
  LANDING,
  LANDED,
  EMERGENCY
};

// Mission context
struct MissionContext {
  std::atomic<double> batteryLevel{100.0};
  std::atomic<int> currentWaypoint{0};
  std::atomic<double> altitude{0.0};
  std::atomic<bool> isArmed{false};
  std::vector<std::string> logs;
  std::recursive_mutex logMutex;

  void addLog(const std::string &msg) {
    std::lock_guard<std::recursive_mutex> lock(logMutex);
    logs.push_back(msg);
  }
};

// Base mission state
struct MissionState : public StateMachine<MissionStateID>::State {
  std::string name;
  MissionState(std::string n) : name(n) {}

  void log(const std::string &msg) {
    auto ctx = getContext<MissionContext>();
    ctx->addLog("[" + name + "] " + msg);
  }
};

struct IdleState : public MissionState {
  IdleState() : MissionState("Idle") {}
  bool enter() override {
    log("System Idle");
    return true;
  }
};

struct ArmingState : public MissionState {
  ArmingState() : MissionState("Arming") {}
  bool enter() override {
    log("Arming motors...");
    std::this_thread::sleep_for(
        std::chrono::milliseconds(50)); // Simluate hardware delay
    auto ctx = getContext<MissionContext>();
    ctx->isArmed = true;
    return true;
  }
  void update() override {
    changeToState(MissionStateID::TAKEOFF, "Armed successfully");
  }
};

struct TakeoffState : public MissionState {
  TakeoffState() : MissionState("Takeoff") {}
  bool enter() override {
    log("Taking off...");
    return true;
  }
  void update() override {
    auto ctx = getContext<MissionContext>();
    ctx->altitude = ctx->altitude + 0.5;
    // Simulate heavy control loop
    std::this_thread::sleep_for(std::chrono::milliseconds(10));

    if (ctx->altitude >= 5.0) {
      changeToState(MissionStateID::WAYPOINTS, "Target altitude reached");
    }
  }
};

struct WaypointsState : public MissionState {
  WaypointsState() : MissionState("Waypoints") {}
  void update() override {
    auto ctx = getContext<MissionContext>();
    // Simulate flying to waypoints
    std::this_thread::sleep_for(std::chrono::milliseconds(20)); // Flight time
    ctx->currentWaypoint++;
    log("Reached waypoint " + std::to_string(ctx->currentWaypoint));

    if (ctx->currentWaypoint >= 3) {
      changeToState(MissionStateID::RTL, "Mission complete");
    }
  }
};

struct RTLState : public MissionState {
  RTLState() : MissionState("RTL") {}
  void update() override {
    auto ctx = getContext<MissionContext>();
    // Simulate flying home
    log("Returning home...");
    changeToState(MissionStateID::LANDING, "Home reached");
  }
};

struct LandingState : public MissionState {
  LandingState() : MissionState("Landing") {}
  void update() override {
    auto ctx = getContext<MissionContext>();
    ctx->altitude = ctx->altitude - 0.5;
    std::this_thread::sleep_for(std::chrono::milliseconds(10));

    if (ctx->altitude <= 0.1) {
      ctx->altitude = 0.0;
      ctx->isArmed = false;
      changeToState(MissionStateID::LANDED, "Touchdown");
    }
  }
};

struct LandedState : public MissionState {
  LandedState() : MissionState("Landed") {}
  bool enter() override {
    log("Landed safely");
    return true;
  }
};

class ComplexMissionTest : public ::testing::Test {
protected:
  void SetUp() override {
    sm = std::make_unique<StateMachine<MissionStateID>>(MissionStateID::IDLE,
                                                        "MissionSM");
    context = std::make_shared<MissionContext>();

    sm->withContext(context);

    sm->addState(MissionStateID::IDLE, "Idle", std::make_unique<IdleState>())
        .addState(MissionStateID::ARMING, "Arming",
                  std::make_unique<ArmingState>())
        .addState(MissionStateID::TAKEOFF, "Takeoff",
                  std::make_unique<TakeoffState>())
        .addState(MissionStateID::WAYPOINTS, "Waypoints",
                  std::make_unique<WaypointsState>())
        .addState(MissionStateID::RTL, "RTL", std::make_unique<RTLState>())
        .addState(MissionStateID::LANDING, "Landing",
                  std::make_unique<LandingState>())
        .addState(MissionStateID::LANDED, "Landed",
                  std::make_unique<LandedState>());

    sm->start();
  }

  std::unique_ptr<StateMachine<MissionStateID>> sm;
  std::shared_ptr<MissionContext> context;
};

TEST_F(ComplexMissionTest, RealTimeMissionSim) {
  // 1. Start Mission
  sm->changeState(MissionStateID::ARMING, "User command");

  // 2. Run simulation loop
  auto startTime = std::chrono::steady_clock::now();
  bool missionComplete = false;

  while (std::chrono::steady_clock::now() - startTime <
         std::chrono::seconds(5)) {
    sm->update();

    // Use recursive mutex for history access just to be safe in test
    // environment though update is sync here.
    if (sm->getCurrentStateId() == MissionStateID::LANDED) {
      missionComplete = true;
      break;
    }

    std::this_thread::sleep_for(std::chrono::milliseconds(10));
  }

  EXPECT_TRUE(missionComplete);
  EXPECT_EQ(context->isArmed, false);
  EXPECT_EQ(context->currentWaypoint, 3);
  EXPECT_NEAR(context->altitude, 0.0, 0.1);

  // Verify sequence
  auto history = sm->getStateHistory();
  std::vector<MissionStateID> expected = {
      MissionStateID::IDLE,    MissionStateID::ARMING,
      MissionStateID::TAKEOFF, MissionStateID::WAYPOINTS,
      MissionStateID::RTL,     MissionStateID::LANDING,
      MissionStateID::LANDED};

  size_t expectedIdx = 0;
  for (const auto &state : history) {
    if (expectedIdx < expected.size() && state == expected[expectedIdx]) {
      expectedIdx++;
    }
  }

  EXPECT_EQ(expectedIdx, expected.size())
      << "Did not see all expected states in order";
}

// ============================================================================
// MULTI-THREADED STRESS TESTS
// ============================================================================
// These tests simulate concurrent ROS callbacks and verify thread safety

class MultiThreadedStressTest : public ::testing::Test {
protected:
  void SetUp() override {
    sm = std::make_unique<StateMachine<TestStateID>>(TestStateID::IDLE,
                                                     "StressMachine");

    sm->addState(TestStateID::IDLE, "Idle", std::make_unique<TestState>("Idle"))
        .addState(TestStateID::RUNNING, "Running",
                  std::make_unique<TestState>("Running"))
        .addState(TestStateID::PAUSED, "Paused",
                  std::make_unique<TestState>("Paused"))
        .addState(TestStateID::ERROR, "Error",
                  std::make_unique<TestState>("Error"))
        .addState(TestStateID::FALLBACK, "Fallback",
                  std::make_unique<TestState>("Fallback"));

    sm->withFallback(TestStateID::FALLBACK);
    sm->start();
  }

  std::unique_ptr<StateMachine<TestStateID>> sm;
};

// Stress test: Multiple threads hammering state changes simultaneously
TEST_F(MultiThreadedStressTest, ConcurrentStateChangesFromMultipleThreads) {
  constexpr int NUM_THREADS = 8;
  constexpr int ITERATIONS_PER_THREAD = 500;

  std::atomic<int> successfulTransitions{0};
  std::atomic<int> failedTransitions{0};
  std::atomic<bool> crashed{false};

  std::vector<TestStateID> states = {TestStateID::IDLE, TestStateID::RUNNING,
                                     TestStateID::PAUSED, TestStateID::ERROR};

  auto worker = [&](int threadId) {
    try {
      for (int i = 0; i < ITERATIONS_PER_THREAD && !crashed; ++i) {
        TestStateID targetState = states[(threadId + i) % states.size()];
        bool result =
            sm->changeState(targetState, "Thread " + std::to_string(threadId));

        if (result) {
          successfulTransitions++;
        } else {
          failedTransitions++;
        }

        // Tiny sleep to increase interleaving
        if (i % 10 == 0) {
          std::this_thread::yield();
        }
      }
    } catch (const std::exception &e) {
      crashed = true;
      FAIL() << "Thread " << threadId << " crashed: " << e.what();
    }
  };

  std::vector<std::thread> threads;
  for (int i = 0; i < NUM_THREADS; ++i) {
    threads.emplace_back(worker, i);
  }

  for (auto &t : threads) {
    t.join();
  }

  EXPECT_FALSE(crashed);
  EXPECT_GT(successfulTransitions.load(), 0);

  // Verify state machine is still functional
  bool finalTransition = sm->changeState(TestStateID::IDLE, "Final check");
  EXPECT_TRUE(finalTransition || sm->getCurrentStateId() == TestStateID::IDLE);
}

// Stress test: Concurrent updates and state changes (simulates timer + topic
// callbacks)
TEST_F(MultiThreadedStressTest, ConcurrentUpdatesAndStateChanges) {
  constexpr int NUM_UPDATE_THREADS = 4;
  constexpr int NUM_CHANGE_THREADS = 4;
  constexpr auto TEST_DURATION = std::chrono::milliseconds(500);

  std::atomic<bool> stop{false};
  std::atomic<int> updateCount{0};
  std::atomic<int> changeCount{0};
  std::atomic<bool> crashed{false};

  std::vector<TestStateID> states = {TestStateID::IDLE, TestStateID::RUNNING,
                                     TestStateID::PAUSED};

  // Update threads (simulate timer callbacks)
  auto updateWorker = [&](int threadId) {
    try {
      while (!stop && !crashed) {
        sm->update();
        updateCount++;
        std::this_thread::sleep_for(std::chrono::microseconds(100));
      }
    } catch (const std::exception &e) {
      crashed = true;
      FAIL() << "Update thread " << threadId << " crashed: " << e.what();
    }
  };

  // State change threads (simulate topic/service callbacks)
  auto changeWorker = [&](int threadId) {
    try {
      int i = 0;
      while (!stop && !crashed) {
        TestStateID targetState = states[(threadId + i++) % states.size()];
        sm->changeState(targetState,
                        "ChangeThread " + std::to_string(threadId));
        changeCount++;
        std::this_thread::sleep_for(std::chrono::microseconds(500));
      }
    } catch (const std::exception &e) {
      crashed = true;
      FAIL() << "Change thread " << threadId << " crashed: " << e.what();
    }
  };

  std::vector<std::thread> threads;

  for (int i = 0; i < NUM_UPDATE_THREADS; ++i) {
    threads.emplace_back(updateWorker, i);
  }
  for (int i = 0; i < NUM_CHANGE_THREADS; ++i) {
    threads.emplace_back(changeWorker, i);
  }

  std::this_thread::sleep_for(TEST_DURATION);
  stop = true;

  for (auto &t : threads) {
    t.join();
  }

  EXPECT_FALSE(crashed);
  EXPECT_GT(updateCount.load(), 100);
  EXPECT_GT(changeCount.load(), 10);

  // Verify state machine integrity
  auto currentState = sm->getCurrentStateId();
  EXPECT_TRUE(currentState == TestStateID::IDLE ||
              currentState == TestStateID::RUNNING ||
              currentState == TestStateID::PAUSED);
}

// Stress test: Callback registration during state changes
TEST_F(MultiThreadedStressTest, ConcurrentCallbacksAndStateChanges) {
  constexpr int NUM_THREADS = 6;
  constexpr auto TEST_DURATION = std::chrono::milliseconds(300);

  std::atomic<bool> stop{false};
  std::atomic<int> callbackInvocations{0};
  std::atomic<bool> crashed{false};

  // Register callback that does work
  sm->onStateChanged([&](const TestStateID &, const TestStateID &,
                         std::string_view, std::string_view, std::string_view) {
    callbackInvocations++;
    // Simulate callback doing some work
    std::this_thread::sleep_for(std::chrono::microseconds(50));
  });

  std::vector<TestStateID> states = {TestStateID::IDLE, TestStateID::RUNNING,
                                     TestStateID::PAUSED, TestStateID::ERROR};

  auto worker = [&](int threadId) {
    try {
      int i = 0;
      while (!stop && !crashed) {
        TestStateID targetState = states[(threadId + i++) % states.size()];
        sm->changeState(targetState,
                        "CallbackTest " + std::to_string(threadId));

        // Also call update to stress further
        sm->update();

        // Query state (read operation)
        auto _ = sm->getCurrentStateId();
        auto __ = sm->getCurrentStateName();
        auto ___ = sm->getStateHistory();
        (void)_;
        (void)__;
        (void)___;

        std::this_thread::yield();
      }
    } catch (const std::exception &e) {
      crashed = true;
      FAIL() << "Thread " << threadId << " crashed: " << e.what();
    }
  };

  std::vector<std::thread> threads;
  for (int i = 0; i < NUM_THREADS; ++i) {
    threads.emplace_back(worker, i);
  }

  std::this_thread::sleep_for(TEST_DURATION);
  stop = true;

  for (auto &t : threads) {
    t.join();
  }

  EXPECT_FALSE(crashed);
  EXPECT_GT(callbackInvocations.load(), 0);
}

// Stress test: Rapid fire state changes with slow enter/exit
TEST_F(MultiThreadedStressTest, RapidStateChangesWithSlowCallbacks) {
  // Create a new state machine with slow states
  struct SlowEnterExitState : public TestState {
    std::atomic<int> concurrentEnters{0};
    std::atomic<int> concurrentExits{0};

    SlowEnterExitState(std::string n) : TestState(n) {}

    bool enter() override {
      int prev = concurrentEnters.fetch_add(1);
      EXPECT_EQ(prev, 0) << "Concurrent enter detected!";

      std::this_thread::sleep_for(std::chrono::milliseconds(5));
      TestState::enter();

      concurrentEnters--;
      return true;
    }

    void exit() override {
      int prev = concurrentExits.fetch_add(1);
      EXPECT_EQ(prev, 0) << "Concurrent exit detected!";

      std::this_thread::sleep_for(std::chrono::milliseconds(5));
      TestState::exit();

      concurrentExits--;
    }
  };

  auto slowSm = std::make_unique<StateMachine<TestStateID>>(TestStateID::IDLE,
                                                            "SlowMachine");

  slowSm
      ->addState(TestStateID::IDLE, "Idle",
                 std::make_unique<SlowEnterExitState>("Idle"))
      .addState(TestStateID::RUNNING, "Running",
                std::make_unique<SlowEnterExitState>("Running"))
      .addState(TestStateID::PAUSED, "Paused",
                std::make_unique<SlowEnterExitState>("Paused"));

  slowSm->start();

  constexpr int NUM_THREADS = 4;
  constexpr int ITERATIONS = 50;

  std::atomic<bool> crashed{false};

  auto worker = [&](int threadId) {
    try {
      std::vector<TestStateID> targets = {
          TestStateID::IDLE, TestStateID::RUNNING, TestStateID::PAUSED};

      for (int i = 0; i < ITERATIONS && !crashed; ++i) {
        TestStateID target = targets[(threadId + i) % targets.size()];
        slowSm->changeState(target, "SlowTest");
      }
    } catch (const std::exception &e) {
      crashed = true;
      FAIL() << "Thread " << threadId << " crashed: " << e.what();
    }
  };

  std::vector<std::thread> threads;
  for (int i = 0; i < NUM_THREADS; ++i) {
    threads.emplace_back(worker, i);
  }

  for (auto &t : threads) {
    t.join();
  }

  EXPECT_FALSE(crashed);
}

// Stress test: History access during rapid state changes
TEST_F(MultiThreadedStressTest, ConcurrentHistoryAccessDuringTransitions) {
  constexpr int NUM_WRITERS = 3;
  constexpr int NUM_READERS = 3;
  constexpr auto TEST_DURATION = std::chrono::milliseconds(300);

  std::atomic<bool> stop{false};
  std::atomic<bool> crashed{false};
  std::atomic<int> readCount{0};

  std::vector<TestStateID> states = {TestStateID::IDLE, TestStateID::RUNNING,
                                     TestStateID::PAUSED};

  // Writer threads - change states
  auto writer = [&](int threadId) {
    try {
      int i = 0;
      while (!stop && !crashed) {
        TestStateID target = states[(threadId + i++) % states.size()];
        sm->changeState(target, "HistoryWriter");
        std::this_thread::yield();
      }
    } catch (const std::exception &e) {
      crashed = true;
      FAIL() << "Writer " << threadId << " crashed: " << e.what();
    }
  };

  // Reader threads - read history
  auto reader = [&](int threadId) {
    try {
      while (!stop && !crashed) {
        auto history = sm->getStateHistory();
        readCount++;

        // Verify history is not corrupted
        EXPECT_FALSE(history.empty());

        // Access all elements
        for (const auto &state : history) {
          (void)state;
        }

        std::this_thread::yield();
      }
    } catch (const std::exception &e) {
      crashed = true;
      FAIL() << "Reader " << threadId << " crashed: " << e.what();
    }
  };

  std::vector<std::thread> threads;

  for (int i = 0; i < NUM_WRITERS; ++i) {
    threads.emplace_back(writer, i);
  }
  for (int i = 0; i < NUM_READERS; ++i) {
    threads.emplace_back(reader, i);
  }

  std::this_thread::sleep_for(TEST_DURATION);
  stop = true;

  for (auto &t : threads) {
    t.join();
  }

  EXPECT_FALSE(crashed);
  EXPECT_GT(readCount.load(), 100);
}

// Stress test: Simulates ROS2 multi-threaded executor with various callbacks
TEST_F(MultiThreadedStressTest, SimulatedROS2MultiThreadedExecutor) {
  constexpr auto TEST_DURATION = std::chrono::milliseconds(500);

  std::atomic<bool> stop{false};
  std::atomic<bool> crashed{false};

  // Simulate timer callback (high frequency update)
  auto timerCallback = [&]() {
    try {
      while (!stop && !crashed) {
        sm->update();
        std::this_thread::sleep_for(std::chrono::milliseconds(10)); // 100Hz
      }
    } catch (const std::exception &e) {
      crashed = true;
      FAIL() << "Timer callback crashed: " << e.what();
    }
  };

  // Simulate topic callback (state change trigger)
  auto topicCallback = [&]() {
    try {
      std::vector<TestStateID> states = {TestStateID::RUNNING,
                                         TestStateID::PAUSED};
      int i = 0;
      while (!stop && !crashed) {
        sm->changeState(states[i++ % states.size()], "Topic callback");
        std::this_thread::sleep_for(std::chrono::milliseconds(50));
      }
    } catch (const std::exception &e) {
      crashed = true;
      FAIL() << "Topic callback crashed: " << e.what();
    }
  };

  // Simulate service callback (query state)
  auto serviceCallback = [&]() {
    try {
      while (!stop && !crashed) {
        auto state = sm->getCurrentStateId();
        auto name = sm->getCurrentStateName();
        auto history = sm->getStateHistory();
        auto ready = sm->isReady();
        (void)state;
        (void)name;
        (void)history;
        (void)ready;
        std::this_thread::sleep_for(std::chrono::milliseconds(20));
      }
    } catch (const std::exception &e) {
      crashed = true;
      FAIL() << "Service callback crashed: " << e.what();
    }
  };

  // Simulate action callback (complex state sequence)
  auto actionCallback = [&]() {
    try {
      while (!stop && !crashed) {
        sm->changeState(TestStateID::IDLE, "Action start");
        std::this_thread::sleep_for(std::chrono::milliseconds(30));
        sm->changeState(TestStateID::RUNNING, "Action execute");
        std::this_thread::sleep_for(std::chrono::milliseconds(30));
        sm->changeState(TestStateID::IDLE, "Action complete");
        std::this_thread::sleep_for(std::chrono::milliseconds(100));
      }
    } catch (const std::exception &e) {
      crashed = true;
      FAIL() << "Action callback crashed: " << e.what();
    }
  };

  std::vector<std::thread> threads;

  // Multiple instances of each callback type (like ROS2 MultiThreadedExecutor)
  threads.emplace_back(timerCallback);
  threads.emplace_back(timerCallback);
  threads.emplace_back(topicCallback);
  threads.emplace_back(topicCallback);
  threads.emplace_back(serviceCallback);
  threads.emplace_back(serviceCallback);
  threads.emplace_back(actionCallback);

  std::this_thread::sleep_for(TEST_DURATION);
  stop = true;

  for (auto &t : threads) {
    t.join();
  }

  EXPECT_FALSE(crashed);
  EXPECT_TRUE(sm->isReady());
}

// Stress test: State change while previous state's update() is still running
// This simulates: Thread A calls update() on StateX, Thread B changes to StateY
// mid-execution
TEST_F(MultiThreadedStressTest, StateChangeWhilePreviousStateCallbackRunning) {
  struct LongRunningUpdateState : public TestState {
    std::atomic<bool> updateStarted{false};
    std::atomic<bool> updateFinished{false};
    std::atomic<bool> wasInterrupted{false};
    std::atomic<int> updateRunningCount{0};

    LongRunningUpdateState(std::string n) : TestState(n) {}

    void update() override {
      int running = updateRunningCount.fetch_add(1);
      if (running > 0) {
        wasInterrupted = true; // Multiple concurrent updates detected
      }

      updateStarted = true;
      // Simulate long-running callback (e.g., sensor processing)
      std::this_thread::sleep_for(std::chrono::milliseconds(50));
      TestState::update();
      updateFinished = true;

      updateRunningCount--;
    }
  };

  auto longSm = std::make_unique<StateMachine<TestStateID>>(
      TestStateID::IDLE, "LongCallbackMachine");

  auto longState = std::make_unique<LongRunningUpdateState>("LongUpdate");
  auto *longStatePtr = longState.get();

  longSm->addState(TestStateID::IDLE, "LongUpdate", std::move(longState))
      .addState(TestStateID::RUNNING, "Running",
                std::make_unique<TestState>("Running"))
      .addState(TestStateID::PAUSED, "Paused",
                std::make_unique<TestState>("Paused"));

  longSm->start();

  std::atomic<bool> crashed{false};

  // Thread 1: Start update on IDLE state (long running)
  std::thread updateThread([&]() {
    try {
      longSm->update(); // This takes 50ms
    } catch (const std::exception &e) {
      crashed = true;
      FAIL() << "Update thread crashed: " << e.what();
    }
  });

  // Wait for update to start
  while (!longStatePtr->updateStarted && !crashed) {
    std::this_thread::sleep_for(std::chrono::microseconds(100));
  }

  // Thread 2 (main): Change state while update is running
  std::thread changeThread([&]() {
    try {
      // Change state while IDLE's update() is mid-execution
      longSm->changeState(TestStateID::RUNNING, "Interrupt update");
    } catch (const std::exception &e) {
      crashed = true;
      FAIL() << "Change thread crashed: " << e.what();
    }
  });

  updateThread.join();
  changeThread.join();

  EXPECT_FALSE(crashed);
  // State machine should be in a valid state
  auto finalState = longSm->getCurrentStateId();
  EXPECT_TRUE(finalState == TestStateID::IDLE ||
              finalState == TestStateID::RUNNING);
}

// Stress test: Callback triggers state change while another callback is pending
TEST_F(MultiThreadedStressTest,
       CallbackTriggersStateChangeWhileAnotherCallbackPending) {
  struct CallbackTriggeringState : public TestState {
    std::atomic<int> nestedChangeAttempts{0};

    CallbackTriggeringState(std::string n) : TestState(n) {}

    void update() override {
      TestState::update();
      // Simulate callback that triggers another state change
      nestedChangeAttempts++;
      changeToState(TestStateID::PAUSED, "Nested from update");
    }
  };

  auto nestedSm = std::make_unique<StateMachine<TestStateID>>(TestStateID::IDLE,
                                                              "NestedMachine");

  nestedSm
      ->addState(TestStateID::IDLE, "Idle", std::make_unique<TestState>("Idle"))
      .addState(TestStateID::RUNNING, "Running",
                std::make_unique<CallbackTriggeringState>("Running"))
      .addState(TestStateID::PAUSED, "Paused",
                std::make_unique<TestState>("Paused"));

  nestedSm->start();
  nestedSm->changeState(TestStateID::RUNNING, "Start");

  std::atomic<bool> stop{false};
  std::atomic<bool> crashed{false};

  // Thread 1: Repeatedly call update (which triggers nested state change)
  auto updateWorker = [&]() {
    try {
      while (!stop && !crashed) {
        nestedSm->update();
        std::this_thread::yield();
      }
    } catch (const std::exception &e) {
      crashed = true;
      FAIL() << "Update worker crashed: " << e.what();
    }
  };

  // Thread 2: Externally change state
  auto changeWorker = [&]() {
    try {
      while (!stop && !crashed) {
        nestedSm->changeState(TestStateID::RUNNING, "External change");
        std::this_thread::sleep_for(std::chrono::milliseconds(5));
      }
    } catch (const std::exception &e) {
      crashed = true;
      FAIL() << "Change worker crashed: " << e.what();
    }
  };

  std::thread t1(updateWorker);
  std::thread t2(changeWorker);
  std::thread t3(changeWorker); // Multiple external changers

  std::this_thread::sleep_for(std::chrono::milliseconds(300));
  stop = true;

  t1.join();
  t2.join();
  t3.join();

  EXPECT_FALSE(crashed);
}

// Stress test: exit() callback of state A runs while enter() of state B is
// called from another thread
TEST_F(MultiThreadedStressTest, ExitAndEnterRaceCondition) {
  struct SlowExitState : public TestState {
    std::atomic<bool> exitStarted{false};
    std::atomic<bool> exitFinished{false};

    SlowExitState(std::string n) : TestState(n) {}

    void exit() override {
      exitStarted = true;
      std::this_thread::sleep_for(std::chrono::milliseconds(30));
      TestState::exit();
      exitFinished = true;
    }
  };

  struct SlowEnterState : public TestState {
    std::atomic<bool> enterStarted{false};
    std::atomic<bool> enterFinished{false};

    SlowEnterState(std::string n) : TestState(n) {}

    bool enter() override {
      enterStarted = true;
      std::this_thread::sleep_for(std::chrono::milliseconds(30));
      bool result = TestState::enter();
      enterFinished = true;
      return result;
    }
  };

  std::atomic<bool> crashed{false};
  constexpr int ITERATIONS = 20;

  for (int iter = 0; iter < ITERATIONS && !crashed; ++iter) {
    auto raceSm = std::make_unique<StateMachine<TestStateID>>(TestStateID::IDLE,
                                                              "RaceMachine");

    auto slowExit = std::make_unique<SlowExitState>("SlowExit");
    auto slowEnter = std::make_unique<SlowEnterState>("SlowEnter");

    raceSm->addState(TestStateID::IDLE, "SlowExit", std::move(slowExit))
        .addState(TestStateID::RUNNING, "SlowEnter", std::move(slowEnter))
        .addState(TestStateID::PAUSED, "Paused",
                  std::make_unique<TestState>("Paused"));

    raceSm->start();

    // Multiple threads try to change state simultaneously
    std::vector<std::thread> threads;
    for (int i = 0; i < 4; ++i) {
      threads.emplace_back([&, i]() {
        try {
          TestStateID target =
              (i % 2 == 0) ? TestStateID::RUNNING : TestStateID::PAUSED;
          raceSm->changeState(target, "Race " + std::to_string(i));
        } catch (const std::exception &e) {
          crashed = true;
          FAIL() << "Thread " << i << " crashed: " << e.what();
        }
      });
    }

    for (auto &t : threads) {
      t.join();
    }

    // Verify state machine is still consistent
    EXPECT_TRUE(raceSm->isReady());
  }

  EXPECT_FALSE(crashed);
}

TEST_F(StateMachineTest, TransitionFromExitTrap) {
  struct ExitTransitionState : public TestState {
    ExitTransitionState() : TestState("ExitTrans") {}
    int exitCalls = 0;

    void exit() override {
      exitCalls++;
      if (exitCalls == 1) {
        // Dangerous: Trigger transition during exit
        // This typically results in "Error" being entered,
        // then immediately overwritten by the original target state
        changeToState(TestStateID::ERROR, "Panic in exit");
      }
    }
  };

  auto riskyState = std::make_unique<ExitTransitionState>();
  auto *riskyPtr = riskyState.get();
  auto errorState = std::make_unique<TestState>("Error");
  auto *errorPtr = errorState.get();

  sm->addState(TestStateID::IDLE, "Start", std::move(riskyState))
      .addState(TestStateID::RUNNING, "Target",
                std::make_unique<TestState>("Target"))
      .addState(TestStateID::ERROR, "Error", std::move(errorState));

  sm->start();

  // Trigger transition IDLE -> RUNNING
  // IDLE exit will trigger IDLE -> ERROR
  sm->changeState(TestStateID::RUNNING, "Go");

  // VERIFICATION:
  // With deferred exit transitions, the nested transition from exit() is
  // executed after the original transition completes, so no state is leaked.

  // 1. Exit runs once for the original IDLE -> RUNNING transition
  EXPECT_EQ(riskyPtr->exitCalls, 1)
      << "Exit is called once for the original transition";

  // 2. The deferred ERROR transition fires after RUNNING is entered,
  //    so the machine ends up in ERROR
  EXPECT_EQ(sm->getCurrentStateId(), TestStateID::ERROR);

  // 3. Error was entered by the deferred transition
  EXPECT_EQ(errorPtr->enterCount, 1);

  // 4. Error is the current state, so it has not been exited
  EXPECT_EQ(errorPtr->exitCount, 0);
}

TEST_F(StateMachineTest, ZombieStateHijacking) {
  // A state that holds a reference to the machine and acts after it's "dead"
  struct ZombieState : public TestState {
    ZombieState() : TestState("Zombie") {}
    std::thread zombieThread;

    void exit() override {
      // detach a thread that sleeps then attacks
      zombieThread = std::thread([this]() {
        std::this_thread::sleep_for(std::chrono::milliseconds(50));
        // I am dead, but I will force a transition!
        this->changeToState(TestStateID::ERROR, "BRAINS...");
      });
      zombieThread.detach();
    }
  };

  sm->addState(TestStateID::IDLE, "ZombieBase", std::make_unique<ZombieState>())
      .addState(TestStateID::RUNNING, "SafeZone",
                std::make_unique<TestState>("SafeZone"))
      .addState(TestStateID::ERROR, "Infected",
                std::make_unique<TestState>("Infected"));

  sm->start();
  sm->changeState(TestStateID::RUNNING, "Escape zombie");

  // We are now safely in RUNNING
  EXPECT_EQ(sm->getCurrentStateId(), TestStateID::RUNNING);

  // Wait for zombie attack
  std::this_thread::sleep_for(std::chrono::milliseconds(100));

  // DECISION: Should a dead state be allowed to change the state?
  // Current impl allows it. If this is undesirable, this test fails.
  if (sm->getCurrentStateId() == TestStateID::ERROR) {
    // Document this behavior: "Any pointer to a state can drive the machine"
    SUCCEED();
  } else {
    FAIL()
        << "Zombie prevented from changing state (Unexpected per current code)";
  }
}

TEST_F(StateMachineTest, SelfTransitionBehavior) {
  auto resetState = std::make_unique<TestState>("Resetable");
  auto *ptr = resetState.get();

  sm->addState(TestStateID::IDLE, "Idle", std::move(resetState));
  sm->start();

  EXPECT_EQ(ptr->enterCount, 1);

  // Request self-transition
  sm->changeState(TestStateID::IDLE, "Self Reset");

  // Current Implementation: Ignores it
  EXPECT_EQ(ptr->exitCount, 0)
      << "Self-transition caused exit (unexpected for this impl)";
  EXPECT_EQ(ptr->enterCount, 1)
      << "Self-transition caused re-enter (unexpected for this impl)";
}

// ============================================================================
// EDGE CASES AND ERROR HANDLING
// ============================================================================
// Tests for boundary conditions, error scenarios, and exceptional cases

// MISSING: Fallback state doesn't exist
TEST(EdgeCase, FallbackStateNotRegistered) {
  StateMachine<TestStateID> sm(TestStateID::IDLE, "Test");
  sm.addState(TestStateID::IDLE, "Idle", std::make_unique<TestState>("Idle"));
  sm.withFallback(TestStateID::ERROR); // ERROR state not added!
  sm.start();

  auto badState = std::make_unique<TestState>("Bad");
  badState->shouldFailEnter = true;
  sm.addState(TestStateID::RUNNING, "Running", std::move(badState));

  sm.changeState(TestStateID::RUNNING); // Should fallback to non-existent ERROR
  // Expected: Error callback should fire, log error (line 214-220)
  // But validate() check (line 704-707) would catch this
}

// MISSING: Fallback to self (infinite loop protection)
TEST(EdgeCase, FallbackToFailingState) {
  StateMachine<TestStateID> sm(TestStateID::IDLE, "Test");

  auto failState = std::make_unique<TestState>("Fail");
  failState->shouldFailEnter = true;

  sm.addState(TestStateID::IDLE, "Idle", std::make_unique<TestState>("Idle"));
  sm.addState(TestStateID::ERROR, "Error", std::move(failState));
  sm.withFallback(TestStateID::ERROR); // Fallback also fails!
  sm.start();

  auto badState = std::make_unique<TestState>("Bad");
  badState->shouldFailEnter = true;
  sm.addState(TestStateID::RUNNING, "Running", std::move(badState));

  bool result = sm.changeState(TestStateID::RUNNING);
  (void)result;
  // Expected: Line 207 prevents fallback to same state
  // But what if fallback itself fails? No protection!
}

// MISSING: Circular fallback chain
// While code prevents same-state fallback (line 207), it doesn't prevent:
// State A fails -> Fallback B fails -> No second-level fallback

// MISSING: Exception in exit() callback
TEST(EdgeCase, ExceptionInExit) {
  struct ThrowingExitState : public TestState {
    ThrowingExitState() : TestState("ThrowingExit") {}
    void exit() override { throw std::runtime_error("Exit failed!"); }
  };

  StateMachine<TestStateID> sm(TestStateID::IDLE, "Test");
  sm.addState(TestStateID::IDLE, "Idle", std::make_unique<ThrowingExitState>());
  sm.addState(TestStateID::RUNNING, "Running",
              std::make_unique<TestState>("Running"));
  sm.start();

  bool result = sm.changeState(TestStateID::RUNNING);
  (void)result;
  // Line 273-276 catches exception, but what's the state after?
  // Current state already changed to RUNNING (line 279)
  // But old state's exit() threw - is cleanup incomplete?
}

// MISSING: Exception in update() callback
TEST(EdgeCase, ExceptionInUpdate) {
  struct ThrowingUpdateState : public TestState {
    ThrowingUpdateState() : TestState("ThrowingUpdate") {}
    void update() override { throw std::runtime_error("Update failed!"); }
  };

  StateMachine<TestStateID> sm(TestStateID::IDLE, "Test");
  sm.addState(TestStateID::IDLE, "Idle",
              std::make_unique<ThrowingUpdateState>());
  sm.start();

  // Should not crash - exception is caught internally
  EXPECT_NO_THROW(sm.update());

  // State machine should still be functional
  EXPECT_TRUE(sm.isReady());
  EXPECT_EQ(sm.getCurrentStateId(), TestStateID::IDLE);
}

// MISSING: Exception in state change callback
TEST(EdgeCase, ExceptionInStateChangeCallback) {
  StateMachine<TestStateID> sm(TestStateID::IDLE, "Test");
  sm.addState(TestStateID::IDLE, "Idle", std::make_unique<TestState>("Idle"));
  sm.addState(TestStateID::RUNNING, "Running",
              std::make_unique<TestState>("Running"));

  sm.onStateChanged([](auto, auto, auto, auto, auto) {
    throw std::runtime_error("Callback explosion!");
  });

  sm.start();

  // Transition should succeed despite callback throwing
  EXPECT_NO_THROW(sm.changeState(TestStateID::RUNNING));
  EXPECT_EQ(sm.getCurrentStateId(), TestStateID::RUNNING);
}

// MISSING: Out of memory during state creation
TEST(EdgeCase, OutOfMemoryDuringStateCreation) {
  // This test documents that bad_alloc is not caught
  // If make_unique throws bad_alloc, it propagates to caller
  // This is expected behavior - memory exhaustion should not be hidden
  StateMachine<TestStateID> sm(TestStateID::IDLE, "Test");

  // Normal allocation should work
  EXPECT_NO_THROW(sm.addState(TestStateID::IDLE, "Idle",
                              std::make_unique<TestState>("Idle")));
}

// MISSING: Transition to same state
TEST(EdgeCase, TransitionToSameState) {
  StateMachine<TestStateID> sm(TestStateID::IDLE, "Test");
  sm.addState(TestStateID::IDLE, "Idle", std::make_unique<TestState>("Idle"));
  sm.start();

  bool result = sm.changeState(TestStateID::IDLE);
  (void)result;
  // Expected: Returns true, logs DEBUG, no enter/exit called
}

// MISSING: Very rapid state changes (stress test)
TEST(EdgeCase, RapidStateOscillation) {
  StateMachine<TestStateID> sm(TestStateID::IDLE, "Test");
  sm.addState(TestStateID::IDLE, "Idle", std::make_unique<TestState>("Idle"));
  sm.addState(TestStateID::RUNNING, "Running",
              std::make_unique<TestState>("Running"));
  sm.start();

  // Rapid oscillation between two states
  for (int i = 0; i < 1000; i++) {
    sm.changeState((i % 2) ? TestStateID::IDLE : TestStateID::RUNNING);
  }

  // Verify state machine is still functional
  EXPECT_TRUE(sm.isReady());
  auto history = sm.getStateHistory();
  EXPECT_FALSE(history.empty());

  // History should be capped (default is typically 100 or similar)
  // Exact size depends on implementation
  EXPECT_GT(history.size(), 0);
}

// MISSING: State change from destructor
TEST(EdgeCase, StateChangeFromDestructor) {
  // This test documents that state changes from destructors are DANGEROUS
  // and should be avoided. The implementation does not protect against this.
  // Attempting to do so would cause segfault or undefined behavior.

  struct SafeDestructorState : public TestState {
    SafeDestructorState() : TestState("SafeDestructor") {}
    ~SafeDestructorState() {
      // DO NOT call changeToState() here - it's unsafe!
      // This test just documents the danger
    }
  };

  auto sm =
      std::make_unique<StateMachine<TestStateID>>(TestStateID::IDLE, "Test");
  sm->addState(TestStateID::IDLE, "Idle",
               std::make_unique<SafeDestructorState>());
  sm->addState(TestStateID::RUNNING, "Running",
               std::make_unique<TestState>("Running"));
  sm->start();

  // This is safe - no state change in destructor
  sm->changeState(TestStateID::RUNNING);
  EXPECT_EQ(sm->getCurrentStateId(), TestStateID::RUNNING);
}

// MISSING: History overflow behavior
TEST(EdgeCase, HistoryOverflow) {
  StateMachine<TestStateID> sm(TestStateID::IDLE, "Test");
  sm.withHistorySize(10);
  sm.addState(TestStateID::IDLE, "Idle", std::make_unique<TestState>("Idle"));
  sm.addState(TestStateID::RUNNING, "Running",
              std::make_unique<TestState>("Running"));
  sm.start();

  // Make many transitions
  for (int i = 0; i < 100; i++) {
    sm.changeState((i % 2) ? TestStateID::IDLE : TestStateID::RUNNING);
  }

  auto history = sm.getStateHistory();
  // History should be capped at configured size
  EXPECT_LE(history.size(), 10);
}

// MISSING: History with same state repeated
TEST(EdgeCase, HistoryDuplicatePrevention) {
  StateMachine<TestStateID> sm(TestStateID::IDLE, "Test");
  sm.addState(TestStateID::IDLE, "Idle", std::make_unique<TestState>("Idle"));
  sm.start();

  auto historyBefore = sm.getStateHistory();
  size_t sizeBefore = historyBefore.size();

  // Self-transition should not add duplicate to history
  sm.changeState(TestStateID::IDLE);

  auto historyAfter = sm.getStateHistory();
  // History should not grow for self-transition
  EXPECT_EQ(historyAfter.size(), sizeBefore);
}

// MISSING: Zero history size
TEST(EdgeCase, ZeroHistorySize) {
  StateMachine<TestStateID> sm(TestStateID::IDLE, "Test");
  sm.withHistorySize(0);
  sm.addState(TestStateID::IDLE, "Idle", std::make_unique<TestState>("Idle"));
  sm.addState(TestStateID::RUNNING, "Running",
              std::make_unique<TestState>("Running"));
  sm.start();

  sm.changeState(TestStateID::RUNNING);

  auto history = sm.getStateHistory();
  // With size 0, history should be empty or minimal
  EXPECT_LE(history.size(), 1);
}

// MISSING: Context accessed when not set
TEST(EdgeCase, AccessNullContext) {
  StateMachine<TestStateID> sm(TestStateID::IDLE, "Test");
  sm.addState(TestStateID::IDLE, "Idle", std::make_unique<TestState>("Idle"));
  sm.start();

  // No withContext() call - should throw when accessing
  EXPECT_THROW(
      {
        auto ctx = sm.getContext<int>();
        (void)ctx;
      },
      std::runtime_error);
}

// MISSING: Context lifetime issues
TEST(EdgeCase, ContextDestroyedBeforeStateMachine) {
  StateMachine<TestStateID> sm(TestStateID::IDLE, "Test");
  sm.addState(TestStateID::IDLE, "Idle", std::make_unique<TestState>("Idle"));

  {
    auto ctx = std::make_shared<int>(42);
    sm.withContext(ctx);
  }
  // sm still holds shared_ptr, should be fine

  sm.start();
  auto ctx = sm.getContext<int>();
  EXPECT_EQ(*ctx, 42);
}

// MISSING: Multiple context types
TEST(EdgeCase, ContextTypeChange) {
  StateMachine<TestStateID> sm(TestStateID::IDLE, "Test");
  sm.addState(TestStateID::IDLE, "Idle", std::make_unique<TestState>("Idle"));

  sm.withContext(std::make_shared<int>(42));
  sm.withContext(std::make_shared<double>(3.14));

  sm.start();

  // Old context is replaced - accessing old type should fail
  EXPECT_THROW(
      {
        auto ctx = sm.getContext<int>();
        (void)ctx;
      },
      std::runtime_error);

  // New context should be accessible
  auto ctx = sm.getContext<double>();
  EXPECT_DOUBLE_EQ(*ctx, 3.14);
}

// MISSING: Context access from destructing state
TEST(EdgeCase, ContextAccessDuringDestruction) {
  // This test verifies that context can be accessed during state lifecycle
  // Note: Accessing context in destructor is generally safe but should be
  // avoided as the state machine may be in an intermediate state

  struct ContextUserState : public TestState {
    ContextUserState() : TestState("ContextUser") {}

    void exit() override {
      // Access context during exit (before destruction)
      try {
        if (getStateMachine()) {
          auto ctx = getContext<int>();
          EXPECT_EQ(*ctx, 42);
        }
      } catch (...) {
        FAIL() << "Context access failed during exit";
      }
      TestState::exit();
    }
  };

  auto sm =
      std::make_unique<StateMachine<TestStateID>>(TestStateID::IDLE, "Test");
  sm->withContext(std::make_shared<int>(42));
  sm->addState(TestStateID::IDLE, "Idle", std::make_unique<ContextUserState>());
  sm->addState(TestStateID::RUNNING, "Running",
               std::make_unique<TestState>("Running"));
  sm->start();

  // Trigger exit of ContextUserState (which accesses context)
  sm->changeState(TestStateID::RUNNING);

  EXPECT_EQ(sm->getCurrentStateId(), TestStateID::RUNNING);
}

// MISSING: State machine destruction during operation
// NOTE: This test is incomplete and dangerous - would cause undefined behavior
// TEST(EdgeCase, DestroyDuringOperation) {
//     auto sm = std::make_unique<StateMachine<TestStateID>>(TestStateID::IDLE,
//     "Test"); sm->addState(TestStateID::IDLE, "Idle",
//     std::make_unique<TestState>("Idle")); sm->start();
//     // This test would intentionally create undefined behavior
//     // Not safe to implement without proper synchronization
// }

// MISSING: Atomic operations correctness
// NOTE: This is covered by existing multi-threaded stress tests
// The stateMutex protects all state map operations

// MISSING: Move constructor behavior
// NOTE: StateMachine has deleted move constructor - cannot be moved
TEST(EdgeCase, MoveConstructor) {
  // StateMachine explicitly deletes move constructor
  // This is intentional to prevent moving state machines
  // Use shared_ptr or unique_ptr to transfer ownership instead

  auto sm =
      std::make_unique<StateMachine<TestStateID>>(TestStateID::IDLE, "SM1");
  sm->addState(TestStateID::IDLE, "Idle", std::make_unique<TestState>("Idle"));
  sm->start();

  EXPECT_TRUE(sm->isReady());

  // Transfer ownership via unique_ptr
  auto sm2 = std::move(sm);
  EXPECT_TRUE(sm2->isReady());
  EXPECT_EQ(sm2->getCurrentStateId(), TestStateID::IDLE);
}

// MISSING: Move assignment
// NOTE: StateMachine has deleted move assignment operator - cannot be moved
TEST(EdgeCase, MoveAssignment) {
  // StateMachine explicitly deletes move assignment operator
  // This is intentional to prevent moving state machines
  // Use shared_ptr or unique_ptr to manage lifetime instead

  auto sm1 =
      std::make_unique<StateMachine<TestStateID>>(TestStateID::IDLE, "SM1");
  sm1->addState(TestStateID::IDLE, "Idle", std::make_unique<TestState>("Idle"));
  sm1->start();

  // Reassign pointer (not move the state machine itself)
  sm1 =
      std::make_unique<StateMachine<TestStateID>>(TestStateID::RUNNING, "SM2");
  sm1->addState(TestStateID::RUNNING, "Running",
                std::make_unique<TestState>("Running"));
  sm1->start();

  EXPECT_TRUE(sm1->isReady());
  EXPECT_EQ(sm1->getCurrentStateId(), TestStateID::RUNNING);
}

// MISSING: Validate called at different lifecycle stages
TEST(EdgeCase, ValidateBeforeStart) {
  StateMachine<TestStateID> sm(TestStateID::IDLE, "Test");

  // No states added - should be invalid
  bool valid = sm.validate();
  EXPECT_FALSE(valid);

  // Add initial state
  sm.addState(TestStateID::IDLE, "Idle", std::make_unique<TestState>("Idle"));
  valid = sm.validate();
  EXPECT_TRUE(valid);
}

TEST(EdgeCase, ValidateAfterStart) {
  StateMachine<TestStateID> sm(TestStateID::IDLE, "Test");
  sm.addState(TestStateID::IDLE, "Idle", std::make_unique<TestState>("Idle"));
  sm.start();

  // Should still be valid after start
  bool valid = sm.validate();
  EXPECT_TRUE(valid);
}

// ============================================================================
// HIERARCHICAL STATE MACHINE TESTS
// ============================================================================

// Sub-state enum for child state machine
enum class SubStateID { SUB_A, SUB_B, SUB_C };

// Substate test state with counters
struct SubTestState : public StateMachine<SubStateID>::State {
  std::string name;
  int enterCount = 0;
  int exitCount = 0;
  int updateCount = 0;

  SubTestState(std::string n) : name(std::move(n)) {}

  bool enter() override {
    enterCount++;
    return true;
  }
  void exit() override { exitCount++; }
  void update() override { updateCount++; }
};

// Parent state with counters (for hierarchy tests)
struct ParentTestState : public StateMachine<TestStateID>::State {
  std::string name;
  int enterCount = 0;
  int exitCount = 0;
  int updateCount = 0;

  ParentTestState(std::string n) : name(std::move(n)) {}

  bool enter() override {
    enterCount++;
    return true;
  }
  void exit() override { exitCount++; }
  void update() override { updateCount++; }
};

// A parent state that uses the State convenience methods to control sub-states
struct ParentWithSubControl : public StateMachine<TestStateID>::State {
  SubStateID targetSubState{};
  bool shouldChangeSubState = false;
  bool subChangeResult = false;
  std::string lastSubName;
  bool lastHasSub = false;

  bool enter() override { return true; }
  void update() override {
    // Use the State convenience methods
    lastSubName = getActiveSubStateName();
    lastHasSub = hasActiveSubMachine();

    if (shouldChangeSubState) {
      subChangeResult = changeSubState(targetSubState, "from parent update");
      shouldChangeSubState = false;
    }
  }
  void exit() override {}
};

// ── Tests ────────────────────────────────────────────────────────────────────

TEST(HierarchicalSM, ParentAndChildLifecycleOrdering) {
  auto subA = std::make_unique<SubTestState>("SubA");
  auto subB = std::make_unique<SubTestState>("SubB");
  auto *subAPtr = subA.get();

  auto parentIdle = std::make_unique<ParentTestState>("Idle");
  auto parentRunning = std::make_unique<ParentTestState>("Running");
  auto *idlePtr = parentIdle.get();

  StateMachine<TestStateID> sm(TestStateID::IDLE, "ParentSM");
  sm.withLogLevel(StateMachine<TestStateID>::LogLevel::NONE);

  sm.addState(TestStateID::IDLE, "Idle", std::move(parentIdle))
      .addState(TestStateID::RUNNING, "Running", std::move(parentRunning))
      .addSubState(TestStateID::IDLE, SubStateID::SUB_A, "SubA",
                   std::move(subA))
      .addSubState(TestStateID::IDLE, SubStateID::SUB_B, "SubB",
                   std::move(subB));

  sm.start();

  EXPECT_TRUE(sm.isReady());
  EXPECT_EQ(idlePtr->enterCount, 1);
  EXPECT_TRUE(sm.hasActiveSubMachine());
  EXPECT_EQ(sm.getActiveSubStateName(), "SubA");
  EXPECT_EQ(subAPtr->enterCount, 1);
}

TEST(HierarchicalSM, UpdateChainsParentThenChild) {
  auto subA = std::make_unique<SubTestState>("SubA");
  auto *subAPtr = subA.get();

  auto parentIdle = std::make_unique<ParentTestState>("Idle");
  auto *idlePtr = parentIdle.get();

  StateMachine<TestStateID> sm(TestStateID::IDLE, "ParentSM");
  sm.withLogLevel(StateMachine<TestStateID>::LogLevel::NONE);

  sm.addState(TestStateID::IDLE, "Idle", std::move(parentIdle))
      .addSubState(TestStateID::IDLE, SubStateID::SUB_A, "SubA",
                   std::move(subA));
  sm.start();

  EXPECT_EQ(idlePtr->updateCount, 0);
  EXPECT_EQ(subAPtr->updateCount, 0);

  sm.update();
  EXPECT_EQ(idlePtr->updateCount, 1);
  EXPECT_EQ(subAPtr->updateCount, 1);

  sm.update();
  sm.update();
  EXPECT_EQ(idlePtr->updateCount, 3);
  EXPECT_EQ(subAPtr->updateCount, 3);
}

TEST(HierarchicalSM, ParentTransitionTearsDownChild) {
  auto subA = std::make_unique<SubTestState>("SubA");
  auto *subAPtr = subA.get();

  auto parentIdle = std::make_unique<ParentTestState>("Idle");
  auto parentRunning = std::make_unique<ParentTestState>("Running");
  auto *idlePtr = parentIdle.get();
  auto *runningPtr = parentRunning.get();

  StateMachine<TestStateID> sm(TestStateID::IDLE, "ParentSM");
  sm.withLogLevel(StateMachine<TestStateID>::LogLevel::NONE);

  sm.addState(TestStateID::IDLE, "Idle", std::move(parentIdle))
      .addState(TestStateID::RUNNING, "Running", std::move(parentRunning))
      .addSubState(TestStateID::IDLE, SubStateID::SUB_A, "SubA",
                   std::move(subA));

  sm.start();
  EXPECT_EQ(subAPtr->enterCount, 1);

  sm.changeState(TestStateID::RUNNING, "go running");

  EXPECT_EQ(subAPtr->exitCount, 1);
  EXPECT_EQ(idlePtr->exitCount, 1);
  EXPECT_EQ(runningPtr->enterCount, 1);
  EXPECT_FALSE(sm.hasActiveSubMachine());
}

TEST(HierarchicalSM, ChildTransitionStaysInParent) {
  auto subA = std::make_unique<SubTestState>("SubA");
  auto subB = std::make_unique<SubTestState>("SubB");
  auto *subAPtr = subA.get();
  auto *subBPtr = subB.get();

  auto parentIdle = std::make_unique<ParentTestState>("Idle");
  auto *idlePtr = parentIdle.get();

  StateMachine<TestStateID> sm(TestStateID::IDLE, "ParentSM");
  sm.withLogLevel(StateMachine<TestStateID>::LogLevel::NONE);

  sm.addState(TestStateID::IDLE, "Idle", std::move(parentIdle))
      .addSubState(TestStateID::IDLE, SubStateID::SUB_A, "SubA",
                   std::move(subA))
      .addSubState(TestStateID::IDLE, SubStateID::SUB_B, "SubB",
                   std::move(subB));
  sm.start();

  EXPECT_EQ(idlePtr->enterCount, 1);
  EXPECT_EQ(subAPtr->enterCount, 1);

  // Single call, no casts needed
  ASSERT_TRUE(sm.changeSubState(SubStateID::SUB_B, "switch sub"));

  EXPECT_EQ(idlePtr->enterCount, 1); // parent NOT re-entered
  EXPECT_EQ(idlePtr->exitCount, 0);
  EXPECT_EQ(subAPtr->exitCount, 1);
  EXPECT_EQ(subBPtr->enterCount, 1);
  EXPECT_EQ(sm.getActiveSubStateName(), "SubB");
}

TEST(HierarchicalSM, FlatStatesUnaffected) {
  StateMachine<TestStateID> sm(TestStateID::IDLE, "FlatSM");
  sm.withLogLevel(StateMachine<TestStateID>::LogLevel::NONE);

  auto idle = std::make_unique<ParentTestState>("Idle");
  auto running = std::make_unique<ParentTestState>("Running");
  auto *idlePtr = idle.get();
  auto *runningPtr = running.get();

  sm.addState(TestStateID::IDLE, "Idle", std::move(idle))
      .addState(TestStateID::RUNNING, "Running", std::move(running));

  sm.start();

  EXPECT_FALSE(sm.hasActiveSubMachine());
  EXPECT_EQ(sm.getActiveSubStateName(), "");
  EXPECT_EQ(sm.getSubMachine(), nullptr);

  sm.update();
  EXPECT_EQ(idlePtr->updateCount, 1);

  sm.changeState(TestStateID::RUNNING);
  EXPECT_EQ(idlePtr->exitCount, 1);
  EXPECT_EQ(runningPtr->enterCount, 1);
  EXPECT_FALSE(sm.hasActiveSubMachine());
}

TEST(HierarchicalSM, StopTearsDownChild) {
  auto subA = std::make_unique<SubTestState>("SubA");
  auto *subAPtr = subA.get();

  auto parentIdle = std::make_unique<ParentTestState>("Idle");
  auto *idlePtr = parentIdle.get();

  StateMachine<TestStateID> sm(TestStateID::IDLE, "ParentSM");
  sm.withLogLevel(StateMachine<TestStateID>::LogLevel::NONE);

  sm.addState(TestStateID::IDLE, "Idle", std::move(parentIdle))
      .addSubState(TestStateID::IDLE, SubStateID::SUB_A, "SubA",
                   std::move(subA));
  sm.start();

  EXPECT_TRUE(sm.isReady());
  EXPECT_TRUE(sm.hasActiveSubMachine());

  sm.stop();

  EXPECT_FALSE(sm.isReady());
  EXPECT_EQ(subAPtr->exitCount, 1);
  EXPECT_EQ(idlePtr->exitCount, 1);
}

TEST(HierarchicalSM, TransitionBetweenTwoParentsWithChildren) {
  auto subA = std::make_unique<SubTestState>("SubA");
  auto subB = std::make_unique<SubTestState>("SubB");
  auto *subAPtr = subA.get();
  auto *subBPtr = subB.get();

  auto parentIdle = std::make_unique<ParentTestState>("Idle");
  auto parentRunning = std::make_unique<ParentTestState>("Running");

  StateMachine<TestStateID> sm(TestStateID::IDLE, "ParentSM");
  sm.withLogLevel(StateMachine<TestStateID>::LogLevel::NONE);

  sm.addState(TestStateID::IDLE, "Idle", std::move(parentIdle))
      .addState(TestStateID::RUNNING, "Running", std::move(parentRunning))
      .addSubState(TestStateID::IDLE, SubStateID::SUB_A, "SubA",
                   std::move(subA))
      .addSubState(TestStateID::RUNNING, SubStateID::SUB_B, "SubB",
                   std::move(subB));

  sm.start();

  EXPECT_EQ(sm.getActiveSubStateName(), "SubA");
  EXPECT_EQ(subAPtr->enterCount, 1);

  sm.changeState(TestStateID::RUNNING);

  EXPECT_EQ(subAPtr->exitCount, 1);
  EXPECT_EQ(subBPtr->enterCount, 1);
  EXPECT_EQ(sm.getActiveSubStateName(), "SubB");
}

TEST(HierarchicalSM, GetTypedSubMachine) {
  auto subA = std::make_unique<SubTestState>("SubA");

  auto parentIdle = std::make_unique<ParentTestState>("Idle");

  StateMachine<TestStateID> sm(TestStateID::IDLE, "ParentSM");
  sm.withLogLevel(StateMachine<TestStateID>::LogLevel::NONE);

  sm.addState(TestStateID::IDLE, "Idle", std::move(parentIdle))
      .addSubState(TestStateID::IDLE, SubStateID::SUB_A, "SubA",
                   std::move(subA));
  sm.start();

  auto *child = sm.getTypedSubMachine<SubStateID>();
  ASSERT_NE(child, nullptr);
  EXPECT_EQ(child->getCurrentStateName(), "SubA");

  auto *child2 = sm.getTypedSubMachine<SubStateID>(TestStateID::IDLE);
  EXPECT_EQ(child2, child);

  auto *bad = sm.getTypedSubMachine<TestStateID>();
  EXPECT_EQ(bad, nullptr);
}

// Test: State convenience methods (changeSubState, getActiveSubStateName, etc.)
TEST(HierarchicalSM, StateConvenienceMethodsControlSubStates) {
  auto subA = std::make_unique<SubTestState>("SubA");
  auto subB = std::make_unique<SubTestState>("SubB");
  auto *subBPtr = subB.get();

  auto parent = std::make_unique<ParentWithSubControl>();
  auto *parentPtr = parent.get();

  StateMachine<TestStateID> sm(TestStateID::IDLE, "ParentSM");
  sm.withLogLevel(StateMachine<TestStateID>::LogLevel::NONE);

  sm.addState(TestStateID::IDLE, "Idle", std::move(parent))
      .addSubState(TestStateID::IDLE, SubStateID::SUB_A, "SubA",
                   std::move(subA))
      .addSubState(TestStateID::IDLE, SubStateID::SUB_B, "SubB",
                   std::move(subB));
  sm.start();

  // First update: parent sees sub-state info via convenience methods
  sm.update();
  EXPECT_EQ(parentPtr->lastSubName, "SubA");
  EXPECT_TRUE(parentPtr->lastHasSub);

  // Tell the parent state to trigger a sub-state change during next update
  parentPtr->shouldChangeSubState = true;
  parentPtr->targetSubState = SubStateID::SUB_B;

  sm.update();

  EXPECT_TRUE(parentPtr->subChangeResult);
  EXPECT_EQ(subBPtr->enterCount, 1);
  EXPECT_EQ(sm.getActiveSubStateName(), "SubB");

  // Next update: parent now sees SUB_B
  sm.update();
  EXPECT_EQ(parentPtr->lastSubName, "SubB");
}

// Test: setInitialSubState overrides the default initial sub-state
TEST(HierarchicalSM, SetInitialSubState) {
  auto subA = std::make_unique<SubTestState>("SubA");
  auto subB = std::make_unique<SubTestState>("SubB");
  auto *subAPtr = subA.get();
  auto *subBPtr = subB.get();

  auto parentIdle = std::make_unique<ParentTestState>("Idle");

  StateMachine<TestStateID> sm(TestStateID::IDLE, "ParentSM");
  sm.withLogLevel(StateMachine<TestStateID>::LogLevel::NONE);

  sm.addState(TestStateID::IDLE, "Idle", std::move(parentIdle))
      .addSubState(TestStateID::IDLE, SubStateID::SUB_A, "SubA",
                   std::move(subA))
      .addSubState(TestStateID::IDLE, SubStateID::SUB_B, "SubB",
                   std::move(subB))
      .setInitialSubState(TestStateID::IDLE, SubStateID::SUB_B);

  sm.start();

  // SUB_B should be the initial sub-state, not SUB_A
  EXPECT_EQ(sm.getActiveSubStateName(), "SubB");
  EXPECT_EQ(subBPtr->enterCount, 1);
  EXPECT_EQ(subAPtr->enterCount, 0);
}

// ============================================================================
// REGRESSION TESTS - transition commit correctness
// ============================================================================

// A state that re-entrantly transitions away from inside its own enter().
struct AdvanceOnEnterState : public TestState {
  TestStateID target;
  bool advance = true;

  AdvanceOnEnterState(std::string n, TestStateID t)
      : TestState(std::move(n)), target(t) {}

  bool enter() override {
    TestState::enter();
    if (advance) {
      advance = false;
      changeToState(target, "auto-advance from enter");
    }
    return !shouldFailEnter;
  }
};

// REGRESSION: history must stay in chronological order when enter() transitions.
// Previously the outer transition appended its own state *after* the nested one,
// producing [IDLE, PAUSED, RUNNING] instead of [IDLE, RUNNING, PAUSED].
TEST(TransitionCommit, NestedTransitionFromEnterKeepsHistoryOrdered) {
  StateMachine<TestStateID> sm(TestStateID::IDLE, "Nested");
  sm.withLogLevel(StateMachine<TestStateID>::LogLevel::NONE);

  sm.addState(TestStateID::IDLE, "Idle", std::make_unique<TestState>("Idle"))
      .addState(TestStateID::RUNNING, "Running",
                std::make_unique<AdvanceOnEnterState>("Running",
                                                      TestStateID::PAUSED))
      .addState(TestStateID::PAUSED, "Paused",
                std::make_unique<TestState>("Paused"));
  sm.start();

  ASSERT_TRUE(sm.changeState(TestStateID::RUNNING, "go"));
  EXPECT_EQ(sm.getCurrentStateId(), TestStateID::PAUSED);

  const std::vector<TestStateID> expected = {
      TestStateID::IDLE, TestStateID::RUNNING, TestStateID::PAUSED};
  EXPECT_EQ(sm.getStateHistory(), expected);
}

// REGRESSION: observers must not be notified out of order. Previously the nested
// RUNNING->PAUSED callback fired before the outer IDLE->RUNNING one, so a
// listener mirroring the FSM ended up reporting RUNNING as the final state.
TEST(TransitionCommit, NestedTransitionNotifiesObserversInOrder) {
  StateMachine<TestStateID> sm(TestStateID::IDLE, "Nested");
  sm.withLogLevel(StateMachine<TestStateID>::LogLevel::NONE);

  std::vector<std::pair<TestStateID, TestStateID>> seen;
  sm.onStateChanged([&](const TestStateID &from, const TestStateID &to,
                        std::string_view, std::string_view, std::string_view) {
    seen.emplace_back(from, to);
  });

  sm.addState(TestStateID::IDLE, "Idle", std::make_unique<TestState>("Idle"))
      .addState(TestStateID::RUNNING, "Running",
                std::make_unique<AdvanceOnEnterState>("Running",
                                                      TestStateID::PAUSED))
      .addState(TestStateID::PAUSED, "Paused",
                std::make_unique<TestState>("Paused"));
  sm.start();
  sm.changeState(TestStateID::RUNNING, "go");

  // The superseded outer arrival is reported as the `from` of the surviving
  // transition, so the last notification always names the real final state.
  ASSERT_EQ(seen.size(), 1u);
  EXPECT_EQ(seen.back().first, TestStateID::RUNNING);
  EXPECT_EQ(seen.back().second, TestStateID::PAUSED);
}

// REGRESSION: a state left from within its own enter() must not have its child
// sub-machine started. Previously both children ended up active, the orphan was
// never stopped, and re-entering the parent hit "already initialized" so its
// initial sub-state never ran again.
TEST(TransitionCommit, SupersededStateDoesNotStartItsChildSubMachine) {
  auto subA = std::make_unique<SubTestState>("SubA");
  auto *subAPtr = subA.get();

  auto running = std::make_unique<AdvanceOnEnterState>("Running",
                                                       TestStateID::PAUSED);
  auto *runningPtr = running.get();

  StateMachine<TestStateID> sm(TestStateID::IDLE, "Nested");
  sm.withLogLevel(StateMachine<TestStateID>::LogLevel::NONE);

  sm.addState(TestStateID::IDLE, "Idle", std::make_unique<TestState>("Idle"))
      .addState(TestStateID::RUNNING, "Running", std::move(running))
      .addState(TestStateID::PAUSED, "Paused",
                std::make_unique<TestState>("Paused"))
      .addSubState(TestStateID::RUNNING, SubStateID::SUB_A, "SubA",
                   std::move(subA));
  sm.start();

  sm.changeState(TestStateID::RUNNING, "advance through");
  ASSERT_EQ(sm.getCurrentStateId(), TestStateID::PAUSED);
  EXPECT_EQ(subAPtr->enterCount, 0) << "orphaned child sub-machine was started";
  EXPECT_FALSE(sm.hasActiveSubMachine());

  // Second visit does not advance, so the child must start normally.
  ASSERT_FALSE(runningPtr->advance);
  sm.changeState(TestStateID::RUNNING, "stay this time");
  ASSERT_EQ(sm.getCurrentStateId(), TestStateID::RUNNING);
  EXPECT_TRUE(sm.hasActiveSubMachine());
  EXPECT_EQ(subAPtr->enterCount, 1);
  EXPECT_EQ(sm.getActiveSubStateName(), "SubA");
}

// ============================================================================
// REGRESSION TESTS - failure recovery
// ============================================================================

// REGRESSION: an unregistered fallback used to leave currentStateId naming the
// state whose enter() failed, and never fired the error callback.
TEST(FailureRecovery, UnregisteredFallbackRollsBackToLastGoodState) {
  auto idle = std::make_unique<TestState>("Idle");
  auto *idlePtr = idle.get();
  auto bad = std::make_unique<TestState>("Bad");
  bad->shouldFailEnter = true;

  StateMachine<TestStateID> sm(TestStateID::IDLE, "Recovery");
  sm.withLogLevel(StateMachine<TestStateID>::LogLevel::NONE);

  bool errorFired = false;
  sm.onError([&](std::string_view, const TestStateID &) { errorFired = true; });

  sm.addState(TestStateID::IDLE, "Idle", std::move(idle))
      .addState(TestStateID::RUNNING, "Bad", std::move(bad));
  sm.withFallback(TestStateID::FALLBACK); // never registered
  sm.start();
  ASSERT_EQ(idlePtr->enterCount, 1);

  EXPECT_FALSE(sm.changeState(TestStateID::RUNNING, "doomed"));

  EXPECT_EQ(sm.getCurrentStateId(), TestStateID::IDLE);
  EXPECT_TRUE(errorFired);
  // Rolled back state is re-entered so its own view matches the machine's.
  EXPECT_EQ(idlePtr->enterCount, 2);
  // The failed state must not linger in history.
  const std::vector<TestStateID> expected = {TestStateID::IDLE};
  EXPECT_EQ(sm.getStateHistory(), expected);
}

// REGRESSION: when the fallback's own enter() fails too, the machine used to
// settle on the state whose enter() had just failed.
TEST(FailureRecovery, FailingFallbackRollsBackToLastGoodState) {
  auto idle = std::make_unique<TestState>("Idle");
  auto *idlePtr = idle.get();
  auto bad = std::make_unique<TestState>("Bad");
  bad->shouldFailEnter = true;
  auto badFallback = std::make_unique<TestState>("BadFallback");
  badFallback->shouldFailEnter = true;

  StateMachine<TestStateID> sm(TestStateID::IDLE, "Recovery");
  sm.withLogLevel(StateMachine<TestStateID>::LogLevel::NONE);

  sm.addState(TestStateID::IDLE, "Idle", std::move(idle))
      .addState(TestStateID::RUNNING, "Bad", std::move(bad))
      .addState(TestStateID::FALLBACK, "BadFallback", std::move(badFallback));
  sm.withFallback(TestStateID::FALLBACK);
  sm.start();

  EXPECT_FALSE(sm.changeState(TestStateID::RUNNING, "doomed"));

  EXPECT_EQ(sm.getCurrentStateId(), TestStateID::IDLE)
      << "must not settle on a state whose enter() failed";
  EXPECT_TRUE(sm.isReady());
  // Recovery must happen exactly once, not once per failed level.
  EXPECT_EQ(idlePtr->enterCount, 2);
}

// REGRESSION: exceptions that do not derive from std::exception used to escape
// changeState() after currentStateId had already been moved, with no recovery.
TEST(FailureRecovery, ForeignExceptionInEnterIsContained) {
  struct ThrowsIntOnEnter : public TestState {
    ThrowsIntOnEnter() : TestState("ThrowsInt") {}
    bool enter() override {
      enterCount++;
      throw 42; // not a std::exception
    }
  };

  StateMachine<TestStateID> sm(TestStateID::IDLE, "Foreign");
  sm.withLogLevel(StateMachine<TestStateID>::LogLevel::NONE);

  sm.addState(TestStateID::IDLE, "Idle", std::make_unique<TestState>("Idle"))
      .addState(TestStateID::RUNNING, "ThrowsInt",
                std::make_unique<ThrowsIntOnEnter>())
      .addState(TestStateID::FALLBACK, "Fallback",
                std::make_unique<TestState>("Fallback"));
  sm.withFallback(TestStateID::FALLBACK);
  sm.start();

  EXPECT_NO_THROW(sm.changeState(TestStateID::RUNNING, "throws"));
  EXPECT_EQ(sm.getCurrentStateId(), TestStateID::FALLBACK);
}

TEST(FailureRecovery, ForeignExceptionInExitAndUpdateIsContained) {
  struct ThrowsIntOnExitUpdate : public TestState {
    ThrowsIntOnExitUpdate() : TestState("ThrowsInt") {}
    void exit() override {
      exitCount++;
      throw 7;
    }
    void update() override {
      updateCount++;
      throw 7;
    }
  };

  StateMachine<TestStateID> sm(TestStateID::IDLE, "Foreign");
  sm.withLogLevel(StateMachine<TestStateID>::LogLevel::NONE);

  sm.addState(TestStateID::IDLE, "ThrowsInt",
              std::make_unique<ThrowsIntOnExitUpdate>())
      .addState(TestStateID::RUNNING, "Running",
                std::make_unique<TestState>("Running"));
  sm.start();

  EXPECT_NO_THROW(sm.update());
  EXPECT_NO_THROW(sm.changeState(TestStateID::RUNNING, "leave"));
  EXPECT_EQ(sm.getCurrentStateId(), TestStateID::RUNNING);
}

// REGRESSION: destroying a running machine used to skip exit() entirely,
// leaking whatever the active state acquired in enter().
TEST(Lifecycle, DestructorExitsActiveStateAndChild) {
  auto sub = std::make_unique<SubTestState>("SubA");
  auto *subPtr = sub.get();
  auto parent = std::make_unique<ParentTestState>("Idle");
  auto *parentPtr = parent.get();

  {
    StateMachine<TestStateID> sm(TestStateID::IDLE, "Teardown");
    sm.withLogLevel(StateMachine<TestStateID>::LogLevel::NONE);
    sm.addState(TestStateID::IDLE, "Idle", std::move(parent))
        .addSubState(TestStateID::IDLE, SubStateID::SUB_A, "SubA",
                     std::move(sub));
    sm.start();
    ASSERT_EQ(parentPtr->enterCount, 1);
    ASSERT_EQ(parentPtr->exitCount, 0);
  } // destructor runs here

  EXPECT_EQ(parentPtr->exitCount, 1);
  EXPECT_EQ(subPtr->exitCount, 1);
}

// ============================================================================
// CONCURRENCY - lock is not held across user code
// ============================================================================

// REGRESSION: update() used to re-acquire the lock and hold it across the child
// sub-machine's update(), so a long child update blocked every other thread.
TEST(Concurrency, ChildUpdateDoesNotHoldParentLock) {
  struct BlockingSubState : public StateMachine<SubStateID>::State {
    std::atomic<bool> inUpdate{false};
    std::atomic<bool> release{false};
    void update() override {
      inUpdate = true;
      while (!release) {
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
      }
    }
  };

  auto sub = std::make_unique<BlockingSubState>();
  auto *subPtr = sub.get();

  StateMachine<TestStateID> sm(TestStateID::IDLE, "LockCheck");
  sm.withLogLevel(StateMachine<TestStateID>::LogLevel::NONE);
  sm.addState(TestStateID::IDLE, "Idle",
              std::make_unique<ParentTestState>("Idle"))
      .addSubState(TestStateID::IDLE, SubStateID::SUB_A, "SubA",
                   std::move(sub));
  sm.start();

  std::thread updater([&] { sm.update(); });
  // Watchdog so this test cannot hang if the lock *is* held.
  std::thread releaser([&] {
    std::this_thread::sleep_for(std::chrono::seconds(2));
    subPtr->release = true;
  });

  while (!subPtr->inUpdate) {
    std::this_thread::sleep_for(std::chrono::milliseconds(1));
  }

  const auto begin = std::chrono::steady_clock::now();
  const auto observed = sm.getCurrentStateId();
  const auto waited = std::chrono::steady_clock::now() - begin;

  subPtr->release = true;
  updater.join();
  releaser.join();

  EXPECT_EQ(observed, TestStateID::IDLE);
  EXPECT_LT(waited, std::chrono::milliseconds(500))
      << "parent lock was held across the child sub-machine's update()";
}

// ============================================================================
// TIMING - injected clock, time-in-state and declarative timeouts
// ============================================================================

TEST(Timing, InjectedClockDrivesTimeInStateAndTimeouts) {
  double virtualClock = 100.0;

  StateMachine<TestStateID> sm(TestStateID::IDLE, "Clock");
  sm.withLogLevel(StateMachine<TestStateID>::LogLevel::NONE)
      .withClock([&virtualClock] { return virtualClock; });

  sm.addState(TestStateID::IDLE, "Idle", std::make_unique<TestState>("Idle"))
      .addState(TestStateID::RUNNING, "Running",
                std::make_unique<TestState>("Running"));
  sm.withStateTimeout(TestStateID::IDLE, 5.0, TestStateID::RUNNING,
                      "idle too long");
  sm.start();

  EXPECT_DOUBLE_EQ(sm.stateEntryTime(), 100.0);
  EXPECT_DOUBLE_EQ(sm.timeInState(), 0.0);

  virtualClock = 104.0;
  sm.update();
  EXPECT_EQ(sm.getCurrentStateId(), TestStateID::IDLE) << "fired too early";
  EXPECT_DOUBLE_EQ(sm.timeInState(), 4.0);

  virtualClock = 105.5;
  sm.update();
  EXPECT_EQ(sm.getCurrentStateId(), TestStateID::RUNNING);
  EXPECT_DOUBLE_EQ(sm.stateEntryTime(), 105.5);
  EXPECT_DOUBLE_EQ(sm.timeInState(), 0.0);
}

TEST(Timing, StateExposesTimeInStateToItself) {
  struct TimeReadingState : public TestState {
    double seen = -1.0;
    TimeReadingState() : TestState("TimeReader") {}
    void update() override { seen = timeInState(); }
  };

  double virtualClock = 0.0;
  auto state = std::make_unique<TimeReadingState>();
  auto *statePtr = state.get();

  StateMachine<TestStateID> sm(TestStateID::IDLE, "Clock");
  sm.withLogLevel(StateMachine<TestStateID>::LogLevel::NONE)
      .withClock([&virtualClock] { return virtualClock; });
  sm.addState(TestStateID::IDLE, "TimeReader", std::move(state));
  sm.start();

  virtualClock = 3.25;
  sm.update();
  EXPECT_DOUBLE_EQ(statePtr->seen, 3.25);
}

// ============================================================================
// TRANSITION GUARDS - whitelist and predicate
// ============================================================================

TEST(TransitionGuards, WhitelistRejectsUndeclaredTransition) {
  StateMachine<TestStateID> sm(TestStateID::IDLE, "Guarded");
  sm.withLogLevel(StateMachine<TestStateID>::LogLevel::NONE);

  sm.addState(TestStateID::IDLE, "Idle", std::make_unique<TestState>("Idle"))
      .addState(TestStateID::RUNNING, "Running",
                std::make_unique<TestState>("Running"))
      .addState(TestStateID::PAUSED, "Paused",
                std::make_unique<TestState>("Paused"));

  sm.withAllowedTransition(TestStateID::IDLE, TestStateID::RUNNING)
      .withAllowedTransition(TestStateID::RUNNING, TestStateID::PAUSED);
  sm.start();

  EXPECT_FALSE(sm.changeState(TestStateID::PAUSED, "illegal skip"));
  EXPECT_EQ(sm.getCurrentStateId(), TestStateID::IDLE);

  EXPECT_TRUE(sm.changeState(TestStateID::RUNNING, "legal"));
  EXPECT_TRUE(sm.changeState(TestStateID::PAUSED, "legal"));
  EXPECT_EQ(sm.getCurrentStateId(), TestStateID::PAUSED);
}

TEST(TransitionGuards, FallbackBypassesWhitelist) {
  auto bad = std::make_unique<TestState>("Bad");
  bad->shouldFailEnter = true;

  StateMachine<TestStateID> sm(TestStateID::IDLE, "Guarded");
  sm.withLogLevel(StateMachine<TestStateID>::LogLevel::NONE);

  sm.addState(TestStateID::IDLE, "Idle", std::make_unique<TestState>("Idle"))
      .addState(TestStateID::RUNNING, "Bad", std::move(bad))
      .addState(TestStateID::FALLBACK, "Fallback",
                std::make_unique<TestState>("Fallback"));

  // FALLBACK is deliberately unreachable through the whitelist.
  sm.withAllowedTransition(TestStateID::IDLE, TestStateID::RUNNING);
  sm.withFallback(TestStateID::FALLBACK);
  sm.start();

  EXPECT_FALSE(sm.changeState(TestStateID::RUNNING, "will fail"));
  EXPECT_EQ(sm.getCurrentStateId(), TestStateID::FALLBACK)
      << "recovery must not be blocked by the whitelist";
}

TEST(TransitionGuards, GuardPredicateRejectsTransition) {
  bool allow = false;

  StateMachine<TestStateID> sm(TestStateID::IDLE, "Guarded");
  sm.withLogLevel(StateMachine<TestStateID>::LogLevel::NONE);
  sm.addState(TestStateID::IDLE, "Idle", std::make_unique<TestState>("Idle"))
      .addState(TestStateID::RUNNING, "Running",
                std::make_unique<TestState>("Running"));
  sm.withTransitionGuard(
      [&allow](const TestStateID &, const TestStateID &) { return allow; });
  sm.start();

  EXPECT_FALSE(sm.changeState(TestStateID::RUNNING, "blocked"));
  EXPECT_EQ(sm.getCurrentStateId(), TestStateID::IDLE);

  allow = true;
  EXPECT_TRUE(sm.changeState(TestStateID::RUNNING, "permitted"));
  EXPECT_EQ(sm.getCurrentStateId(), TestStateID::RUNNING);
}

// ============================================================================
// TRANSITION API - compare-and-swap and forced re-entry
// ============================================================================

TEST(TransitionApi, ChangeStateIfOnlyAppliesFromExpectedState) {
  StateMachine<TestStateID> sm(TestStateID::IDLE, "Cas");
  sm.withLogLevel(StateMachine<TestStateID>::LogLevel::NONE);
  sm.addState(TestStateID::IDLE, "Idle", std::make_unique<TestState>("Idle"))
      .addState(TestStateID::RUNNING, "Running",
                std::make_unique<TestState>("Running"))
      .addState(TestStateID::PAUSED, "Paused",
                std::make_unique<TestState>("Paused"));
  sm.start();

  EXPECT_FALSE(sm.changeStateIf(TestStateID::PAUSED, TestStateID::RUNNING));
  EXPECT_EQ(sm.getCurrentStateId(), TestStateID::IDLE);

  EXPECT_TRUE(sm.changeStateIf(TestStateID::IDLE, TestStateID::RUNNING));
  EXPECT_EQ(sm.getCurrentStateId(), TestStateID::RUNNING);
}

TEST(TransitionApi, ReenterStateForcesExitAndEnter) {
  auto idle = std::make_unique<TestState>("Idle");
  auto *idlePtr = idle.get();

  StateMachine<TestStateID> sm(TestStateID::IDLE, "Reenter");
  sm.withLogLevel(StateMachine<TestStateID>::LogLevel::NONE);
  sm.addState(TestStateID::IDLE, "Idle", std::move(idle));
  sm.start();
  ASSERT_EQ(idlePtr->enterCount, 1);

  // Plain changeState to the active state stays a no-op.
  EXPECT_TRUE(sm.changeState(TestStateID::IDLE, "same"));
  EXPECT_EQ(idlePtr->enterCount, 1);
  EXPECT_EQ(idlePtr->exitCount, 0);

  EXPECT_TRUE(sm.reenterState("restart"));
  EXPECT_EQ(idlePtr->exitCount, 1);
  EXPECT_EQ(idlePtr->enterCount, 2);
}

// ============================================================================
// HISTORY, PATHS AND INTROSPECTION
// ============================================================================

TEST(History, TransitionHistoryCarriesTimestampsAndReasons) {
  double virtualClock = 10.0;

  StateMachine<TestStateID> sm(TestStateID::IDLE, "Hist");
  sm.withLogLevel(StateMachine<TestStateID>::LogLevel::NONE)
      .withClock([&virtualClock] { return virtualClock; });
  sm.addState(TestStateID::IDLE, "Idle", std::make_unique<TestState>("Idle"))
      .addState(TestStateID::RUNNING, "Running",
                std::make_unique<TestState>("Running"));
  sm.start();

  virtualClock = 12.5;
  sm.changeState(TestStateID::RUNNING, "because reasons");

  const auto history = sm.getTransitionHistory();
  ASSERT_EQ(history.size(), 2u);
  EXPECT_EQ(history[0].state, TestStateID::IDLE);
  EXPECT_DOUBLE_EQ(history[0].timestamp, 10.0);
  EXPECT_EQ(history[1].state, TestStateID::RUNNING);
  EXPECT_DOUBLE_EQ(history[1].timestamp, 12.5);
  EXPECT_EQ(history[1].reason, "because reasons");
}

TEST(Introspection, StatePathIncludesActiveSubStates) {
  StateMachine<TestStateID> sm(TestStateID::IDLE, "Path");
  sm.withLogLevel(StateMachine<TestStateID>::LogLevel::NONE);
  sm.addState(TestStateID::IDLE, "Idle",
              std::make_unique<ParentTestState>("Idle"))
      .addState(TestStateID::RUNNING, "Running",
                std::make_unique<ParentTestState>("Running"))
      .addSubState(TestStateID::IDLE, SubStateID::SUB_A, "SubA",
                   std::make_unique<SubTestState>("SubA"))
      .addSubState(TestStateID::IDLE, SubStateID::SUB_B, "SubB",
                   std::make_unique<SubTestState>("SubB"));
  sm.start();

  EXPECT_EQ(sm.statePath(), "Idle/SubA");

  ASSERT_TRUE(sm.changeSubState(SubStateID::SUB_B, "switch"));
  EXPECT_EQ(sm.statePath(), "Idle/SubB");

  sm.changeState(TestStateID::RUNNING, "flat state");
  EXPECT_EQ(sm.statePath(), "Running");
}

TEST(Introspection, TryGetContextNeverThrows) {
  StateMachine<TestStateID> sm(TestStateID::IDLE, "Ctx");
  sm.withLogLevel(StateMachine<TestStateID>::LogLevel::NONE);
  sm.addState(TestStateID::IDLE, "Idle", std::make_unique<TestState>("Idle"));

  EXPECT_EQ(sm.tryGetContext<int>(), nullptr);

  sm.withContext(std::make_shared<int>(5));
  auto typed = sm.tryGetContext<int>();
  ASSERT_NE(typed, nullptr);
  EXPECT_EQ(*typed, 5);

  std::shared_ptr<double> wrong;
  EXPECT_NO_THROW({ wrong = sm.tryGetContext<double>(); });
  EXPECT_EQ(wrong, nullptr);
}

// ============================================================================
// LOGGING - injectable sink
// ============================================================================

// Drive a machine through one transition, capturing everything the sink sees.
static std::vector<std::string> captureSinkOutput(bool colorsEnabled) {
  std::vector<std::string> lines;

  StateMachine<TestStateID> sm(TestStateID::IDLE, "Sink");
  sm.withLogSink([&lines](StateMachine<TestStateID>::LogLevel,
                          std::string_view msg) { lines.emplace_back(msg); })
      .withLogLevel(StateMachine<TestStateID>::LogLevel::INFO)
      .withColors(colorsEnabled);

  sm.addState(TestStateID::IDLE, "Idle", std::make_unique<TestState>("Idle"))
      .addState(TestStateID::RUNNING, "Running",
                std::make_unique<TestState>("Running"));
  sm.start();
  sm.changeState(TestStateID::RUNNING, "why not");
  return lines;
}

static bool describesTheTransition(const std::vector<std::string> &lines) {
  return std::any_of(lines.begin(), lines.end(), [](const std::string &line) {
    return line.find("Idle") != std::string::npos &&
           line.find("Running") != std::string::npos &&
           line.find("why not") != std::string::npos;
  });
}

TEST(Logging, SinkOmitsAnsiWhenColorsDisabled) {
  const auto lines = captureSinkOutput(/*colorsEnabled=*/false);

  ASSERT_FALSE(lines.empty());
  for (const auto &line : lines) {
    EXPECT_EQ(line.find('\033'), std::string::npos)
        << "ANSI escape leaked into an ANSI-free sink: " << line;
  }
  EXPECT_TRUE(describesTheTransition(lines));
}

// A sink must not silently strip colors: routing to rosout should look the same
// as routing to stdout unless colors are explicitly turned off.
TEST(Logging, SinkKeepsAnsiWhenColorsEnabled) {
  const auto lines = captureSinkOutput(/*colorsEnabled=*/true);

  ASSERT_FALSE(lines.empty());
  const bool anyColored =
      std::any_of(lines.begin(), lines.end(), [](const std::string &line) {
        return line.find('\033') != std::string::npos;
      });
  EXPECT_TRUE(anyColored) << "sink received no ANSI despite colors being on";
  EXPECT_TRUE(describesTheTransition(lines));
}

// REGRESSION: sink records used to omit the machine name, so nested machines
// were indistinguishable and the only tag left was whatever prefix the adapter
// happened to add.
TEST(Logging, SinkRecordsCarryTheMachineName) {
  for (const bool colors : {false, true}) {
    const auto lines = captureSinkOutput(colors);
    ASSERT_FALSE(lines.empty());
    for (const auto &line : lines) {
      EXPECT_NE(line.find("[Sink]"), std::string::npos)
          << "record is missing the machine name tag: " << line;
    }
  }
}

// REGRESSION: tint() closed its span with a full reset (ESC[0m), which cleared
// the color of the enclosing line, so everything after the struck-through state
// name rendered in the terminal's default color.
TEST(Logging, TintedSpanDoesNotResetTheLineColor) {
  const auto lines = captureSinkOutput(/*colorsEnabled=*/true);

  const auto transition =
      std::find_if(lines.begin(), lines.end(), [](const std::string &line) {
        return line.find("why not") != std::string::npos;
      });
  ASSERT_NE(transition, lines.end());

  const std::string reset = "\033[0m";
  // The only full reset must be the trailing one that closes the record.
  EXPECT_EQ(transition->find(reset), transition->size() - reset.size())
      << "a full reset appears mid-line, clearing the line color: "
      << *transition;
  // The struck-through state name is closed with the targeted off-code instead.
  EXPECT_NE(transition->find("\033[29m"), std::string::npos)
      << "strikethrough was not closed with its own off-code: " << *transition;
}

// The transition banner travels through the ROS logging pipeline, which mangles
// multi-byte UTF-8 under the non-UTF-8 locale used in the containers.
TEST(Logging, TransitionOutputIsAsciiOnly) {
  for (const bool colors : {false, true}) {
    const auto lines = captureSinkOutput(colors);
    ASSERT_FALSE(lines.empty());
    for (const auto &line : lines) {
      for (const unsigned char ch : line) {
        EXPECT_LT(ch, 0x80u)
            << "non-ASCII byte 0x" << std::hex << static_cast<int>(ch)
            << " in state machine output: " << line;
      }
    }
  }
}

// ============================================================================
// VALIDATION
// ============================================================================

TEST(Validation, DetectsUnregisteredTimeoutTarget) {
  StateMachine<TestStateID> sm(TestStateID::IDLE, "Val");
  sm.withLogLevel(StateMachine<TestStateID>::LogLevel::NONE);
  sm.addState(TestStateID::IDLE, "Idle", std::make_unique<TestState>("Idle"));
  EXPECT_TRUE(sm.validate());

  // PAUSED is never registered.
  sm.withStateTimeout(TestStateID::IDLE, 1.0, TestStateID::PAUSED, "nowhere");
  EXPECT_FALSE(sm.validate());
}

TEST(Validation, RecursesIntoChildStateMachines) {
  StateMachine<TestStateID> sm(TestStateID::IDLE, "Val");
  sm.withLogLevel(StateMachine<TestStateID>::LogLevel::NONE);
  sm.addState(TestStateID::IDLE, "Idle",
              std::make_unique<ParentTestState>("Idle"));
  EXPECT_TRUE(sm.validate());

  // Child's initial sub-state (SUB_C) is never registered on the child.
  sm.withSubStates<SubStateID>(
      TestStateID::IDLE, SubStateID::SUB_C, [](auto &sub) {
        sub.addState(SubStateID::SUB_A, "SubA",
                     std::make_unique<SubTestState>("SubA"));
      });

  EXPECT_FALSE(sm.validate())
      << "an invalid child state machine must invalidate the parent";
}

TEST(Validation, NotifyInitialStateIsOptIn) {
  auto makeMachine = [](bool notify, int &counter) {
    auto sm = std::make_unique<StateMachine<TestStateID>>(TestStateID::IDLE,
                                                          "Init");
    sm->withLogLevel(StateMachine<TestStateID>::LogLevel::NONE);
    sm->onStateChanged([&counter](const TestStateID &, const TestStateID &,
                                 std::string_view, std::string_view,
                                 std::string_view) { counter++; });
    sm->addState(TestStateID::IDLE, "Idle",
                 std::make_unique<TestState>("Idle"));
    if (notify) {
      sm->withNotifyInitialState();
    }
    sm->start();
    return sm;
  };

  int withoutNotify = 0;
  auto quiet = makeMachine(false, withoutNotify);
  EXPECT_EQ(withoutNotify, 0);

  int withNotify = 0;
  auto loud = makeMachine(true, withNotify);
  EXPECT_EQ(withNotify, 1);
}

// ============================================================================
// BUG REGRESSION TESTS
// ============================================================================
// These tests assert the *correct* behavior. They FAIL when the bug is
// present, and PASS once the bug is fixed.
// ============================================================================

// ---------------------------------------------------------------------------
// Bug 1 (Critical): transitions from exit() can corrupt the active lifecycle
// changeStateInternal() calls exit() then unconditionally commits its
// destination. If exit() performs a nested transition, that nested state
// is entered then silently overwritten.
// ---------------------------------------------------------------------------

enum class Bug1State { A, B, C };

struct Bug1StateA : StateMachine<Bug1State>::State {
  std::atomic<bool> triggerInExit{false};
  int enterCount{0}, exitCount{0};
  bool enter() override { enterCount++; return true; }
  void exit() override {
    exitCount++;
    if (triggerInExit) {
      triggerInExit = false;
      changeToState(Bug1State::C, "nested from exit");
    }
  }
};

struct Bug1StateB : StateMachine<Bug1State>::State {
  int enterCount{0}, exitCount{0};
  bool enter() override { enterCount++; return true; }
  void exit() override { exitCount++; }
};

struct Bug1StateC : StateMachine<Bug1State>::State {
  int enterCount{0}, exitCount{0};
  bool enter() override { enterCount++; return true; }
  void exit() override { exitCount++; }
};

TEST(StateMachineBugs, ExitTransitionCorruptsLifecycle) {
  StateMachine<Bug1State> sm(Bug1State::A, "Bug1");
  sm.withLogLevel(StateMachine<Bug1State>::LogLevel::NONE);

  auto a = std::make_unique<Bug1StateA>();
  auto *aPtr = a.get();
  auto b = std::make_unique<Bug1StateB>();
  auto c = std::make_unique<Bug1StateC>();
  auto *cPtr = c.get();

  sm.addState(Bug1State::A, "A", std::move(a));
  sm.addState(Bug1State::B, "B", std::move(b));
  sm.addState(Bug1State::C, "C", std::move(c));
  sm.start();

  aPtr->triggerInExit = true;
  sm.changeState(Bug1State::B, "A->B");

  // Correct: the nested transition to C should be respected, not overwritten.
  // If C was entered, it must also be exited if the outer transition overwrites.
  EXPECT_EQ(sm.getCurrentStateId(), Bug1State::C)
      << "Nested transition from exit() was silently overwritten by outer A->B";
  // If the machine is in B (bug), C was entered but never exited — a leak.
  if (sm.getCurrentStateId() == Bug1State::B) {
    EXPECT_EQ(cPtr->exitCount, cPtr->enterCount)
        << "C was entered by nested transition but never exited — leaked";
  }
}

// ---------------------------------------------------------------------------
// Bug 1b: stop() has the same problem — machine stays initialized during exit()
// ---------------------------------------------------------------------------

TEST(StateMachineBugs, StopAllowsTransitionDuringShutdown) {
  StateMachine<Bug1State> sm(Bug1State::A, "Bug1b");
  sm.withLogLevel(StateMachine<Bug1State>::LogLevel::NONE);

  auto a = std::make_unique<Bug1StateA>();
  auto *aPtr = a.get();
  auto b = std::make_unique<Bug1StateB>();
  auto c = std::make_unique<Bug1StateC>();
  auto *cPtr = c.get();

  sm.addState(Bug1State::A, "A", std::move(a));
  sm.addState(Bug1State::B, "B", std::move(b));
  sm.addState(Bug1State::C, "C", std::move(c));
  sm.start();

  aPtr->triggerInExit = true;
  sm.stop();

  // Correct: transitions during stop() should be rejected.
  // C should never be entered because the machine is shutting down.
  EXPECT_EQ(cPtr->enterCount, 0)
      << "stop() allowed a nested transition during shutdown — C was entered";
}

// ---------------------------------------------------------------------------
// Bug 2 (High): failed enter() is not always balanced by exit()
// When enter() fails without a usable fallback, the code rolls back
// without calling exit() on the failed state.
// ---------------------------------------------------------------------------

enum class Bug2State { A, B, FALLBACK };

struct Bug2StateA : StateMachine<Bug2State>::State {
  bool enter() override { return true; }
};

struct Bug2StateB : StateMachine<Bug2State>::State {
  int enterCount{0}, exitCount{0};
  bool enter() override { enterCount++; return false; }
  void exit() override { exitCount++; }
};

struct Bug2StateFallback : StateMachine<Bug2State>::State {
  bool enter() override { return true; }
};

TEST(StateMachineBugs, FailedEnterNotBalancedByExit) {
  StateMachine<Bug2State> sm(Bug2State::A, "Bug2");
  sm.withLogLevel(StateMachine<Bug2State>::LogLevel::NONE);

  sm.addState(Bug2State::A, "A", std::make_unique<Bug2StateA>());
  auto b = std::make_unique<Bug2StateB>();
  auto *bPtr = b.get();
  sm.addState(Bug2State::B, "B", std::move(b));
  sm.start();

  sm.changeState(Bug2State::B, "try B");

  // Correct: B.exit() must be called to clean up any partial resources.
  EXPECT_GE(bPtr->exitCount, 1)
      << "B.exit() should be called to clean up partial enter resources";
}

// Bug 2b: Initial-state failure has the same issue

enum class Bug2bState { INIT, OTHER };

struct Bug2bInitState : StateMachine<Bug2bState>::State {
  int enterCount{0}, exitCount{0};
  bool enter() override { enterCount++; return false; }
  void exit() override { exitCount++; }
};

struct Bug2bOtherState : StateMachine<Bug2bState>::State {
  bool enter() override { return true; }
};

TEST(StateMachineBugs, InitialStateFailureSkipsExit) {
  StateMachine<Bug2bState> sm(Bug2bState::INIT, "Bug2b");
  sm.withLogLevel(StateMachine<Bug2bState>::LogLevel::NONE);

  auto init = std::make_unique<Bug2bInitState>();
  auto *initPtr = init.get();
  sm.addState(Bug2bState::INIT, "Init", std::move(init));
  sm.addState(Bug2bState::OTHER, "Other", std::make_unique<Bug2bOtherState>());
  sm.start();

  EXPECT_FALSE(sm.isReady());
  // Correct: exit() must be called even when initial enter() fails.
  EXPECT_GE(initPtr->exitCount, 1)
      << "Init.exit() should be called even when initial enter() fails";
}

// ---------------------------------------------------------------------------
// Bug 3 (High): timeout transitions can act on the wrong state
// update() snapshots the active state, unlocks, updates the child, then
// executes the old timeout using unconditional changeState(). If the child
// transitions the parent meanwhile, an expired timeout belonging to state A
// can transition a newer state B.
// ---------------------------------------------------------------------------

enum class Bug3State { A, B, C };
enum class Bug3Child { X };

struct Bug3StateA : StateMachine<Bug3State>::State {
  bool enter() override { return true; }
};

struct Bug3StateB : StateMachine<Bug3State>::State {
  bool enter() override { return true; }
};

struct Bug3StateC : StateMachine<Bug3State>::State {
  bool enter() override { return true; }
};

// Child state that transitions the parent during child->update()
struct Bug3ChildX : StateMachine<Bug3Child>::State {
  StateMachine<Bug3State> *parent{nullptr};
  std::atomic<bool> trigger{false};
  bool enter() override { return true; }
  void update() override {
    if (trigger.exchange(false) && parent) {
      parent->changeState(Bug3State::B, "child triggered parent transition");
    }
  }
};

TEST(StateMachineBugs, TimeoutActsOnWrongState) {
  double fakeTime = 0.0;

  StateMachine<Bug3State> sm(Bug3State::A, "Bug3");
  sm.withLogLevel(StateMachine<Bug3State>::LogLevel::NONE);
  sm.withClock([&] { return fakeTime; });

  sm.addState(Bug3State::B, "B", std::make_unique<Bug3StateB>());
  sm.addState(Bug3State::C, "C", std::make_unique<Bug3StateC>());

  // Create child SM for state A — the child will transition the parent
  // during child->update(), after the timeout was snapshotted for A.
  auto childSM = std::make_unique<StateMachine<Bug3Child>>(Bug3Child::X, "Bug3Child");
  childSM->withLogLevel(StateMachine<Bug3Child>::LogLevel::NONE);
  childSM->withClock([&] { return fakeTime; });
  auto childState = std::make_unique<Bug3ChildX>();
  auto *childPtr = childState.get();
  childState->parent = &sm;
  childSM->addState(Bug3Child::X, "X", std::move(childState));

  sm.addState(Bug3State::A, "A", std::make_unique<Bug3StateA>(), std::move(childSM));
  sm.withStateTimeout(Bug3State::A, 10.0, Bug3State::C, "A timeout");
  sm.start();

  // During update(): A::update() does nothing, then child X::update()
  // transitions parent to B. Then the stale timeout (for A) fires
  // changeState(C), transitioning B to C.
  childPtr->trigger = true;
  fakeTime = 20.0; // exceed the timeout
  sm.update();

  // Correct: timeout belonged to A, machine is now in B, timeout should NOT fire.
  EXPECT_EQ(sm.getCurrentStateId(), Bug3State::B)
      << "Timeout for A incorrectly fired after A->B transition during child update";
}

// Bug 3b: update callback receives stale activeId/activeName after timeout

enum class Bug3bState { A, B };

struct Bug3bStateA : StateMachine<Bug3bState>::State {
  bool enter() override { return true; }
};

struct Bug3bStateB : StateMachine<Bug3bState>::State {
  bool enter() override { return true; }
};

TEST(StateMachineBugs, UpdateCallbackReceivesStaleState) {
  StateMachine<Bug3bState> sm(Bug3bState::A, "Bug3b");
  sm.withLogLevel(StateMachine<Bug3bState>::LogLevel::NONE);

  double fakeTime = 0.0;
  sm.withClock([&] { return fakeTime; });

  sm.addState(Bug3bState::A, "A", std::make_unique<Bug3bStateA>());
  sm.addState(Bug3bState::B, "B", std::make_unique<Bug3bStateB>());

  // Timeout on A: after 10s, go to B
  sm.withStateTimeout(Bug3bState::A, 10.0, Bug3bState::B, "A timeout");

  Bug3bState callbackState = Bug3bState::A;
  std::string callbackName;
  sm.onStateUpdated([&](const Bug3bState &current, std::string_view currentName) {
    callbackState = current;
    callbackName = std::string(currentName);
  });

  sm.start();
  fakeTime = 20.0; // exceed the timeout
  sm.update();

  // The timeout fires during update(), changing A->B.
  // Correct: the callback should report the current state B.
  EXPECT_EQ(sm.getCurrentStateId(), Bug3bState::B);
  EXPECT_EQ(callbackState, Bug3bState::B)
      << "Update callback received stale state A instead of current B";
  EXPECT_EQ(callbackName, "B")
      << "Update callback received stale name 'A' instead of 'B'";
}

// ---------------------------------------------------------------------------
// Bug 4 (High): failed re-entry corrupts history
// Consecutive identical states are not appended, but rollback removes
// the last entry merely because its state ID matches.
// ---------------------------------------------------------------------------

enum class Bug4State { A, B };

struct Bug4StateA : StateMachine<Bug4State>::State {
  bool enter() override { return true; }
};

struct Bug4StateB : StateMachine<Bug4State>::State {
  int enterCount{0};
  bool shouldFail{false};
  bool enter() override { enterCount++; return !shouldFail; }
};

TEST(StateMachineBugs, FailedReentryCorruptsHistory) {
  StateMachine<Bug4State> sm(Bug4State::A, "Bug4");
  sm.withLogLevel(StateMachine<Bug4State>::LogLevel::NONE);

  sm.addState(Bug4State::A, "A", std::make_unique<Bug4StateA>());
  auto b = std::make_unique<Bug4StateB>();
  auto *bPtr = b.get();
  sm.addState(Bug4State::B, "B", std::move(b));
  sm.start();

  // Go A -> B (success)
  sm.changeState(Bug4State::B, "first B");
  ASSERT_EQ(sm.getCurrentStateId(), Bug4State::B);
  ASSERT_EQ(sm.getStateHistory().size(), 2u); // [A, B]

  // Force re-enter B, but make it fail
  bPtr->shouldFail = true;
  sm.reenterState("force reenter B");

  // Correct: the failed re-entry should NOT delete the previous valid B record.
  // History should still contain [A, B] (the valid entries).
  auto history = sm.getStateHistory();
  EXPECT_GE(history.size(), 2u)
      << "Failed re-entry deleted the previous valid B record from history";
}

// Bug 4b: failed tentative transition can evict oldest capped record

TEST(StateMachineBugs, FailedTransitionEvictsCappedRecord) {
  StateMachine<Bug4State> sm(Bug4State::A, "Bug4b");
  sm.withLogLevel(StateMachine<Bug4State>::LogLevel::NONE);
  sm.withHistorySize(2); // small cap

  sm.addState(Bug4State::A, "A", std::make_unique<Bug4StateA>());
  auto b = std::make_unique<Bug4StateB>();
  auto *bPtr = b.get();
  sm.addState(Bug4State::B, "B", std::move(b));
  sm.start();

  // History: [A]
  sm.changeState(Bug4State::B, "to B");
  // History: [A, B] (capped at 2)
  sm.changeState(Bug4State::A, "to A");
  // History: [B, A] (A evicted from front)
  ASSERT_EQ(sm.getStateHistory().size(), 2u);

  // Now try to go to B but fail. commitEntry pushes B, evicting the oldest B.
  // Then rollbackHistory pops the new B. The old valid B is lost forever.
  bPtr->shouldFail = true;
  sm.changeState(Bug4State::B, "fail B");

  // Correct: history should still be [B, A] (unchanged by the failed transition).
  auto history = sm.getStateHistory();
  EXPECT_EQ(sm.getCurrentStateId(), Bug4State::A);
  EXPECT_EQ(history.size(), 2u)
      << "Failed transition evicted a capped record that rollback could not restore";
}

// ---------------------------------------------------------------------------
// Bug 5 (High): fallback is reported as success
// changeState(B) returns true when B::enter() fails but the fallback succeeds.
// ---------------------------------------------------------------------------

enum class Bug5State { A, B, FALLBACK };

struct Bug5StateA : StateMachine<Bug5State>::State {
  bool enter() override { return true; }
};

struct Bug5StateB : StateMachine<Bug5State>::State {
  bool enter() override { return false; }
};

struct Bug5StateFallback : StateMachine<Bug5State>::State {
  bool enter() override { return true; }
};

TEST(StateMachineBugs, FallbackReportedAsSuccess) {
  StateMachine<Bug5State> sm(Bug5State::A, "Bug5");
  sm.withLogLevel(StateMachine<Bug5State>::LogLevel::NONE);

  sm.addState(Bug5State::A, "A", std::make_unique<Bug5StateA>());
  sm.addState(Bug5State::B, "B", std::make_unique<Bug5StateB>());
  sm.addState(Bug5State::FALLBACK, "Fallback",
              std::make_unique<Bug5StateFallback>());
  sm.withFallback(Bug5State::FALLBACK);
  sm.start();

  bool result = sm.changeState(Bug5State::B, "try B");

  // Correct: changeState(B) should return false when B fails, even if
  // the fallback succeeds. The caller must know B was not reached.
  EXPECT_EQ(sm.getCurrentStateId(), Bug5State::FALLBACK);
  EXPECT_FALSE(result)
      << "changeState(B) returned true but B failed — caller is misled";
}

// ---------------------------------------------------------------------------
// Bug 6 (Medium): strict configuration does not actually freeze configuration
// configChangeRejected() is not applied to fallback, clock, timeout,
// whitelist, or guard configuration.
// ---------------------------------------------------------------------------

enum class Bug6State { A, B };

struct Bug6StateA : StateMachine<Bug6State>::State {
  bool enter() override { return true; }
};

struct Bug6StateB : StateMachine<Bug6State>::State {
  bool enter() override { return true; }
};

TEST(StateMachineBugs, StrictConfigDoesNotFreezeAllConfig) {
  StateMachine<Bug6State> sm(Bug6State::A, "Bug6");
  sm.withLogLevel(StateMachine<Bug6State>::LogLevel::NONE);
  sm.withStrictConfig(true);

  double fakeTime = 0.0;
  sm.withClock([&] { return fakeTime; });

  sm.addState(Bug6State::A, "A", std::make_unique<Bug6StateA>());
  sm.addState(Bug6State::B, "B", std::make_unique<Bug6StateB>());
  sm.start();

  // Correct: with strict config, post-start changes should be rejected.
  // We verify by adding a timeout after start and checking it does NOT fire.
  sm.withStateTimeout(Bug6State::A, 5.0, Bug6State::B, "post-start timeout");

  fakeTime = 200.0; // well past the timeout
  sm.update();

  // If strict config worked, the timeout was rejected and the machine stays in A.
  EXPECT_EQ(sm.getCurrentStateId(), Bug6State::A)
      << "Strict config failed to reject post-start withStateTimeout — "
         "the illegally added timeout fired";
}

// Bug 6b: changing the clock after startup causes time jump

TEST(StateMachineBugs, ClockChangeCausesTimeJump) {
  StateMachine<Bug6State> sm(Bug6State::A, "Bug6b");
  sm.withLogLevel(StateMachine<Bug6State>::LogLevel::NONE);

  double fakeTime = 100.0;
  sm.withClock([&] { return fakeTime; });

  sm.addState(Bug6State::A, "A", std::make_unique<Bug6StateA>());
  sm.addState(Bug6State::B, "B", std::make_unique<Bug6StateB>());
  sm.start();

  // stateEntryTimestamp is now 100.0
  fakeTime = 105.0;
  EXPECT_NEAR(sm.timeInState(), 5.0, 0.01);

  // Bug: change the clock to a completely different epoch
  double newTime = 1000.0;
  sm.withClock([&] { return newTime; });

  // Correct: stateEntryTimestamp should be rebased, so timeInState stays ~5s.
  double tis = sm.timeInState();
  EXPECT_NEAR(tis, 5.0, 1.0)
      << "Changing clock after start causes timeInState to jump to " << tis
      << "s because stateEntryTimestamp is not rebased";
}

// ---------------------------------------------------------------------------
// Bug 7 (Medium): logging is not fully thread-safe or exception-safe
// log() reads logSink and useColors without synchronization.
// A throwing custom sink can escape stop() during destruction.
// ---------------------------------------------------------------------------

enum class Bug7State { A, B };

struct Bug7StateA : StateMachine<Bug7State>::State {
  bool enter() override { return true; }
};

struct Bug7StateB : StateMachine<Bug7State>::State {
  bool enter() override { return true; }
};

TEST(StateMachineBugs, LoggingDataRace) {
  // This test is primarily a TSAN/ASAN target. Under TSAN it would report
  // a data race on logSink/useColors. Without TSAN it typically passes.
  StateMachine<Bug7State> sm(Bug7State::A, "Bug7");
  sm.withLogLevel(StateMachine<Bug7State>::LogLevel::DEBUG);

  sm.addState(Bug7State::A, "A", std::make_unique<Bug7StateA>());
  sm.addState(Bug7State::B, "B", std::make_unique<Bug7StateB>());
  sm.start();

  std::atomic<bool> stop{false};

  std::thread t1([&] {
    while (!stop) sm.update();
  });

  std::thread t2([&] {
    int counter = 0;
    while (!stop) {
      counter++;
      if (counter % 2 == 0) {
        sm.withLogSink([](auto, auto) {});
      } else {
        sm.withLogSink(nullptr);
      }
      sm.withColors(counter % 2 == 0);
    }
  });

  std::this_thread::sleep_for(std::chrono::milliseconds(50));
  stop = true;
  t1.join();
  t2.join();

  SUCCEED() << "Race exercised — run under TSAN to detect the data race";
}

TEST(StateMachineBugs, ThrowingSinkEscapesStop) {
  StateMachine<Bug7State> sm(Bug7State::A, "Bug7b");
  sm.withLogLevel(StateMachine<Bug7State>::LogLevel::INFO);
  sm.withColors(false);

  sm.addState(Bug7State::A, "A", std::make_unique<Bug7StateA>());
  sm.start();

  // Install a sink that throws on any message
  sm.withLogSink([](auto, auto) { throw std::runtime_error("sink throws"); });

  // Correct: stop() should catch sink exceptions internally.
  // stop() logs "State machine stopped" at INFO level, which goes through
  // the throwing sink. If the bug is present, the exception escapes.
  EXPECT_NO_THROW({
    try {
      sm.stop();
    } catch (...) {
      FAIL() << "Throwing sink escaped stop() — not caught internally";
    }
  });
}

// ---------------------------------------------------------------------------
// Bug 8 (Medium): escaped states contain a dangling machine pointer
// States contain a raw StateMachine*, while getCurrentState() lets a
// shared_ptr<State> escape. If that state outlives the machine, its
// convenience methods dereference a destroyed machine.
// ---------------------------------------------------------------------------

enum class Bug8State { A, B };

struct Bug8StateA : StateMachine<Bug8State>::State {
  bool enter() override { return true; }
};

struct Bug8StateB : StateMachine<Bug8State>::State {
  bool enter() override { return true; }
};

TEST(StateMachineBugs, EscapedStateDanglingPointer) {
  std::shared_ptr<StateMachine<Bug8State>::State> escaped;

  {
    StateMachine<Bug8State> sm(Bug8State::A, "Bug8");
    sm.withLogLevel(StateMachine<Bug8State>::LogLevel::NONE);
    sm.addState(Bug8State::A, "A", std::make_unique<Bug8StateA>());
    sm.addState(Bug8State::B, "B", std::make_unique<Bug8StateB>());
    sm.start();

    escaped = sm.getCurrentState();
    ASSERT_NE(escaped, nullptr);
  } // sm is now destroyed

  // Correct: stateMachine should be cleared to nullptr during destruction.
  auto *rawMachinePtr = escaped->getStateMachine();
  EXPECT_EQ(rawMachinePtr, nullptr)
      << "stateMachine pointer is dangling — machine was destroyed but "
         "pointer was not cleared";
}

// ---------------------------------------------------------------------------
// Bug 9 (Medium): null child machine causes immediate dereference
// The public child-machine overload accepts a null unique_ptr, wraps it,
// and registerState() immediately invokes methods through it.
// ---------------------------------------------------------------------------

enum class Bug9State { A, B };
enum class Bug9Child { X };

struct Bug9StateA : StateMachine<Bug9State>::State {
  bool enter() override { return true; }
};

TEST(StateMachineBugs, NullChildMachineDereference) {
  // Correct: addState with a null child machine should be rejected gracefully,
  // not crash with a null dereference.
  pid_t pid = fork();
  if (pid == 0) {
    StateMachine<Bug9State> sm(Bug9State::A, "Bug9");
    sm.withLogLevel(StateMachine<Bug9State>::LogLevel::NONE);
    sm.addState(Bug9State::A, "A", std::make_unique<Bug9StateA>());

    std::unique_ptr<StateMachine<Bug9Child>> nullChild{nullptr};
    sm.addState(Bug9State::B, "B_with_null_child",
                std::make_unique<Bug9StateA>(), std::move(nullChild));
    _exit(0); // If we reach here, the null was handled gracefully
  }

  int status = 0;
  waitpid(pid, &status, 0);
  // Correct: the child should exit normally (not crash).
  EXPECT_FALSE(WIFSIGNALED(status))
      << "Null child machine caused a crash (SIGSEGV) instead of graceful rejection";
  if (WIFEXITED(status)) {
    EXPECT_EQ(WEXITSTATUS(status), 0)
        << "Child process should have exited normally";
  }
}

// ============================================================================
// LIVENESS / COOPERATIVE UPDATE CANCELLATION
// ============================================================================
// update() runs State::update() without the machine lock, so another thread
// can transition away mid-update. These cover the primitives that let a
// long-running update() notice and bail out.
// ---------------------------------------------------------------------------

enum class LiveState { A, B, C };

struct LiveProbe : StateMachine<LiveState>::State {
  // Observations recorded from inside the lifecycle hooks.
  bool currentInsideEnter = false;
  bool currentInsideUpdate = false;
  bool cancelledInsideUpdate = false;
  std::uint64_t epochInsideUpdate = 0;
  int updateCount = 0;

  // Optional behaviour knobs.
  std::function<void(LiveProbe &)> onUpdate;

  bool enter() override {
    currentInsideEnter = isCurrentState();
    return true;
  }

  void update() override {
    ++updateCount;
    epochInsideUpdate = transitionEpoch();
    currentInsideUpdate = isCurrentState();
    if (onUpdate) {
      onUpdate(*this);
    }
    cancelledInsideUpdate = shouldCancelUpdate();
  }
};

namespace {

// Builds a started 3-state machine and hands back the raw state probes.
struct LiveFixture {
  StateMachine<LiveState> sm{LiveState::A, "Live"};
  LiveProbe *a = nullptr;
  LiveProbe *b = nullptr;
  LiveProbe *c = nullptr;

  LiveFixture() {
    auto stateA = std::make_unique<LiveProbe>();
    auto stateB = std::make_unique<LiveProbe>();
    auto stateC = std::make_unique<LiveProbe>();
    a = stateA.get();
    b = stateB.get();
    c = stateC.get();
    sm.withLogLevel(StateMachine<LiveState>::LogLevel::NONE);
    sm.addState(LiveState::A, "A", std::move(stateA))
        .addState(LiveState::B, "B", std::move(stateB))
        .addState(LiveState::C, "C", std::move(stateC));
  }
};

} // namespace

TEST(StateMachineLiveness, StateIdRecordedOnRegistration) {
  LiveFixture fx;
  EXPECT_EQ(fx.a->stateId(), LiveState::A);
  EXPECT_EQ(fx.b->stateId(), LiveState::B);
  EXPECT_EQ(fx.c->stateId(), LiveState::C);
}

TEST(StateMachineLiveness, IsCurrentStateTracksActiveState) {
  LiveFixture fx;
  fx.sm.start();

  EXPECT_TRUE(fx.a->isCurrentState());
  EXPECT_FALSE(fx.b->isCurrentState());

  fx.sm.changeState(LiveState::B, "go");

  EXPECT_FALSE(fx.a->isCurrentState());
  EXPECT_TRUE(fx.b->isCurrentState());
}

TEST(StateMachineLiveness, IsCurrentStateTrueInsideEnter) {
  // Arrival is committed before enter() runs, so a state entering itself
  // already observes that it is current. (StateMachineCpp gets this wrong by
  // assigning currentStateId only after enter() returns.)
  LiveFixture fx;
  fx.sm.start();
  EXPECT_TRUE(fx.a->currentInsideEnter);

  fx.sm.changeState(LiveState::B, "go");
  EXPECT_TRUE(fx.b->currentInsideEnter);
}

TEST(StateMachineLiveness, EpochAdvancesOnlyOnCommittedTransitions) {
  LiveFixture fx;
  fx.sm.start();

  const auto afterStart = fx.sm.transitionEpoch();
  EXPECT_TRUE(fx.sm.stillActive(afterStart));

  // A no-op transition to the active state must not advance the epoch.
  fx.sm.changeState(LiveState::A, "same state");
  EXPECT_EQ(fx.sm.transitionEpoch(), afterStart);
  EXPECT_TRUE(fx.sm.stillActive(afterStart));

  // A real transition must.
  fx.sm.changeState(LiveState::B, "go");
  const auto afterMove = fx.sm.transitionEpoch();
  EXPECT_NE(afterMove, afterStart);
  EXPECT_FALSE(fx.sm.stillActive(afterStart));
  EXPECT_TRUE(fx.sm.stillActive(afterMove));

  // Explicit re-entry counts as a commit.
  fx.sm.reenterState("restart");
  EXPECT_FALSE(fx.sm.stillActive(afterMove));

  // So does stop().
  const auto beforeStop = fx.sm.transitionEpoch();
  fx.sm.stop();
  EXPECT_FALSE(fx.sm.stillActive(beforeStop));
}

TEST(StateMachineLiveness, EpochUnchangedByRejectedTransition) {
  LiveFixture fx;
  fx.sm.withAllowedTransition(LiveState::A, LiveState::B);
  fx.sm.start();

  const auto epoch = fx.sm.transitionEpoch();
  EXPECT_FALSE(fx.sm.changeState(LiveState::C, "not whitelisted"));
  EXPECT_TRUE(fx.sm.stillActive(epoch));
}

TEST(StateMachineLiveness, StillActiveDetectsAbaRoundTrip) {
  // The core reason to prefer stillActive() over isCurrentState(): after
  // A -> B -> A the machine is in A again, so isCurrentState() is true, but a
  // fresh enter() has run and any in-flight work from the first visit is stale.
  LiveFixture fx;
  fx.sm.start();

  const auto epoch = fx.sm.transitionEpoch();
  fx.sm.changeState(LiveState::B, "away");
  fx.sm.changeState(LiveState::A, "back");

  EXPECT_TRUE(fx.a->isCurrentState()) << "ABA leaves A current again";
  EXPECT_FALSE(fx.sm.stillActive(epoch))
      << "stillActive() must still report the first visit as superseded";
}

TEST(StateMachineLiveness, ShouldCancelUpdateFalseWhenUndisturbed) {
  LiveFixture fx;
  fx.sm.start();
  fx.sm.update();

  EXPECT_EQ(fx.a->updateCount, 1);
  EXPECT_TRUE(fx.a->currentInsideUpdate);
  EXPECT_FALSE(fx.a->cancelledInsideUpdate);
}

TEST(StateMachineLiveness, ShouldCancelUpdateAfterSelfTransition) {
  // A state that transitions away from inside its own update() must see the
  // cancellation flag for the remainder of that call.
  LiveFixture fx;
  fx.a->onUpdate = [](LiveProbe &self) {
    self.changeToState(LiveState::B, "from update");
  };
  fx.sm.start();
  fx.sm.update();

  EXPECT_EQ(fx.sm.getCurrentStateId(), LiveState::B);
  EXPECT_TRUE(fx.a->cancelledInsideUpdate);
  EXPECT_FALSE(fx.a->isCurrentState());
}

TEST(StateMachineLiveness, ShouldCancelUpdateResetsEachCycle) {
  LiveFixture fx;
  fx.sm.start();

  fx.b->onUpdate = [](LiveProbe &) {};
  fx.a->onUpdate = [](LiveProbe &self) {
    self.changeToState(LiveState::B, "from update");
  };
  fx.sm.update(); // cancels: A leaves during its own update
  ASSERT_TRUE(fx.a->cancelledInsideUpdate);

  fx.sm.update(); // fresh cycle in B, nothing disturbs it
  EXPECT_EQ(fx.b->updateCount, 1);
  EXPECT_FALSE(fx.b->cancelledInsideUpdate);
}

TEST(StateMachineLiveness, StoppedMachineCancelsInFlightUpdate) {
  LiveFixture fx;
  fx.a->onUpdate = [](LiveProbe &self) {
    self.getStateMachine()->stop();
  };
  fx.sm.start();
  fx.sm.update();

  EXPECT_FALSE(fx.sm.isReady());
  EXPECT_TRUE(fx.a->cancelledInsideUpdate)
      << "stop() bumps the epoch, so in-flight update work is cancelled";
}

TEST(StateMachineLiveness, LongUpdateBailsOutOnCrossThreadTransition) {
  // The hole this API closes: update() holds no lock, so a transition from
  // another thread must be observable mid-update.
  StateMachine<LiveState> sm(LiveState::A, "LiveThreaded");
  sm.withLogLevel(StateMachine<LiveState>::LogLevel::NONE);

  std::atomic<bool> insideUpdate{false};
  std::atomic<bool> bailedOut{false};
  std::atomic<int> iterations{0};

  struct LongUpdateState : StateMachine<LiveState>::State {
    std::atomic<bool> *insideUpdate;
    std::atomic<bool> *bailedOut;
    std::atomic<int> *iterations;

    void update() override {
      const auto epoch = transitionEpoch();
      insideUpdate->store(true);
      // Bounded so the test cannot hang if cancellation never arrives.
      for (int i = 0; i < 100000; ++i) {
        iterations->fetch_add(1);
        if (!stillActive(epoch)) {
          bailedOut->store(true);
          return;
        }
        std::this_thread::sleep_for(std::chrono::microseconds(10));
      }
    }
  };

  auto longState = std::make_unique<LongUpdateState>();
  longState->insideUpdate = &insideUpdate;
  longState->bailedOut = &bailedOut;
  longState->iterations = &iterations;

  sm.addState(LiveState::A, "A", std::move(longState))
      .addState<LiveProbe>(LiveState::B, "B");
  sm.start();

  std::thread updater([&] { sm.update(); });

  // Wait for update() to actually be running before interrupting it.
  while (!insideUpdate.load()) {
    std::this_thread::sleep_for(std::chrono::microseconds(100));
  }
  sm.changeState(LiveState::B, "cross-thread interrupt");

  updater.join();

  EXPECT_TRUE(bailedOut.load())
      << "long update() did not observe the cross-thread transition";
  EXPECT_LT(iterations.load(), 100000)
      << "update() ran to completion instead of cancelling early";
  EXPECT_EQ(sm.getCurrentStateId(), LiveState::B);
}

TEST(StateMachineLiveness, DetachedStateReportsSafeDefaults) {
  // A state that was never registered has no machine back-pointer; the
  // helpers must not dereference it.
  LiveProbe orphan;
  EXPECT_FALSE(orphan.isCurrentState());
  EXPECT_FALSE(orphan.shouldCancelUpdate());
  EXPECT_EQ(orphan.transitionEpoch(), 0u);
  EXPECT_FALSE(orphan.stillActive(0));
}

// ============================================================================
// END OF TEST SUITE
// ============================================================================