/**
 * @file regions_example.cpp
 * @brief Example demonstrating orthogonal regions and hierarchical sub-states
 *
 * This example models a robot with:
 * - A parent state machine for high-level mission phases
 * - Orthogonal regions within the "Operating" state:
 *   - Motion region: tracks movement sub-states (Idle → Moving → Stopped)
 *   - Sensor region: tracks sensor sub-states (Calibrating → Active → Standby)
 * - A single sub-state machine (withSubStates) within the "Docked" state
 */

#include "StateMachine.hpp"
#include <iostream>
#include <string>

// ── Parent state enum ──────────────────────────────────────────────
enum class RobotState {
    DOCKED,
    OPERATING,
    EMERGENCY
};

inline std::ostream &operator<<(std::ostream &os, RobotState s) {
    switch (s) {
    case RobotState::DOCKED:    return os << "DOCKED";
    case RobotState::OPERATING: return os << "OPERATING";
    case RobotState::EMERGENCY: return os << "EMERGENCY";
    default: return os << "UNKNOWN";
    }
}

// ── Motion sub-state enum (for the Motion region) ─────────────────
enum class MotionState {
    MOTION_IDLE,
    MOVING,
    MOTION_STOPPED
};

// ── Sensor sub-state enum (for the Sensor region) ─────────────────
enum class SensorState {
    CALIBRATING,
    SENSOR_ACTIVE,
    SENSOR_STANDBY
};

// ── Dock sub-state enum (for the Docked sub-machine) ──────────────
enum class DockSubState {
    CHARGING,
    DOCK_READY
};

// ── Parent states ─────────────────────────────────────────────────

class DockedState : public StateMachine<RobotState>::State {
public:
    bool enter() override {
        std::cout << "  [Docked] Robot is docked" << std::endl;
        return true;
    }

    void update() override {
        static int tick = 0;
        if (++tick >= 3) {
            tick = 0;
            std::cout << "  [Docked] Fully charged — deploying" << std::endl;
            changeToState(RobotState::OPERATING, "Deploying");
        }
    }

    void exit() override {
        std::cout << "  [Docked] Undocking" << std::endl;
    }
};

class OperatingState : public StateMachine<RobotState>::State {
public:
    bool enter() override {
        std::cout << "  [Operating] Robot is now operating" << std::endl;
        return true;
    }

    void update() override {
        static int tick = 0;
        if (++tick >= 8) {
            tick = 0;
            std::cout << "  [Operating] Mission complete — returning to dock" << std::endl;
            changeToState(RobotState::DOCKED, "Mission complete");
        }
    }

    void exit() override {
        std::cout << "  [Operating] Ceasing operations" << std::endl;
    }
};

class EmergencyState : public StateMachine<RobotState>::State {
public:
    bool enter() override {
        std::cout << "  [Emergency] Emergency stop!" << std::endl;
        return true;
    }

    void update() override {
        static int tick = 0;
        if (++tick >= 2) {
            tick = 0;
            std::cout << "  [Emergency] Recovered — docking" << std::endl;
            changeToState(RobotState::DOCKED, "Recovered from emergency");
        }
    }
};

// ── Motion region states ──────────────────────────────────────────

class MotionIdleState : public StateMachine<MotionState>::State {
public:
    bool enter() override {
        std::cout << "    [Motion/Idle] Motors idle" << std::endl;
        return true;
    }
    void update() override {
        static int tick = 0;
        if (++tick >= 2) {
            tick = 0;
            changeToState(MotionState::MOVING, "Starting to move");
        }
    }
};

class MovingState : public StateMachine<MotionState>::State {
public:
    bool enter() override {
        std::cout << "    [Motion/Moving] Robot is moving" << std::endl;
        return true;
    }
    void update() override {
        static int tick = 0;
        if (++tick >= 3) {
            tick = 0;
            changeToState(MotionState::MOTION_STOPPED, "Reached destination");
        }
    }
};

class MotionStoppedState : public StateMachine<MotionState>::State {
public:
    bool enter() override {
        std::cout << "    [Motion/Stopped] Robot has stopped" << std::endl;
        return true;
    }
    void update() override {
        static int tick = 0;
        if (++tick >= 2) {
            tick = 0;
            changeToState(MotionState::MOTION_IDLE, "Ready to move again");
        }
    }
};

// ── Sensor region states ──────────────────────────────────────────

class CalibratingState : public StateMachine<SensorState>::State {
public:
    bool enter() override {
        std::cout << "    [Sensor/Calibrating] Calibrating sensors" << std::endl;
        return true;
    }
    void update() override {
        static int tick = 0;
        if (++tick >= 2) {
            tick = 0;
            changeToState(SensorState::SENSOR_ACTIVE, "Calibration complete");
        }
    }
};

class SensorActiveState : public StateMachine<SensorState>::State {
public:
    bool enter() override {
        std::cout << "    [Sensor/Active] Sensors active" << std::endl;
        return true;
    }
    void update() override {
        static int tick = 0;
        if (++tick >= 4) {
            tick = 0;
            changeToState(SensorState::SENSOR_STANDBY, "Entering standby");
        }
    }
};

class SensorStandbyState : public StateMachine<SensorState>::State {
public:
    bool enter() override {
        std::cout << "    [Sensor/Standby] Sensors on standby" << std::endl;
        return true;
    }
    void update() override {
        static int tick = 0;
        if (++tick >= 2) {
            tick = 0;
            changeToState(SensorState::SENSOR_ACTIVE, "Reactivating sensors");
        }
    }
};

// ── Dock sub-states (single sub-machine, not orthogonal regions) ──

class ChargingState : public StateMachine<DockSubState>::State {
public:
    bool enter() override {
        std::cout << "    [Dock/Charging] Charging battery" << std::endl;
        return true;
    }
    void update() override {
        static int tick = 0;
        if (++tick >= 2) {
            tick = 0;
            changeToState(DockSubState::DOCK_READY, "Charging complete");
        }
    }
};

class DockReadyState : public StateMachine<DockSubState>::State {
public:
    bool enter() override {
        std::cout << "    [Dock/Ready] Docked and ready" << std::endl;
        return true;
    }
};

// ── Helper: print the full state path ─────────────────────────────

void printStatus(StateMachine<RobotState> &sm) {
    std::cout << "  Path: " << sm.statePath() << std::endl;
    std::cout << "  Motion region: " << sm.getActiveRegionStateName("Motion") << std::endl;
    std::cout << "  Sensor region: " << sm.getActiveRegionStateName("Sensor") << std::endl;
}

// ── Main ──────────────────────────────────────────────────────────

int main() {
    std::cout << "\n=== Robot with Orthogonal Regions Example ===\n" << std::endl;

    StateMachine<RobotState> robot(RobotState::DOCKED, "Robot");

    robot.addState<DockedState>(RobotState::DOCKED, "Docked")
        .addState<OperatingState>(RobotState::OPERATING, "Operating")
        .addState<EmergencyState>(RobotState::EMERGENCY, "Emergency")
        .withFallback(RobotState::EMERGENCY)
        .withLogLevel(StateMachine<RobotState>::LogLevel::INFO)

        // Single sub-state machine inside Docked (no orthogonal regions)
        .withSubStates<DockSubState>(
            RobotState::DOCKED, DockSubState::CHARGING,
            [](auto &dock) {
                dock.template addState<ChargingState>(DockSubState::CHARGING, "Charging")
                    .template addState<DockReadyState>(DockSubState::DOCK_READY, "Ready");
            })

        // Orthogonal region "Motion" inside Operating
        .withRegion<MotionState>(
            RobotState::OPERATING, "Motion", MotionState::MOTION_IDLE,
            [](auto &motion) {
                motion.template addState<MotionIdleState>(MotionState::MOTION_IDLE, "Idle")
                    .template addState<MovingState>(MotionState::MOVING, "Moving")
                    .template addState<MotionStoppedState>(MotionState::MOTION_STOPPED, "Stopped");
            })

        // Orthogonal region "Sensor" inside Operating (runs in parallel with Motion)
        .withRegion<SensorState>(
            RobotState::OPERATING, "Sensor", SensorState::CALIBRATING,
            [](auto &sensor) {
                sensor.template addState<CalibratingState>(SensorState::CALIBRATING, "Calibrating")
                    .template addState<SensorActiveState>(SensorState::SENSOR_ACTIVE, "Active")
                    .template addState<SensorStandbyState>(SensorState::SENSOR_STANDBY, "Standby");
            })

        .onStateChanged([](const RobotState &, const RobotState &,
                           std::string_view fromName, std::string_view toName,
                           std::string_view reason) {
            std::cout << "  >> " << fromName << " -> " << toName;
            if (!reason.empty()) std::cout << " (" << reason << ")";
            std::cout << std::endl;
        });

    // Validate before starting
    if (!robot.validate()) {
        std::cout << "Configuration invalid!" << std::endl;
        return 1;
    }

    std::cout << "Starting robot...\n" << std::endl;
    robot.start();

    // Run the mission
    for (int i = 0; i < 20; ++i) {
        std::cout << "\n--- Tick " << (i + 1) << " ---" << std::endl;
        robot.update();
        printStatus(robot);
    }

    std::cout << "\n=== Regions example complete ===" << std::endl;
    return 0;
}
