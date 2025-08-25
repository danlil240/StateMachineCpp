# StateMachine Library

**A high-performance, thread-safe state machine implementation for C++17+**

## Features

- ✅ **Type-safe states** via enum class
- ✅ **Thread-safe operations** with minimal locking
- ✅ **Fluent API** with method chaining
- ✅ **Customizable state transitions** with callbacks
- ✅ **Type-safe context objects** with validation
- ✅ **Error recovery** with fallback states
- ✅ **State history tracking**
- ✅ **Configurable logging** with ANSI colors
- ✅ **Exception handling** for robust operation
- ✅ **Header-only template** library

## Quick Start

```cpp
#include "StateMachine.hpp"

enum class MyState { IDLE, WORKING, FINISHED };

class IdleState : public StateMachine<MyState>::State {
public:
    bool enter() override {
        std::cout << "Entering idle state" << std::endl;
        return true;
    }
    void update() override {
        // Transition after some condition
        changeToState(MyState::WORKING, "Starting work");
    }
    void exit() override {
        std::cout << "Leaving idle state" << std::endl;
    }
};

int main() {
    StateMachine<MyState> sm(MyState::IDLE, "MyMachine");
    
    sm.addState<IdleState>(MyState::IDLE, "Idle")
      .start();
      
    // Main loop
    for (int i = 0; i < 10; ++i) {
        sm.update();
    }
    
    return 0;
}
```

## Core Components

### StateMachine Template Class

The main template class `StateMachine<StateID>` where `StateID` must be a trivially copyable type (typically an enum class).

### State Base Class

All states must inherit from `StateMachine<StateID>::State`:

```cpp
class MyState : public StateMachine<MyStateEnum>::State {
public:
    bool enter() override;  // Called when entering state
    void update() override; // Called each update cycle
    void exit() override;   // Called when leaving state
};
```

## Complete API Reference

### Constructor

```cpp
StateMachine(const StateID& initialState, std::string name = "StateMachine")
```

### State Management

```cpp
// Add states to the machine
template<typename StateType>
StateMachine& addState(const StateID& id, std::string_view name);

// Add pre-configured state instance
StateMachine& addState(const StateID& id, std::string_view name, 
                      std::unique_ptr<State> state);

// Check if state exists
bool hasState(const StateID& id) const;

// Get number of registered states
size_t getStateCount() const;
```

### Lifecycle

```cpp
// Initialize and start the state machine
StateMachine& start();

// Check if initialized
bool isReady() const;

// Update the state machine (call in main loop)
void update();

// Change state manually
bool changeState(const StateID& newStateId, std::string_view reason = "");

// Reset to initial state
bool reset();
```

### State Information

```cpp
// Get current state
StateID getCurrentStateId() const;
std::string getCurrentStateName() const;
State* getCurrentState() const;

// Get state history
std::vector<StateID> getStateHistory() const;
```

### Context Management

Type-safe context objects shared between states:

```cpp
// Set context (fluent API)
template<typename T>
StateMachine& withContext(std::shared_ptr<T> context);

// Get context from state machine or state
template<typename T>
std::shared_ptr<T> getContext() const;
```

**Example:**
```cpp
struct RobotContext {
    double x, y, theta;
    bool sensors_active;
};

auto context = std::make_shared<RobotContext>();
sm.withContext(context);

// In state methods:
auto ctx = getContext<RobotContext>();
ctx->x = 1.5;
```

### Callbacks

Register callbacks for state machine events:

```cpp
// State change callback
StateMachine& onStateChanged(StateChangeCallback callback);

// State update callback  
StateMachine& onStateUpdated(StateUpdateCallback callback);

// Error callback
StateMachine& onError(ErrorCallback callback);
```

**Callback signatures:**
```cpp
using StateChangeCallback = std::function<void(
    const StateID& from, const StateID& to,
    std::string_view fromName, std::string_view toName,
    std::string_view reason)>;

using StateUpdateCallback = std::function<void(
    const StateID& current, std::string_view currentName)>;

using ErrorCallback = std::function<void(
    std::string_view error, const StateID& currentState)>;
```

### Configuration

```cpp
// Set fallback state for error recovery
StateMachine& withFallback(const StateID& fallbackState);

// Set logging level
StateMachine& withLogLevel(LogLevel level);

// Set maximum history size
StateMachine& withHistorySize(size_t size);
```

**Logging levels:**
- `LogLevel::NONE` - No logging
- `LogLevel::ERROR` - Only errors
- `LogLevel::WARN` - Warnings and errors
- `LogLevel::INFO` - Info, warnings, and errors (default)
- `LogLevel::DEBUG` - All logging including debug info

### Validation

```cpp
// Validate state machine configuration
bool validate() const;
```

## State Methods

### State Transitions

From within a state, request transitions using:

```cpp
void changeToState(const StateID& newStateId, std::string_view reason = "");
```

### Context Access

Access shared context from any state:

```cpp
template<typename T>
std::shared_ptr<T> getContext() const;
```

### State Machine Access

Get reference to the parent state machine:

```cpp
StateMachine* getStateMachine() const;
```

## Advanced Usage Examples

### Robot Navigation State Machine

```cpp
enum class RobotState {
    IDLE, NAVIGATING, OBSTACLE_AVOIDANCE, GOAL_REACHED, ERROR
};

struct RobotContext {
    geometry_msgs::Point target;
    sensor_msgs::LaserScan scan;
    bool goal_reached = false;
    bool obstacle_detected = false;
};

class NavigatingState : public StateMachine<RobotState>::State {
public:
    bool enter() override {
        auto ctx = getContext<RobotContext>();
        std::cout << "Starting navigation to (" 
                  << ctx->target.x << ", " << ctx->target.y << ")" << std::endl;
        return true;
    }

    void update() override {
        auto ctx = getContext<RobotContext>();
        
        if (ctx->obstacle_detected) {
            changeToState(RobotState::OBSTACLE_AVOIDANCE, "Obstacle detected");
        } else if (ctx->goal_reached) {
            changeToState(RobotState::GOAL_REACHED, "Reached target");
        }
        
        // Continue navigation logic...
    }
};
```

### Error Handling and Fallback

```cpp
StateMachine<RobotState> robotSM(RobotState::IDLE, "RobotController");

robotSM
    .addState<IdleState>(RobotState::IDLE, "Idle")
    .addState<NavigatingState>(RobotState::NAVIGATING, "Navigating")
    .addState<ErrorState>(RobotState::ERROR, "Error")
    .withFallback(RobotState::ERROR) // Fallback on transition failures
    .withContext(context)
    .onError([](std::string_view error, const RobotState& state) {
        std::cerr << "State machine error: " << error << std::endl;
    })
    .start();
```

### Comprehensive Callback Usage

```cpp
sm.onStateChanged([](const RobotState& from, const RobotState& to,
                     std::string_view fromName, std::string_view toName,
                     std::string_view reason) {
    std::cout << "Transition: " << fromName << " -> " << toName 
              << " (" << reason << ")" << std::endl;
})
.onStateUpdated([](const RobotState& current, std::string_view name) {
    // Called after each update
    static int count = 0;
    if (++count % 100 == 0) {
        std::cout << "State " << name << " updated " << count << " times" << std::endl;
    }
})
.onError([](std::string_view error, const RobotState& state) {
    std::cerr << "Error in state machine: " << error << std::endl;
});
```

## Thread Safety

The state machine is designed for thread-safe operation:

- **Atomic operations** for state tracking
- **Mutex protection** for critical sections
- **Minimal locking** to avoid performance issues

**Recommended usage:**
- Call `update()` from a single thread (main control loop)
- `changeState()` can be called from multiple threads safely
- Context objects should handle their own thread safety if accessed concurrently

## Error Handling

The state machine gracefully handles various error conditions:

- **State enter() failures**: Trigger fallback state if configured
- **Exceptions in state methods**: Caught and logged, state machine continues
- **Invalid transitions**: Rejected safely, current state maintained
- **Missing states**: Detected during validation

## Best Practices

1. **Use enum class for StateID** - Provides type safety
2. **Keep state methods lightweight** - Avoid blocking operations
3. **Use context for shared data** - Better than static variables
4. **Implement proper enter/exit** - Initialize/cleanup resources
5. **Handle edge cases in update()** - Check conditions before transitions
6. **Use callbacks for logging** - Monitor state machine behavior
7. **Validate configuration** - Call `validate()` after setup
8. **Set appropriate log level** - DEBUG for development, ERROR for production

## Integration with ROS2

```cpp
#include <rclcpp/rclcpp.hpp>

class ROS2StateMachineNode : public rclcpp::Node {
private:
    StateMachine<RobotState> sm_;
    rclcpp::TimerInterface::SharedPtr timer_;
    
public:
    ROS2StateMachineNode() : Node("state_machine_node"), sm_(RobotState::IDLE, "RobotSM") {
        setupStateMachine();
        
        timer_ = create_wall_timer(
            std::chrono::milliseconds(50),
            [this]() { sm_.update(); }
        );
    }
    
private:
    void setupStateMachine() {
        auto context = std::make_shared<RobotContext>();
        
        sm_.addState<IdleState>(RobotState::IDLE, "Idle")
           .addState<NavigatingState>(RobotState::NAVIGATING, "Navigating")
           .withContext(context)
           .withLogLevel(StateMachine<RobotState>::LogLevel::INFO)
           .onStateChanged([this](const auto& from, const auto& to, 
                                  auto fromName, auto toName, auto reason) {
               RCLCPP_INFO(get_logger(), "State: %s -> %s (%s)", 
                          std::string(fromName).c_str(), 
                          std::string(toName).c_str(),
                          std::string(reason).c_str());
           })
           .start();
    }
};
```

## Building

### CMake Integration

```cmake
# As a subdirectory
add_subdirectory(StateMachine)
target_link_libraries(your_target PRIVATE state_machine_lib)

# Or include directly
target_include_directories(your_target PRIVATE path/to/StateMachine)
```

### Standalone Build

```bash
mkdir build && cd build
cmake ..
make
./state_machine_test  # Run comprehensive tests
```

## Requirements

- **C++17 or later**
- **CMake 3.16+** (for standalone builds)
- **Threading support** (pthread on Linux)

## Testing

The library includes a comprehensive test suite (`test.cpp`) covering:

- Basic functionality and transitions
- Context management and type safety
- Callback functionality
- Illegal operations and error handling  
- Exception handling and recovery
- Fallback mechanisms
- Same-state transitions
- History tracking
- State machine validation
- Automatic transitions
- Basic thread safety
- Reset functionality

Run tests with:
```bash
make test
# or
./state_machine_test
```

## License

This library is part of the ROS Common2 package for drone operations.

## Contributing

When contributing:
1. Maintain thread safety
2. Add comprehensive tests for new features
3. Update this documentation
4. Follow existing code style
5. Ensure all tests pass

---

*For more examples, see `example.cpp` in this directory.*
