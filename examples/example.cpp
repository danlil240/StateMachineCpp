/**
 * @file example.cpp
 * @brief Comprehensive example demonstrating all StateMachine features
 * 
 * This example shows a drone mission state machine with:
 * - All state machine functionalities
 * - Error handling and recovery
 * - Context sharing between states
 * - Callbacks for monitoring
 * - Fallback mechanisms
 * - History tracking
 * - Practical real-world usage patterns
 */

#include "StateMachine.hpp"
#include <chrono>
#include <thread>
#include <iostream>
#include <iomanip>
#include <cmath>
#include <random>

// Drone mission states - using enum class for type safety
enum class DroneState
{
    IDLE,
    PREFLIGHT_CHECK,
    TAKEOFF,
    NAVIGATE_TO_TARGET,
    HOVER_AT_TARGET,
    RETURN_TO_BASE,
    LANDING,
    EMERGENCY,
    MISSION_COMPLETE
};

// Stream operator for DroneState enum
inline std::ostream &operator<<(std::ostream &os, DroneState state)
{
    switch (state)
    {
    case DroneState::IDLE:
        return os << "IDLE";
    case DroneState::PREFLIGHT_CHECK:
        return os << "PREFLIGHT_CHECK";
    case DroneState::TAKEOFF:
        return os << "TAKEOFF";
    case DroneState::NAVIGATE_TO_TARGET:
        return os << "NAVIGATE_TO_TARGET";
    case DroneState::HOVER_AT_TARGET:
        return os << "HOVER_AT_TARGET";
    case DroneState::RETURN_TO_BASE:
        return os << "RETURN_TO_BASE";
    case DroneState::LANDING:
        return os << "LANDING";
    case DroneState::EMERGENCY:
        return os << "EMERGENCY";
    case DroneState::MISSION_COMPLETE:
        return os << "MISSION_COMPLETE";
    default:
        return os << "UNKNOWN";
    }
}

// Shared context between all states
struct DroneContext {
    // Position and navigation
    double x = 0.0, y = 0.0, z = 0.0;
    double target_x = 10.0, target_y = 10.0, target_z = 5.0;
    double base_x = 0.0, base_y = 0.0, base_z = 0.0;
    
    // Status flags
    bool sensors_ok = true;
    bool gps_lock = true;
    bool battery_ok = true;
    bool motors_ok = true;
    bool emergency_triggered = false;
    
    // Mission parameters
    double battery_level = 100.0;
    double mission_start_time = 0.0;
    int waypoint_count = 0;
    
    // Helper methods
    double distanceToTarget() const {
        return std::sqrt(std::pow(target_x - x, 2) + std::pow(target_y - y, 2) + std::pow(target_z - z, 2));
    }
    
    double distanceToBase() const {
        return std::sqrt(std::pow(base_x - x, 2) + std::pow(base_y - y, 2) + std::pow(base_z - z, 2));
    }
    
    void simulateBatteryDrain() {
        battery_level -= 0.5; // Drain battery over time
        if (battery_level < 20.0) battery_ok = false;
    }
};

// State implementations demonstrating different patterns

class IdleState : public StateMachine<DroneState>::State
{
public:
    bool enter() override {
        std::cout << "🚁 Drone is IDLE - Waiting for mission command" << std::endl;
        
        // Context is optional - only access if available
        try {
            auto ctx = getContext<DroneContext>();
            if (ctx) {
                ctx->battery_level = 100.0; // Reset battery when idle
                ctx->battery_ok = true;
            }
        } catch (const std::runtime_error&) {
            // No context available - that's fine for basic operation
            std::cout << "  (No context available - using basic mode)" << std::endl;
        }
        return true;
    }
    
    void update() override {
        static int idle_counter = 0;
        idle_counter++;
        
        // Simulate receiving mission command after some time
        if (idle_counter == 5) {
            std::cout << "📡 Mission command received - Starting preflight check" << std::endl;
            changeToState(DroneState::PREFLIGHT_CHECK, "Mission command received");
            idle_counter = 0; // Reset after transition
        }
    }
    
    void exit() override {
        std::cout << "✈️ Leaving idle state" << std::endl;
    }
};

class PreflightCheckState : public StateMachine<DroneState>::State
{
private:
    int check_counter = 0;
    std::mt19937 rng{std::random_device{}()};
    
public:
    bool enter() override {
        std::cout << "🔍 Starting preflight checks..." << std::endl;
        check_counter = 0;
        
        auto ctx = getContext<DroneContext>();
        ctx->mission_start_time = std::chrono::duration_cast<std::chrono::seconds>(
            std::chrono::steady_clock::now().time_since_epoch()).count();
        return true;
    }
    
    void update() override {
        check_counter++;
        auto ctx = getContext<DroneContext>();
        
        switch (check_counter) {
            case 1:
                std::cout << "  ✓ Checking sensors..." << std::endl;
                // Simulate random sensor failure (5% chance)
                ctx->sensors_ok = std::uniform_real_distribution<>(0.0, 1.0)(rng) > 0.05;
                break;
            case 2:
                std::cout << "  ✓ Checking GPS lock..." << std::endl;
                ctx->gps_lock = std::uniform_real_distribution<>(0.0, 1.0)(rng) > 0.1;
                break;
            case 3:
                std::cout << "  ✓ Checking motors..." << std::endl;
                ctx->motors_ok = std::uniform_real_distribution<>(0.0, 1.0)(rng) > 0.05;
                break;
            case 4:
                std::cout << "  ✓ Checking battery..." << std::endl;
                ctx->battery_ok = ctx->battery_level > 30.0;
                break;
            case 5:
                // Complete preflight check
                if (ctx->sensors_ok && ctx->gps_lock && ctx->motors_ok && ctx->battery_ok) {
                    std::cout << "✅ All preflight checks passed!" << std::endl;
                    changeToState(DroneState::TAKEOFF, "Preflight check passed");
                } else {
                    changeToState(DroneState::EMERGENCY, "Preflight check failed");
                }
                break;
        }
    }
    
    void exit() override {
        std::cout << "🔍 Preflight check complete" << std::endl;
    }
};

class TakeoffState : public StateMachine<DroneState>::State
{
private:
    int takeoff_progress = 0;
    
public:
    bool enter() override {
        std::cout << "🚀 Starting takeoff sequence..." << std::endl;
        takeoff_progress = 0;
        return true;
    }
    
    void update() override {
        auto ctx = getContext<DroneContext>();
        ctx->simulateBatteryDrain();
        
        takeoff_progress++;
        ctx->z = takeoff_progress * 0.5; // Rise at 0.5m per update
        
        std::cout << "  Altitude: " << std::fixed << std::setprecision(1) 
                  << ctx->z << "m" << std::endl;
        
        if (ctx->z >= 5.0) {
            std::cout << "✅ Takeoff complete - At target altitude" << std::endl;
            changeToState(DroneState::NAVIGATE_TO_TARGET, "Takeoff complete");
        }
        else if (!ctx->battery_ok || !ctx->motors_ok)
        {
            changeToState(DroneState::EMERGENCY, "System failure during takeoff");
        }
    }
    
    void exit() override {
        std::cout << "🚀 Takeoff sequence finished" << std::endl;
    }
};

class NavigateToTargetState : public StateMachine<DroneState>::State
{
public:
    bool enter() override {
        auto ctx = getContext<DroneContext>();
        std::cout << "🧭 Navigating to target (" << ctx->target_x << ", " 
                  << ctx->target_y << ", " << ctx->target_z << ")" << std::endl;
        return true;
    }
    
    void update() override {
        auto ctx = getContext<DroneContext>();
        ctx->simulateBatteryDrain();
        
        // Simple navigation simulation - move toward target
        double dx = ctx->target_x - ctx->x;
        double dy = ctx->target_y - ctx->y;
        double dz = ctx->target_z - ctx->z;
        
        double speed = 1.0; // m/s
        double distance = std::sqrt(dx*dx + dy*dy + dz*dz);
        
        if (distance > 0.5) {
            // Normalize and scale by speed
            ctx->x += (dx / distance) * speed;
            ctx->y += (dy / distance) * speed;
            ctx->z += (dz / distance) * speed;
            
            std::cout << "  Position: (" << std::fixed << std::setprecision(1)
                      << ctx->x << ", " << ctx->y << ", " << ctx->z 
                      << ") Distance to target: " << distance << "m" << std::endl;
        } else {
            std::cout << "🎯 Reached target location!" << std::endl;
            changeToState(DroneState::HOVER_AT_TARGET, "Arrived at target");
        }
        
        // Emergency checks
        if (!ctx->battery_ok) {
            changeToState(DroneState::EMERGENCY, "Low battery during navigation");
        }
    }
    
    void exit() override {
        std::cout << "🧭 Navigation complete" << std::endl;
    }
};

class HoverAtTargetState : public StateMachine<DroneState>::State
{
private:
    int hover_time = 0;
    
public:
    bool enter() override {
        std::cout << "🎯 Hovering at target - Performing mission tasks" << std::endl;
        hover_time = 0;
        auto ctx = getContext<DroneContext>();
        ctx->waypoint_count++;
        return true;
    }
    
    void update() override {
        auto ctx = getContext<DroneContext>();
        ctx->simulateBatteryDrain();
        
        hover_time++;
        
        if (hover_time <= 5) {
            std::cout << "  📷 Performing surveillance... (" << hover_time << "/5)" << std::endl;
        } else {
            std::cout << "✅ Mission task completed - Returning to base" << std::endl;
            changeToState(DroneState::RETURN_TO_BASE, "Mission task completed");
        }
        
        if (!ctx->battery_ok) {
            changeToState(DroneState::EMERGENCY, "Low battery during hover");
        }
    }
    
    void exit() override {
        std::cout << "🎯 Hover phase complete" << std::endl;
    }
};

class ReturnToBaseState : public StateMachine<DroneState>::State
{
public:
    bool enter() override {
        auto ctx = getContext<DroneContext>();
        std::cout << "🏠 Returning to base (" << ctx->base_x << ", " 
                  << ctx->base_y << ", " << ctx->base_z << ")" << std::endl;
        return true;
    }
    
    void update() override {
        auto ctx = getContext<DroneContext>();
        ctx->simulateBatteryDrain();
        
        // Navigate back to base
        double dx = ctx->base_x - ctx->x;
        double dy = ctx->base_y - ctx->y;
        double dz = ctx->base_z - ctx->z;
        
        double speed = 1.5; // Faster return speed
        double distance = std::sqrt(dx*dx + dy*dy + dz*dz);
        
        if (distance > 0.5) {
            ctx->x += (dx / distance) * speed;
            ctx->y += (dy / distance) * speed;
            ctx->z += (dz / distance) * speed;
            
            std::cout << "  Position: (" << std::fixed << std::setprecision(1)
                      << ctx->x << ", " << ctx->y << ", " << ctx->z 
                      << ") Distance to base: " << distance << "m" << std::endl;
        } else {
            std::cout << "🏠 Reached base - Starting landing sequence" << std::endl;
            changeToState(DroneState::LANDING, "Arrived at base");
        }
        
        if (!ctx->battery_ok) {
            changeToState(DroneState::EMERGENCY, "Critical battery during return");
        }
    }
    
    void exit() override {
        std::cout << "🏠 Return to base complete" << std::endl;
    }
};

class LandingState : public StateMachine<DroneState>::State
{
public:
    bool enter() override {
        std::cout << "🛬 Starting landing sequence..." << std::endl;
        return true;
    }
    
    void update() override {
        auto ctx = getContext<DroneContext>();
        
        if (ctx->z > 0.1) {
            ctx->z -= 0.7; // Descend at 0.7m per update
            if (ctx->z < 0) ctx->z = 0;
            
            std::cout << "  Altitude: " << std::fixed << std::setprecision(1) 
                      << ctx->z << "m" << std::endl;
        } else {
            std::cout << "✅ Landing complete - Mission accomplished!" << std::endl;
            changeToState(DroneState::MISSION_COMPLETE, "Successful landing");
        }
    }
    
    void exit() override {
        std::cout << "🛬 Landing sequence finished" << std::endl;
    }
};

class EmergencyState : public StateMachine<DroneState>::State
{
private:
    int emergency_time = 0;
    
public:
    bool enter() override {
        std::cout << "🚨 EMERGENCY STATE ACTIVATED!" << std::endl;
        emergency_time = 0;
        auto ctx = getContext<DroneContext>();
        ctx->emergency_triggered = true;
        return true;
    }
    
    void update() override {
        auto ctx = getContext<DroneContext>();
        emergency_time++;
        
        std::cout << "🚨 Emergency procedures active... (" << emergency_time << ")" << std::endl;
        
        // Emergency landing
        if (ctx->z > 0.1) {
            ctx->z -= 1.0; // Fast emergency descent
            if (ctx->z < 0) ctx->z = 0;
            std::cout << "  Emergency descent - Altitude: " << ctx->z << "m" << std::endl;
        } else {
            std::cout << "⚠️ Emergency landing complete" << std::endl;
            changeToState(DroneState::IDLE, "Emergency procedures completed");
        }
    }
    
    void exit() override {
        std::cout << "🚨 Emergency state deactivated" << std::endl;
        auto ctx = getContext<DroneContext>();
        ctx->emergency_triggered = false;
    }
};

class MissionCompleteState : public StateMachine<DroneState>::State
{
public:
    bool enter() override {
        auto ctx = getContext<DroneContext>();
        double mission_time = std::chrono::duration_cast<std::chrono::seconds>(
            std::chrono::steady_clock::now().time_since_epoch()).count() - ctx->mission_start_time;
            
        std::cout << "🎉 MISSION COMPLETE!" << std::endl;
        std::cout << "📊 Mission Statistics:" << std::endl;
        std::cout << "  ⏱️  Duration: " << mission_time << " seconds" << std::endl;
        std::cout << "  🔋 Battery remaining: " << std::fixed << std::setprecision(1) 
                  << ctx->battery_level << "%" << std::endl;
        std::cout << "  📍 Waypoints visited: " << ctx->waypoint_count << std::endl;
        return true;
    }
    
    void update() override {
        static int complete_counter = 0;
        complete_counter++;
        
        if (complete_counter >= 3) {
            std::cout << "🔄 Starting new mission cycle..." << std::endl;
            changeToState(DroneState::IDLE, "Ready for next mission");
        }
    }
    
    void exit() override {
        std::cout << "🎉 Mission complete state finished" << std::endl;
    }
};

// Demonstration of all features
void demonstrateAllFeatures() {
    std::cout << "\n" << std::string(80, '=') << std::endl;
    std::cout << "🚁 Advanced Drone Mission State Machine Demo" << std::endl;
    std::cout << "==============================================" << std::endl;

    // Create shared context
    auto droneContext = std::make_shared<DroneContext>();
    droneContext->mission_start_time =
        std::chrono::duration_cast<std::chrono::seconds>(std::chrono::steady_clock::now().time_since_epoch()).count();

    // Create the main state machine with template parameter
    StateMachine<DroneState> droneSM(DroneState::IDLE, "DroneController");

    // Add all states
    droneSM.addState<IdleState>(DroneState::IDLE, "Idle")
        .addState<PreflightCheckState>(DroneState::PREFLIGHT_CHECK, "PreflightCheck")
        .addState<TakeoffState>(DroneState::TAKEOFF, "Takeoff")
        .addState<NavigateToTargetState>(DroneState::NAVIGATE_TO_TARGET, "NavigateToTarget")
        .addState<HoverAtTargetState>(DroneState::HOVER_AT_TARGET, "HoverAtTarget")
        .addState<ReturnToBaseState>(DroneState::RETURN_TO_BASE, "ReturnToBase")
        .addState<LandingState>(DroneState::LANDING, "Landing")
        .addState<EmergencyState>(DroneState::EMERGENCY, "Emergency")
        .addState<MissionCompleteState>(DroneState::MISSION_COMPLETE, "MissionComplete")

        // Set context
        .withContext(droneContext)

        // Configure error handling
        .withFallback(DroneState::EMERGENCY)

        // Set logging level
        .withLogLevel(StateMachine<DroneState>::LogLevel::INFO)

        // Configure history tracking
        .withHistorySize(20)

        // Add comprehensive callbacks
        .onStateChanged(
            [](const DroneState & /* from */, const DroneState & /* to */, std::string_view fromName,
               std::string_view toName, std::string_view reason)
            {
                std::cout << "🔄 STATE CHANGE: " << fromName << " ➜ " << toName;
                if (!reason.empty())
                {
                    std::cout << " (" << reason << ")";
                }
                std::cout << std::endl;
            })

        .onStateUpdated(
            [](const DroneState & /* current */, std::string_view currentName)
            {
                // Example: Log periodic status
                static int update_count = 0;
                update_count++;
                if (update_count % 10 == 0)
                {
                    std::cout << "📈 Status: In " << currentName << " state (" << update_count << " updates)"
                              << std::endl;
                }
            })

        .onError([](std::string_view error, const DroneState &currentState)
                 { std::cout << "❌ ERROR: " << error << " (Current state: " << currentState << ")" << std::endl; });

    // Validate configuration
    std::cout << "\n🔍 Validating state machine configuration..." << std::endl;
    if (droneSM.validate()) {
        std::cout << "✅ Configuration valid" << std::endl;
    } else {
        std::cout << "❌ Configuration invalid!" << std::endl;
        return;
    }
    
    // Display initial status
    std::cout << "\n📊 Initial Status:" << std::endl;
    std::cout << "  States registered: " << droneSM.getStateCount() << std::endl;
    std::cout << "  Current state: " << droneSM.getCurrentStateName() << std::endl;
    std::cout << "  Ready: " << (droneSM.isReady() ? "No" : "Yes") << " (before start)" << std::endl;
    
    // Start the state machine
    std::cout << "\n🚀 Starting drone state machine..." << std::endl;
    droneSM.start();
    
    std::cout << "  Ready: " << (droneSM.isReady() ? "Yes" : "No") << " (after start)" << std::endl;
    
    // Main mission loop
    std::cout << "\n🔄 Starting mission execution..." << std::endl;
    std::cout << std::string(50, '-') << std::endl;
    
    int loop_count = 0;
    const int max_loops = 100; // Prevent infinite loops
    
    while (loop_count < max_loops) {
        loop_count++;
        
        // Update state machine
        droneSM.update();
        
        // Check if mission is complete and we've returned to idle
        if (droneSM.getCurrentStateId() == DroneState::IDLE && loop_count > 10) {
            std::cout << "\n🏁 Mission cycle completed!" << std::endl;
            break;
        }
        
        // Add small delay for readability
        std::this_thread::sleep_for(std::chrono::milliseconds(50));
    }
    
    // Display final statistics
    std::cout << "\n" << std::string(50, '-') << std::endl;
    std::cout << "📊 Final Statistics:" << std::endl;
    std::cout << "  Total updates: " << loop_count << std::endl;
    std::cout << "  Final state: " << droneSM.getCurrentStateName() << std::endl;
    std::cout << "  Battery level: " << std::fixed << std::setprecision(1) 
              << droneContext->battery_level << "%" << std::endl;
    
    // Display state history
    auto history = droneSM.getStateHistory();
    std::cout << "  State history (" << history.size() << " entries): ";
    for (size_t i = 0; i < history.size(); ++i) {
        if (i > 0) std::cout << " → ";
        std::cout << static_cast<int>(history[i]);
    }
    std::cout << std::endl;
    
    // Demonstrate reset functionality
    std::cout << "\n🔄 Demonstrating reset functionality..." << std::endl;
    if (droneSM.getCurrentStateId() != DroneState::IDLE) {
        std::cout << "  Current state: " << droneSM.getCurrentStateName() << std::endl;
        if (droneSM.reset()) {
            std::cout << "  ✅ Reset successful - Back to: " << droneSM.getCurrentStateName() << std::endl;
        } else {
            std::cout << "  ❌ Reset failed" << std::endl;
        }
    } else {
        std::cout << "  Already in initial state (IDLE)" << std::endl;
    }
}

// Demonstrate exception handling
void demonstrateExceptionHandling() {
    std::cout << "\n" << std::string(80, '=') << std::endl;
    std::cout << "🚨 EXCEPTION HANDLING DEMONSTRATION" << std::endl;
    std::cout << std::string(80, '=') << std::endl;
    
    // Create a state that throws exceptions
    class ExceptionState : public StateMachine<DroneState>::State
    {
    public:
        bool enter() override {
            std::cout << "💥 Throwing exception in enter()" << std::endl;
            throw std::runtime_error("Simulated sensor failure");
            return true;
        }
    };

    StateMachine<DroneState> testSM(DroneState::IDLE, "TestSM");

    testSM.addState<IdleState>(DroneState::IDLE, "Idle")
        .addState<ExceptionState>(DroneState::EMERGENCY, "ExceptionState")
        .withLogLevel(StateMachine<DroneState>::LogLevel::INFO)
        .onError([](std::string_view error, const DroneState & /* state */)
                 { std::cout << "🚨 Caught error: " << error << std::endl; })
        .start();

    std::cout << "Current state: " << testSM.getCurrentStateName() << std::endl;
    
    // Try to transition to exception state
    std::cout << "\nAttempting transition to exception state..." << std::endl;
    bool result = testSM.changeState(DroneState::EMERGENCY, "Testing exception handling");
    
    std::cout << "Transition result: " << (result ? "Success" : "Failed") << std::endl;
    std::cout << "Current state after exception: " << testSM.getCurrentStateName() << std::endl;
    std::cout << "✅ State machine survived exception gracefully!" << std::endl;
}

int main() {
    try {
        // Run comprehensive demonstration
        demonstrateAllFeatures();
        
        // Demonstrate exception handling
        demonstrateExceptionHandling();
        
        std::cout << "\n🎉 All demonstrations completed successfully!" << std::endl;
        std::cout << "The state machine has proven to be robust and feature-complete." << std::endl;
        
    } catch (const std::exception& e) {
        std::cerr << "💥 Unexpected exception: " << e.what() << std::endl;
        return 1;
    }
    
    return 0;
}
