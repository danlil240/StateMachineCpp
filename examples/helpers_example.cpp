/**
 * @file helpers_example.cpp
 * @brief Demonstrates the convenience helper functions added to StateMachine
 *
 * Covers: isInState(), getStateName(), getRegisteredStateIds(), getMachineName(),
 *         getInitialStateId(), hasFallback(), getFallbackStateId(), isStopped(),
 *         getHistorySize(), clearHistory(), log(), runUntil(), runFor(),
 *         runInThread(), stopThread(), and State::log().
 */

#include "StateMachine.hpp"
#include <iostream>
#include <thread>

// ---------------------------------------------------------------------------
//  Enum + state classes
// ---------------------------------------------------------------------------

enum class RobotState { IDLE, MOVING, SCANNING, CHARGING, ERROR };

class IdleState : public StateMachine<RobotState>::State {
public:
  bool enter() override {
    log(StateMachine<RobotState>::LogLevel::INFO, "Robot idle — waiting for command");
    return true;
  }
  void update() override {
    if (updateCount_++ >= 3) {
      changeToState(RobotState::MOVING, "Mission received");
    }
  }
  void exit() override { log(StateMachine<RobotState>::LogLevel::DEBUG, "Leaving idle"); }

private:
  int updateCount_ = 0;
};

class MovingState : public StateMachine<RobotState>::State {
public:
  bool enter() override {
    log(StateMachine<RobotState>::LogLevel::INFO, "Robot moving toward target");
    return true;
  }
  void update() override {
    if (updateCount_++ >= 5) {
      changeToState(RobotState::SCANNING, "Reached target area");
    }
  }
  void exit() override { log(StateMachine<RobotState>::LogLevel::DEBUG, "Stopping motors"); }

private:
  int updateCount_ = 0;
};

class ScanningState : public StateMachine<RobotState>::State {
public:
  bool enter() override {
    log(StateMachine<RobotState>::LogLevel::INFO, "Scanning environment...");
    return true;
  }
  void update() override {
    if (updateCount_++ >= 4) {
      changeToState(RobotState::CHARGING, "Scan complete — heading to dock");
    }
  }
  void exit() override { log(StateMachine<RobotState>::LogLevel::DEBUG, "Scan finished"); }

private:
  int updateCount_ = 0;
};

class ChargingState : public StateMachine<RobotState>::State {
public:
  bool enter() override {
    log(StateMachine<RobotState>::LogLevel::INFO, "Charging battery...");
    return true;
  }
  void update() override {
    if (updateCount_++ >= 3) {
      changeToState(RobotState::IDLE, "Fully charged");
    }
  }
  void exit() override { log(StateMachine<RobotState>::LogLevel::DEBUG, "Disconnecting charger"); }

private:
  int updateCount_ = 0;
};

class ErrorState : public StateMachine<RobotState>::State {
public:
  bool enter() override {
    log(StateMachine<RobotState>::LogLevel::ERROR, "Robot entered error state!");
    return true;
  }
};

// ---------------------------------------------------------------------------
//  Helpers for pretty-printing
// ---------------------------------------------------------------------------

const char *stateStr(RobotState s) {
  switch (s) {
  case RobotState::IDLE:     return "IDLE";
  case RobotState::MOVING:   return "MOVING";
  case RobotState::SCANNING: return "SCANNING";
  case RobotState::CHARGING: return "CHARGING";
  case RobotState::ERROR:    return "ERROR";
  }
  return "?";
}

void printMachineInfo(const StateMachine<RobotState> &sm) {
  std::cout << "\n── Machine Info ──────────────────────────────\n";
  std::cout << "  Name:           " << sm.getMachineName() << "\n";
  std::cout << "  Ready:          " << (sm.isReady() ? "yes" : "no") << "\n";
  std::cout << "  Stopped:        " << (sm.isStopped() ? "yes" : "no") << "\n";
  std::cout << "  Current state:  " << sm.getCurrentStateName() << "\n";
  std::cout << "  Initial state:  " << stateStr(sm.getInitialStateId()) << "\n";
  std::cout << "  State count:    " << sm.getStateCount() << "\n";
  std::cout << "  Has fallback:   " << (sm.hasFallback() ? "yes" : "no") << "\n";
  std::cout << "  History size:   " << sm.getHistorySize() << "\n";

  std::cout << "  Registered IDs: ";
  for (auto id : sm.getRegisteredStateIds()) {
    std::cout << stateStr(id) << " ";
  }
  std::cout << "\n";

  std::cout << "  State names:\n";
  for (auto id : sm.getRegisteredStateIds()) {
    std::cout << "    " << stateStr(id) << " -> " << sm.getStateName(id) << "\n";
  }
  std::cout << "──────────────────────────────────────────────\n";
}

// ---------------------------------------------------------------------------
//  main
// ---------------------------------------------------------------------------

int main() {
  std::cout << "================================================================\n"
            << "  StateMachine Helper Functions Demo\n"
            << "================================================================\n";

  StateMachine<RobotState> robot(RobotState::IDLE, "RobotController");

  robot.addState<IdleState>(RobotState::IDLE, "Idle")
      .addState<MovingState>(RobotState::MOVING, "Moving")
      .addState<ScanningState>(RobotState::SCANNING, "Scanning")
      .addState<ChargingState>(RobotState::CHARGING, "Charging")
      .addState<ErrorState>(RobotState::ERROR, "Error")
      .withFallback(RobotState::IDLE)
      .onStateChanged([](const RobotState & /*from*/, const RobotState & /*to*/,
                         auto fromName, auto toName, auto reason) {
        std::cout << "  [transition] " << fromName << " -> " << toName
                  << "  (" << reason << ")\n";
      });

  // ── Query helpers before start ──────────────────────────────────────
  std::cout << "\n[1] Query helpers before start()\n";
  printMachineInfo(robot);

  // ── Public log helper ───────────────────────────────────────────────
  std::cout << "\n[2] Public log() helper\n";
  using L = StateMachine<RobotState>::LogLevel;
  robot.log(L::WARN, "This is a warning from application code");
  robot.log(L::INFO, "This is an info from application code");

  // ── runUntil: run until we reach a target state ─────────────────────
  std::cout << "\n[3] runUntil() — run until SCANNING is reached\n";
  robot.start();
  bool reached = robot.runUntil(RobotState::SCANNING, 100);
  std::cout << "  reached SCANNING: " << (reached ? "yes" : "no") << "\n";
  std::cout << "  isInState(SCANNING): "
            << (robot.isInState(RobotState::SCANNING) ? "yes" : "no") << "\n";
  std::cout << "  History size: " << robot.getHistorySize() << "\n";

  // ── Clear history ───────────────────────────────────────────────────
  std::cout << "\n[4] clearHistory()\n";
  robot.clearHistory();
  std::cout << "  History size after clear: " << robot.getHistorySize() << "\n";

  // ── runFor: run for a fixed wall-clock duration ─────────────────────
  std::cout << "\n[5] runFor() — run for 0.3 seconds at 50 Hz\n";
  robot.runFor(0.3, 20);
  std::cout << "  Current state after runFor: " << robot.getCurrentStateName()
            << "\n";

  // ── runInThread: background update thread ───────────────────────────
  std::cout << "\n[6] runInThread() — background updates at 50 Hz\n";
  robot.runInThread(0.02); // 50 Hz
  std::cout << "  Thread running, main thread sleeps 0.5s...\n";
  std::this_thread::sleep_for(std::chrono::milliseconds(500));

  // Query from the main thread while the background thread updates
  std::cout << "  Current state (queried from main): "
            << robot.getCurrentStateName() << "\n";
  std::cout << "  isInState(IDLE): "
            << (robot.isInState(RobotState::IDLE) ? "yes" : "no") << "\n";

  // Stop the background thread explicitly
  robot.stopThread();
  std::cout << "  Background thread stopped.\n";

  // ── Final query ─────────────────────────────────────────────────────
  std::cout << "\n[7] Final machine info\n";
  printMachineInfo(robot);

  std::cout << "\n  Transition history:\n";
  for (const auto &rec : robot.getTransitionHistory()) {
    std::cout << "    " << stateStr(rec.state) << " @ " << rec.timestamp
              << "s  (" << rec.reason << ")\n";
  }

  // ── stop() joins the thread automatically if still running ──────────
  std::cout << "\n[8] stop()\n";
  robot.stop();
  std::cout << "  isStopped(): " << (robot.isStopped() ? "yes" : "no") << "\n";
  std::cout << "  isReady():   " << (robot.isReady() ? "yes" : "no") << "\n";

  // ── Destructor also joins automatically — safe to let go out of scope
  std::cout << "\n  (Destructor will join any remaining thread automatically)\n";

  std::cout << "\n================================================================\n"
            << "  Demo complete — all helpers work correctly!\n"
            << "================================================================\n";
  return 0;
}
