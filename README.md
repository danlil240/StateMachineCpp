# StateMachine

A mutex-based, flat finite state machine with fluent API, type-safe context, optional hierarchical composition via child state machines, **orthogonal regions**, declarative state timeouts, transition whitelists/guards, and built-in error recovery.

Designed for real-time robotics applications (ROS 2 nodes, flight controllers, mission planners).

---

## Table of Contents

- [Features](#features)
- [Quick Start](#quick-start)
- [Architecture](#architecture)
- [API Reference](#api-reference)
  - [Construction & Configuration](#construction--configuration)
  - [Adding States](#adding-states)
  - [Child State Machines & Orthogonal Regions](#child-state-machines--orthogonal-regions)
  - [Context Objects](#context-objects)
  - [Lifecycle](#lifecycle)
  - [Transitions](#transitions)
  - [Queries](#queries)
  - [Callbacks](#callbacks)
  - [Logging](#logging)
  - [ROS 2 Integration](#ros-2-integration)
  - [Headers](#headers)
- [Writing a State](#writing-a-state)
- [Threading Model](#threading-model)
- [Error Recovery](#error-recovery)
- [Examples](#examples)
  - [Minimal Example](#minimal-example)
  - [With Context and Callbacks](#with-context-and-callbacks)
  - [Hierarchical (Parent + Child SM)](#hierarchical-parent--child-sm)
  - [Orthogonal Regions](#orthogonal-regions)
  - [Real-World: Mission Controller](#real-world-mission-controller)
- [Design Decisions & Caveats](#design-decisions--caveats)

---

## Features

- **Type-safe states** — states are keyed by `enum class`, eliminating stringly-typed bugs
- **Fluent builder API** — method chaining for concise, readable setup
- **Compile-time validation** — `static_assert` ensures state classes inherit from `State`
- **Type-safe context** — shared data between states with runtime type checking; non-throwing `tryGetContext<T>()` for use in `enter()`
- **Hierarchical composition** — attach child state machines to any state; depth is unlimited via recursive composition
- **Orthogonal regions** — a single parent state may own multiple named child SMs that run in parallel within each `update()` cycle
- **Declarative state timeouts** — `withStateTimeout()` auto-transitions after N seconds, replacing hand-rolled timer logic
- **Transition whitelist & guard** — restrict legal transitions declaratively; custom guard predicate for runtime checks
- **Error recovery** — configurable fallback state when `enter()` fails, with rollback to last known-good state
- **Transaction-like entry/exit** — history, timestamps, and sequence numbers are committed atomically per transition; failed transitions are rolled back
- **Configurable transition depth guard** — prevents infinite recursion from re-entrant transitions (default max depth: 4, adjustable)
- **Timestamped transition history** — bounded history with state, timestamp, and reason per entry
- **Injectable clock** — `withClock()` for ROS sim time or test virtual clocks
- **Custom log sink** — `withLogSink()` routes output to any callable (e.g. `rclcpp` logger); built-in `sm_ros::logSink()` adapter
- **Strict config mode** — `withStrictConfig()` rejects configuration changes after `start()` and refuses to start invalid configurations
- **Thread-safe** — all mutable state protected by `recursive_mutex`; `update()` releases the lock during user code
- **Cooperative update cancellation** — `transitionEpoch()`/`stillActive()` let a long-running `update()` detect that it has been superseded and bail out
- **Configurable colored logging** — per-level ANSI terminal output with per-machine color; colors can be toggled independently of the sink
- **Exception-safe** — all user callbacks and state methods are wrapped in try-catch

---

## Quick Start

```cpp
#include "StateMachine/StateMachine.hpp"

// 1. Define your state IDs
enum class MyState { IDLE, RUNNING, DONE };

// 2. Implement states
class IdleState : public StateMachine<MyState>::State {
public:
  bool enter() override {
    std::cout << "Entering IDLE\n";
    return true;
  }
  void update() override {
    // Transition when ready
    changeToState(MyState::RUNNING, "work available");
  }
  void exit() override {
    std::cout << "Leaving IDLE\n";
  }
};

class RunningState : public StateMachine<MyState>::State {
public:
  void update() override {
    // ... do work ...
    changeToState(MyState::DONE, "work complete");
  }
};

class DoneState : public StateMachine<MyState>::State {};

// 3. Build and run
StateMachine<MyState> sm(MyState::IDLE, "MyMachine");

sm.addState<IdleState>(MyState::IDLE, "Idle")
  .addState<RunningState>(MyState::RUNNING, "Running")
  .addState<DoneState>(MyState::DONE, "Done")
  .start();

// 4. Drive from your main loop
while (running) {
  sm.update();
}

// 5. Cleanup
sm.stop();
```

---

## Architecture

```
┌──────────────────────────────────────────────┐
│            StateMachine<StateID>             │
│                                              │
│  ┌─────────┐  ┌─────────┐  ┌────────┐       │
│  │  IDLE   │  │ FLYING  │  │  LAND  │       │  ← Flat states
│  │ (State) │  │ (State) │  │ (State)│       │
│  └─────────┘  └────┬────┘  └────────┘       │
│                     │                        │
│         ┌───────────┴──────────────┐         │
│         │  Orthogonal Regions      │         │  ← Optional composition
│         │  ┌────────┐ ┌────────┐   │         │
│         │  │Motion  │ │Sensors │   │         │
│         │  │TAKEOFF │ │SCAN    │   │         │
│         │  │CRUISE  │ │TRACK   │   │         │
│         │  └────────┘ └────────┘   │         │
│         └──────────────────────────┘         │
└──────────────────────────────────────────────┘
```

- **Flat FSM** with one active state at a time
- **Optional child state machines** attached to specific parent states
- **Orthogonal regions** — a state may own multiple named child SMs that run in parallel within each `update()` cycle
- Regions are started in registration order, stopped in reverse order
- Required regions that fail to start abort the parent state's entry; optional regions log a warning and continue
- Child SMs are automatically started/stopped when their parent state is entered/exited
- Child SMs are updated after the parent state's `update()` in each cycle, in registration order
- Log level, log sink, colors, and clock are automatically propagated from parent to child

---

## API Reference

### Construction & Configuration

```cpp
// Create with initial state, optional name and color
StateMachine<MyState> sm(MyState::IDLE, "MachineName",
                         StateMachine<MyState>::Color::CYAN);

// Fluent configuration
sm.withLogLevel(StateMachine<MyState>::LogLevel::DEBUG)
  .withHistorySize(200)
  .withFallback(MyState::IDLE)
  .withContext(myContextPtr)
  .withClock([this] { return this->now().seconds(); })
  .withLogSink(sm_ros::logSink(get_logger()))
  .withColors(false)
  .withStrictConfig(true)
  .withMaxTransitionDepth(6)
  .withNotifyInitialState(true);
```

| Method | Description |
|--------|-------------|
| `StateMachine(initialState, name, color)` | Constructor. `name` defaults to `"StateMachine"`, `color` to `BLUE` |
| `withLogLevel(level)` | Set log verbosity: `NONE`, `ERROR`, `WARN`, `INFO`, `DEBUG`. Propagated to child SMs. |
| `withLogSink(sink)` | Route log records to a custom sink (e.g. `sm_ros::logSink(logger)`). Pass `{}` to restore stdout. |
| `withColors(bool)` | Enable/disable ANSI color codes on stdout and sink alike. Default: `true`. |
| `withClock(fn)` | Inject a time source (seconds, monotonic). Default: `steady_clock`. Propagated to child SMs. |
| `withHistorySize(n)` | Max states to keep in history (default: 100) |
| `withFallback(stateId)` | Fallback state if `enter()` fails. Rejected after `start()` in strict mode. |
| `withMaxTransitionDepth(n)` | Cap on nested re-entrant transitions (default: 4, clamped to min 1) |
| `withNotifyInitialState(bool)` | Fire `onStateChanged` for the initial state on `start()` (default: `false`) |
| `withStrictConfig(bool)` | Reject config changes after `start()` and refuse to start invalid configurations (default: `false`) |
| `withStateTimeout(id, seconds, target, reason)` | Auto-transition from `id` to `target` after `seconds` elapse |
| `withAllowedTransition(from, to)` | Whitelist a legal transition; declaring any enables enforcement for all states |
| `withTransitionGuard(predicate)` | Custom `(from, to) -> bool` predicate consulted before every transition |
| `withContext(ptr)` | Set the shared context object |
| `setInitialState(stateId)` | Override initial state (must be called **before** `start()`) |

### Adding States

```cpp
// By type (default-constructed)
sm.addState<MyIdleState>(MyState::IDLE, "Idle");

// Pre-built instance (for constructor injection)
auto state = std::make_unique<MyRunningState>(someArg);
sm.addState(MyState::RUNNING, "Running", std::move(state));
```

Both return `StateMachine&` for chaining. Duplicate IDs are silently skipped with a warning log.

### Child State Machines & Orthogonal Regions

A state may own one or more child state machines. When only one is attached it is stored in the **Default** region. Multiple named regions run in parallel within each `update()` cycle.

#### Method 1: `addSubState` (incremental, Default region)

```cpp
sm.addState<FlyingState>(Main::FLYING, "Flying")
  .addSubState<TakeoffState, FlySub>(Main::FLYING, FlySub::TAKEOFF, "Takeoff")
  .addSubState<CruiseState, FlySub>(Main::FLYING, FlySub::CRUISE, "Cruise");
```

The first sub-state added becomes the initial sub-state. Override with `setInitialSubState()`.

#### Method 2: `withSubStates` (lambda, Default region)

```cpp
sm.addState<FlyingState>(Main::FLYING, "Flying")
  .withSubStates<FlySub>(Main::FLYING, FlySub::TAKEOFF,
      [](auto& sub) {
        sub.template addState<TakeoffState>(FlySub::TAKEOFF, "Takeoff")
           .template addState<CruiseState>(FlySub::CRUISE, "Cruise");
      });
```

#### Method 3: `addState` with pre-built child SM

```cpp
auto childSM = std::make_unique<StateMachine<FlySub>>(FlySub::TAKEOFF, "FlySub");
childSM->addState<TakeoffState>(FlySub::TAKEOFF, "Takeoff");

sm.addState(Main::FLYING, "Flying", std::make_unique<FlyingState>(),
            std::move(childSM));
```

#### Method 4: `withRegion` (named orthogonal regions)

```cpp
sm.addState<FlightState>(Main::FLYING, "Flying")
  .withRegion<MotionSub>(Main::FLYING, "Motion", MotionSub::TAKEOFF,
      [](auto& sub) {
        sub.template addState<TakeoffState>(MotionSub::TAKEOFF, "Takeoff")
           .template addState<CruiseState>(MotionSub::CRUISE, "Cruise");
      }, /*required=*/true)
  .withRegion<SensorSub>(Main::FLYING, "Sensors", SensorSub::SCAN,
      [](auto& sub) {
        sub.template addState<ScanState>(SensorSub::SCAN, "Scan")
           .template addState<TrackState>(SensorSub::TRACK, "Track");
      }, /*required=*/false);
```

- Regions are started in registration order and stopped in reverse.
- A **required** region that fails to start aborts the parent state's entire entry (rolling back any already-started regions).
- An **optional** region that fails logs a warning and continues.
- Log level, log sink, colors, and clock are automatically propagated from the parent to every region.

#### Interacting with sub-states and regions from within a State

```cpp
class FlyingState : public StateMachine<Main>::State {
  void update() override {
    // Change sub-state (Default region)
    changeSubState(FlySub::CRUISE, "altitude reached");

    // Change a named region's state
    changeRegionState<SensorSub>("Sensors", SensorSub::TRACK, "target locked");

    // Query Default region
    std::string subName = getActiveSubStateName();
    bool hasSub = hasActiveSubMachine();

    // Query a named region
    std::string regionName = getActiveRegionStateName("Sensors");

    // Direct access to Default region
    auto* child = getSubMachine<FlySub>();

    // Direct access to a named region
    auto* sensorRegion = getRegionMachine<SensorSub>("Sensors");
  }
};
```

### Context Objects

Share data across all states via a type-safe context:

```cpp
struct MissionContext {
  double altitude;
  std::string targetId;
};

auto ctx = std::make_shared<MissionContext>();
sm.withContext(ctx);

// Inside any state:
class TakeoffState : public StateMachine<MyState>::State {
  void update() override {
    auto ctx = getContext<MissionContext>();
    if (ctx->altitude > 100.0) {
      changeToState(MyState::CRUISE, "altitude reached");
    }
  }
};
```

Type mismatches throw `std::runtime_error` at runtime. Use `tryGetContext<T>()` inside `enter()` to avoid exceptions being silently converted to a transition failure.

### Lifecycle

| Method | Description |
|--------|-------------|
| `start()` | Initialize and enter the initial state. Validates configuration in strict mode. Returns `*this` for chaining. |
| `update()` | Drive one cycle: runs active state's `update()`, updates all orthogonal regions in registration order, evaluates state timeouts, then fires the update callback. |
| `stop()` | Exit current state and all regions (bottom-up), reset to initial state, mark uninitialized. Safe to call when already stopped. Rejects re-entrant calls during teardown. |
| `reset()` | Transition back to the initial state, bypassing guards/whitelist. Machine must be running. |
| `reenterState(reason)` | Exit and re-enter the current state (use when you want to restart, not no-op). |

After `stop()`, calling `start()` will re-enter the initial state cleanly.

### Transitions

```cpp
// From outside the state machine
sm.changeState(MyState::RUNNING, "optional reason");

// Atomic compare-and-transition (avoids TOCTOU race)
sm.changeStateIf(MyState::IDLE, MyState::RUNNING, "work available");

// From within a State
changeToState(MyState::RUNNING, "optional reason");

// Change sub-state from outside (Default region)
sm.changeSubState(FlySub::CRUISE, "altitude ok");

// Change a named region's state from outside
sm.changeRegionState<SensorSub>("Sensors", SensorSub::TRACK, "target locked");

// Re-enter the current state
sm.reenterState("retry");
```

- Transitions to the **same state** are silently ignored (returns `true`). Use `reenterState()` to force a re-entry.
- Transitions to **unregistered states** return `false` with an error log.
- If `enter()` fails, the fallback state is tried. If that also fails, `currentStateId` **rolls back** to the last committed state and the error callback fires.
- **Re-entrant transitions** (e.g., `changeState` called from within `enter()`) are supported up to the configured max depth (default: 4), after which the transition is rejected.
- **Transition whitelist**: if any `withAllowedTransition()` is declared, only whitelisted transitions are permitted. Fallback and rollback always bypass the whitelist.
- **Transition guard**: `withTransitionGuard(predicate)` is consulted in addition to the whitelist. Returning `false` rejects the transition. Also bypassed for fallback/rollback.
- **Deferred exit transitions**: when a transition is requested from within `exit()`, it is queued and executed after the current transition chain completes, preventing re-entrant teardown.

### Queries

| Method | Returns |
|--------|---------|
| `getCurrentStateId()` | `StateID` — current state enum value |
| `getCurrentStateName()` | `std::string` — human-readable name |
| `getCurrentState()` | `std::shared_ptr<State>` — shared pointer to active state |
| `hasState(id)` | `bool` — whether a state is registered |
| `getStateCount()` | `size_t` — number of registered states |
| `getStateHistory()` | `std::vector<StateID>` — copy of state IDs, oldest first |
| `getTransitionHistory()` | `std::vector<TransitionRecord>` — full history with timestamps and reasons |
| `isReady()` | `bool` — whether the machine is initialized |
| `validate()` | `bool` — checks initial/current/fallback states, timeout targets, and child SMs recursively |
| `statePath()` | `std::string` — slash-separated path of active state and all active descendant regions |
| `now()` | `double` — current clock reading |
| `stateEntryTime()` | `double` — clock reading when the active state was entered |
| `timeInState()` | `double` — seconds elapsed in the active state |

All query methods are **thread-safe** (acquire the mutex).

### Callbacks

```cpp
sm.onStateChanged([](const MyState& from, const MyState& to,
                     std::string_view fromName, std::string_view toName,
                     std::string_view reason) {
  std::cout << fromName << " -> " << toName << ": " << reason << "\n";
});

sm.onStateUpdated([](const MyState& current, std::string_view name) {
  // Called after each update() cycle
});

sm.onError([](std::string_view error, const MyState& state) {
  std::cerr << "Error in state " << static_cast<int>(state)
            << ": " << error << "\n";
});
```

### Logging

```cpp
sm.withLogLevel(StateMachine<MyState>::LogLevel::DEBUG);

// Route to a custom sink (e.g. ROS logger)
sm.withLogSink(sm_ros::logSink(get_logger()));
sm.withColors(false); // disable ANSI if the sink can't handle them
```

| Level | Output |
|-------|--------|
| `NONE` | Silent |
| `ERROR` | Transition failures, exceptions, invalid configurations |
| `WARN` | Duplicate states, fallback triggers, rejected stops, config changes after start (non-strict) |
| `INFO` | State transitions, start/stop |
| `DEBUG` | State additions, region attachments, region lifecycle |

Output is colored per-machine (constructor `color` parameter) and per-message severity. By default uses ANSI escape codes to `std::cout`; `withLogSink()` redirects to any callable, and `withColors(false)` strips ANSI codes. Both settings are propagated to child SMs.

### ROS 2 Integration

`sm_ros::logSink()` lives in a separate, **opt-in** header. Include it only from translation units that already depend on `rclcpp`:

```cpp
#include "StateMachine/StateMachine.hpp"
#include "StateMachine/StateMachineRos.hpp"   // pulls in rclcpp

// In a ROS 2 node:
sm.withLogSink(sm_ros::logSink(this->get_logger()))
  .withColors(false)   // rosout doesn't render ANSI
  .withClock([this] { return this->now().seconds(); });
```

The adapter maps `LogLevel` to `RCLCPP_ERROR`/`WARN`/`INFO`/`DEBUG` and forwards the fully formatted message. The logger is captured by value, so the sink does not depend on the node's lifetime.

`StateMachine.hpp` itself is `rclcpp`-free, so non-ROS consumers (including this package's own unit tests) never pay for the dependency.

### Headers

| Header | Contents | Pulls in `rclcpp`? |
|--------|----------|--------------------|
| `StateMachine/StateMachine.hpp` | `StateMachine<StateID>` and its nested `State` | No |
| `StateMachine/StateMachineLogging.hpp` | `sm_detail::StateMachineLogging`: `LogLevel`, `Color`, `LogSink`, `ClockFn`, ANSI codes | No |
| `StateMachine/ISubMachine.hpp` | Type-erased child-machine interface | No |
| `StateMachine/StateMachineRos.hpp` | `sm_ros::logSink()` adapter | **Yes** |

`StateMachine.hpp` includes the logging and `ISubMachine` headers itself, so including it alone is enough for everything except the ROS adapter.

---

## Writing a State

Every state must inherit from `StateMachine<YourEnum>::State`:

```cpp
class MyState : public StateMachine<StateID>::State {
public:
  // Called when entering this state.
  // Return false to trigger fallback (or error if no fallback).
  bool enter() override { return true; }

  // Called each update() cycle while this state is active.
  void update() override {}

  // Called when leaving this state.
  void exit() override {}
};
```

### Available from within a State

| Method | Description |
|--------|-------------|
| `changeToState(id, reason)` | Request a transition |
| `getStateMachine()` | Get the owning `StateMachine*` |
| `getContext<T>()` | Get the typed context (throws on type mismatch) |
| `tryGetContext<T>()` | Get the typed context or `nullptr` (no throw — preferred in `enter()`) |
| `timeInState()` | Seconds elapsed since this state was entered |
| `stateEntryTime()` | Clock reading captured at entry |
| `now()` | Current clock reading from the machine's clock source |
| `statePath()` | Slash-separated path of this state and active descendants |
| `stateId()` | The `StateID` this instance was registered under |
| `isCurrentState()` | Whether this state is the machine's active state |
| `transitionEpoch()` | Opaque transition counter, to pair with `stillActive()` (lock-free) |
| `stillActive(epoch)` | Whether no transition has committed since `epoch` (lock-free) |
| `shouldCancelUpdate()` | Whether a transition committed since this `update()` cycle began |
| `changeSubState<ChildID>(id, reason)` | Transition the Default region's child SM |
| `changeRegionState<ChildID>(region, id, reason)` | Transition a named orthogonal region |
| `getActiveSubStateName()` | Name of active sub-state in the Default region |
| `getActiveRegionStateName(region)` | Name of active sub-state in a named region |
| `hasActiveSubMachine()` | Whether current state has an active child SM |
| `getSubMachine<ChildID>()` | Get typed child SM pointer (Default region) |
| `getRegionMachine<ChildID>(region)` | Get typed child SM pointer for a named region |

---

## Threading Model

```
Thread A (main loop)          Thread B (e.g. ROS callback)
─────────────────────         ────────────────────────────
sm.update()                   sm.changeState(NEW, "reason")
  │                             │
  ├─ lock(stateMutex)           ├─ lock(stateMutex)  ← blocks until A releases
  ├─ snapshot current state     │
  ├─ unlock(stateMutex)         │
  │                             │
  ├─ state->update()  ←──── runs concurrently with B's transition
  │                             ├─ exit old state
  │                             ├─ enter new state
  │                             └─ unlock(stateMutex)
  │
  ├─ lock(stateMutex)
  ├─ re-lookup current state  ← sees NEW state
  ├─ update orthogonal regions (in registration order)
  ├─ evaluate state timeout
  ├─ re-snapshot for callback
  ├─ fire update callback
  └─ unlock(stateMutex)
```

**Key points:**
- `update()` releases the lock while running `State::update()`, so external `changeState()` calls aren't blocked by long-running update logic.
- This means `exit()` can be called on a state whose `update()` is still executing. **States that may be updated and exited concurrently must handle their own internal synchronization** (e.g., `std::atomic` flags).
- A long-running `update()` should **poll for cancellation** so it doesn't act on a state the machine has already left:

```cpp
void update() override {
  const auto epoch = transitionEpoch();   // lock-free
  for (const auto& waypoint : waypoints_) {
    if (!stillActive(epoch)) return;      // superseded — drop the remaining work
    process(waypoint);
  }
}
```

  `stillActive(epoch)` is strictly stronger than `isCurrentState()`: it also catches `A -> B -> A` round trips (where the machine is in `A` again but a fresh `enter()` has run), explicit `reenterState()`, and `stop()`. `shouldCancelUpdate()` is the zero-bookkeeping equivalent, comparing against the epoch the machine captured when it dispatched the current `update()`.
- `isCurrentState()` returns `true` inside `enter()`: arrival is committed *before* `enter()` runs.
- `changeState()` from within `State::update()` is safe — it acquires the lock independently.
- The `recursive_mutex` allows re-entrant transitions (e.g., `enter()` calling `changeToState()`).
- Transitions requested from within `exit()` are deferred until the current transition chain completes, preventing re-entrant teardown.
- `stop()` is rejected if called re-entrantly during state or region teardown.

---

## Error Recovery

### Fallback State

```cpp
sm.withFallback(MyState::SAFE_MODE);
```

If any state's `enter()` returns `false` or throws, the machine attempts to transition to the fallback state. If the fallback itself fails, `currentStateId` is rolled back to the **last committed state** (tracked separately from history) and the error callback fires. The rollback target is re-entered to keep the state object's view of itself consistent.

### Transaction-like Entry/Exit

Each transition commits the new state ID, entry timestamp, and history record atomically. If the transition fails (enter returns false, required region fails to start), all side effects are rolled back:
- History record is removed
- Timestamp is restored
- Already-started regions are stopped in reverse order
- The state's `exit()` is called via `guardedExit()`

This ensures the machine and its history stay in a consistent state even after failed transitions.

### Transition Depth Guard

Re-entrant transitions (transitions triggered from within `enter()` or callbacks) are capped at the configured max depth (default: **4**, adjustable via `withMaxTransitionDepth()`). Beyond that, the transition is rejected and the error callback fires. This prevents infinite loops such as:

```
enter(A) → changeState(B) → enter(B) fails → fallback(C) → enter(C) → changeState(A) → ...
```

### Deferred Exit Transitions

A transition requested from within `exit()` is deferred and executed after the current transition chain completes. This prevents re-entrant teardown and ensures `exit()` runs exactly once per state departure.

### Exception Handling

All user-facing methods (`enter()`, `update()`, `exit()`, callbacks) are wrapped in `try-catch`. Exceptions are logged but do not crash the state machine.

---

## Examples

### Minimal Example

```cpp
enum class Light { RED, GREEN, YELLOW };

class RedState : public StateMachine<Light>::State {
  void update() override { changeToState(Light::GREEN, "timer"); }
};
class GreenState : public StateMachine<Light>::State {
  void update() override { changeToState(Light::YELLOW, "timer"); }
};
class YellowState : public StateMachine<Light>::State {
  void update() override { changeToState(Light::RED, "timer"); }
};

StateMachine<Light> sm(Light::RED, "TrafficLight");
sm.addState<RedState>(Light::RED, "Red")
  .addState<GreenState>(Light::GREEN, "Green")
  .addState<YellowState>(Light::YELLOW, "Yellow")
  .start();
```

### With Context and Callbacks

```cpp
struct AppContext {
  int counter = 0;
};

auto ctx = std::make_shared<AppContext>();

StateMachine<MyState> sm(MyState::IDLE, "App");
sm.withContext(ctx)
  .withFallback(MyState::IDLE)
  .withLogLevel(StateMachine<MyState>::LogLevel::DEBUG)
  .onStateChanged([](auto& from, auto& to, auto fromN, auto toN, auto reason) {
    std::cout << fromN << " -> " << toN << "\n";
  })
  .onError([](auto error, auto state) {
    std::cerr << "ERROR: " << error << "\n";
  })
  .addState<IdleState>(MyState::IDLE, "Idle")
  .addState<WorkState>(MyState::WORK, "Work")
  .start();
```

### Hierarchical (Parent + Child SM)

```cpp
enum class Main { GROUND, FLYING, LANDED };
enum class FlySub { TAKEOFF, CRUISE, DESCEND };

// Parent states
class GroundState : public StateMachine<Main>::State { /* ... */ };
class FlyingState : public StateMachine<Main>::State {
  void update() override {
    // The child SM is automatically updated after this
    if (hasActiveSubMachine()) {
      auto subName = getActiveSubStateName();
      // React to sub-state if needed
    }
  }
};
class LandedState : public StateMachine<Main>::State { /* ... */ };

// Sub-states
class TakeoffSub : public StateMachine<FlySub>::State {
  void update() override {
    // When done, transition within the child SM
    changeToState(FlySub::CRUISE, "altitude reached");
  }
};
class CruiseSub : public StateMachine<FlySub>::State { /* ... */ };
class DescendSub : public StateMachine<FlySub>::State { /* ... */ };

// Build
StateMachine<Main> sm(Main::GROUND, "FlightSM");
sm.addState<GroundState>(Main::GROUND, "Ground")
  .addState<FlyingState>(Main::FLYING, "Flying")
  .addSubState<TakeoffSub, FlySub>(Main::FLYING, FlySub::TAKEOFF, "Takeoff")
  .addSubState<CruiseSub, FlySub>(Main::FLYING, FlySub::CRUISE, "Cruise")
  .addSubState<DescendSub, FlySub>(Main::FLYING, FlySub::DESCEND, "Descend")
  .addState<LandedState>(Main::LANDED, "Landed")
  .withFallback(Main::GROUND)
  .start();
```

### Orthogonal Regions

```cpp
enum class Main { GROUND, FLYING, LANDED };
enum class MotionSub { TAKEOFF, CRUISE, DESCEND };
enum class SensorSub { SCAN, TRACK, IDLE };

class FlyingState : public StateMachine<Main>::State {
  void update() override {
    // Both regions are updated automatically after this update()
    // Read what each region is doing
    auto motion = getActiveRegionStateName("Motion");
    auto sensor = getActiveRegionStateName("Sensors");
  }
};

StateMachine<Main> sm(Main::GROUND, "FlightSM");
sm.addState<GroundState>(Main::GROUND, "Ground")
  .addState<FlyingState>(Main::FLYING, "Flying")
  // Required motion region
  .withRegion<MotionSub>(Main::FLYING, "Motion", MotionSub::TAKEOFF,
      [](auto& sub) {
        sub.template addState<TakeoffSub>(MotionSub::TAKEOFF, "Takeoff")
           .template addState<CruiseSub>(MotionSub::CRUISE, "Cruise")
           .template addState<DescendSub>(MotionSub::DESCEND, "Descend");
      }, /*required=*/true)
  // Optional sensor region
  .withRegion<SensorSub>(Main::FLYING, "Sensors", SensorSub::SCAN,
      [](auto& sub) {
        sub.template addState<ScanSub>(SensorSub::SCAN, "Scan")
           .template addState<TrackSub>(SensorSub::TRACK, "Track")
           .template addState<IdleSub>(SensorSub::IDLE, "Idle");
      }, /*required=*/false)
  .addState<LandedState>(Main::LANDED, "Landed")
  .withStateTimeout(Main::FLYING, 600.0, Main::LANDED, "max flight time")
  .start();
```

### Real-World: Mission Controller

```cpp
// From MissionController::setupStateMachine()
state_machine_ = std::make_unique<StateMachine<MissionStateID>>(
    MissionStateID::IDLE, "MC_STATE_MACHINE",
    StateMachine<MissionStateID>::Color::BLUE);

auto mc_ptr = std::dynamic_pointer_cast<MissionController>(shared_from_this());
state_machine_->withContext(mc_ptr)
    .withLogLevel(StateMachine<MissionStateID>::LogLevel::INFO);

state_machine_->addState<IdleState>(MissionStateID::IDLE, "Idle")
    .addState<TakeoffState>(MissionStateID::TAKEOFF, "Takeoff")
    .addState<TransitionFWState>(MissionStateID::TRANSITION_FW, "Transition FW")
    .addState<FWCruiseState>(MissionStateID::FW_CRUISE, "FW Cruise")
    .addState<TransitionMCState>(MissionStateID::TRANSITION_MC, "Transition MC")
    .addState<LandState>(MissionStateID::LAND, "Land");

state_machine_->start();

// In the 50Hz main loop:
void MissionController::main_loop() {
  if (state_machine_) {
    state_machine_->update();
  }
}
```

---

## Design Decisions & Caveats

| Decision | Rationale |
|----------|-----------|
| **Flat FSM, not true HSM** | Simpler implementation; child SMs are composed via ownership, not ancestor-aware transitions. Orthogonal regions cover parallel use-cases without full HSM complexity. |
| **`recursive_mutex`** | Required to support re-entrant transitions (e.g., `enter()` calling `changeToState()`, fallback chains). |
| **Lock released during `State::update()`** | Allows external `changeState()` to proceed without waiting for a potentially long update cycle. Trade-off: `exit()` and `update()` can run concurrently on the same state. |
| **`shared_ptr<State>` snapshot in `update()`** | Prevents use-after-free if the state is replaced while `update()` is running. |
| **Transition whitelist is all-or-nothing** | Declaring even one `withAllowedTransition()` enables enforcement for every state. This avoids silent gaps where some transitions are checked and others aren't. Fallback and rollback always bypass the whitelist so recovery cannot be locked out. |
| **Deferred exit transitions** | A transition requested from within `exit()` is queued and run after the current chain completes, preventing re-entrant teardown. |
| **No state removal** | States are added once and live for the lifetime of the machine. This simplifies memory safety. |
| **Copy disabled, move disabled** | `recursive_mutex` and `atomic` are not movable. State machines should be heap-allocated (`unique_ptr`) if ownership transfer is needed. |
| **Config changes after `start()`** | By default, post-start config changes log a warning and are ignored. `withStrictConfig(true)` upgrades this to an error and also refuses to start an invalid configuration. |
| **`std::cout` default logging** | Lightweight, no external dependencies. Use `withLogSink(sm_ros::logSink(logger))` to route to `rclcpp`, or any callable for custom destinations. |
