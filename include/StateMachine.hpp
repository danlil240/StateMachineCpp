#ifndef ROS_COMMON2_STATE_MACHINE_HPP
#define ROS_COMMON2_STATE_MACHINE_HPP

#include "ISubMachine.hpp"
#include "StateMachineLogging.hpp"

#include <algorithm>
#include <atomic>
#include <cstdint>
#include <deque>
#include <functional>
#include <iostream>
#include <memory>
#include <mutex>
#include <optional>
#include <stdexcept>
#include <string>
#include <string_view>
#include <thread>
#include <type_traits>
#include <typeindex>
#include <typeinfo>
#include <unordered_map>
#include <unordered_set>
#include <utility>
#include <vector>

/**
 * @brief Mutex-based flat finite state machine with fluent API
 *
 * Architecture:
 * - Flat FSM with one active state and one transition primitive
 * - Arbitrarily deep child state machines via composition
 * - Optional orthogonal regions: one parent state may own several child
 *   machines that are active and updated together in deterministic order
 * - This is not a full UML HSM: there are no ancestor-aware transitions or
 *   automatic event bubbling
 * - All mutable state is protected by a single recursive_mutex
 *
 * Features:
 * - Type-safe states via enum class
 * - Fluent API with method chaining
 * - Customizable state transitions with callbacks
 * - Type-safe context objects with validation
 * - Error recovery with fallback states
 * - Timestamped state history with transition reasons
 * - Optional allowed-transition whitelist / guard predicate
 * - Per-state timeouts and built-in time-in-state tracking
 * - Configurable logging via an injectable sink
 *
 * Threading model:
 * - All mutable state is guarded by stateMutex (recursive_mutex)
 * - update() releases the lock while calling State::update(), the child
 *   sub-machine's update() and the state-update callback, so external
 *   changeState() calls are never blocked by long-running update logic.
 *   States that may be updated and exited concurrently should handle their
 *   own internal synchronization.
 * - changeStateInternal() holds the lock across exit()/enter() so a transition
 *   is atomic with respect to other threads. Observer callbacks fired from a
 *   transition therefore run under the lock and may call changeState()
 *   re-entrantly (same thread, recursive mutex).
 * - Transitions requested from exit() are rejected. An exit hook is teardown,
 *   not a second transition decision point; allowing it to supersede the
 *   transition being executed would leave partially entered states behind.
 *
 * Transition commit model:
 * - currentStateId, the entry timestamp and the history record are committed
 *   before enter() runs, so re-entrant transitions from within enter() observe
 *   a consistent machine and history stays in chronological order.
 * - Every commit bumps transitionSeq. After enter() returns, a changed
 *   transitionSeq means a nested transition already superseded this one, so the
 *   outer call skips starting the child sub-machine and skips its observer
 *   callback. A state that is entered and immediately left from within enter()
 *   is therefore reported to observers as the `from` of the resulting
 *   transition rather than as a separate arrival.
 * - If enter() fails, the machine tries the fallback state; if that is
 *   unusable it rolls back to (and re-enters) the last successfully entered
 *   state, so currentStateId never names a state whose enter() failed.
 */
template <typename StateID>
class StateMachine : public sm_detail::StateMachineLogging {
  // States are stored in an unordered_map keyed by StateID and are default-
  // constructed in a few places (rollback targets, empty region lookups).
  // Asserting the requirements here turns an unreadable instantiation error
  // deep inside libstdc++ into a one-line diagnostic.
  static_assert(std::is_default_constructible_v<StateID>,
                "StateID must be default constructible");
  static_assert(std::is_copy_constructible_v<StateID> &&
                    std::is_copy_assignable_v<StateID>,
                "StateID must be copyable");
  static_assert(
      std::is_invocable_r_v<std::size_t, std::hash<StateID>, const StateID &>,
      "StateID must be hashable: provide a std::hash<StateID> specialization "
      "(enum class and integral types work out of the box)");
  static_assert(std::is_invocable_r_v<bool, std::equal_to<StateID>,
                                      const StateID &, const StateID &>,
                "StateID must be equality comparable");

public:
  /**
   * @brief Base state class that all states must inherit from
   */
  class State {
  public:
    virtual ~State() = default;

    /**
     * @brief Called when entering this state
     * @return true if enter succeeded, false to trigger fallback
     */
    virtual bool enter() { return true; }

    /**
     * @brief Called each update cycle when this state is active
     */
    virtual void update() {}

    /**
     * @brief Called when exiting this state
     */
    virtual void exit() {}

    /**
     * @brief Request a state transition from within the state
     * @param newStateId The state ID to transition to
     * @param reason Optional reason for the transition
     */
    void changeToState(const StateID &newStateId, std::string reason = "") {
      if (stateMachine) {
        stateMachine->changeState(newStateId, std::move(reason));
      }
    }

    /**
     * @brief Get the state machine this state belongs to
     */
    StateMachine *getStateMachine() const { return stateMachine; }

    /**
     * @brief Get the context object with type safety
     * @tparam T The type to cast the context to
     * @return std::shared_ptr<T> Shared pointer to the context object
     * @throws std::runtime_error if context type doesn't match
     */
    template <typename T> std::shared_ptr<T> getContext() const {
      if (!stateMachine) {
        throw std::runtime_error("State not associated with a state machine");
      }
      return stateMachine->template getContext<T>();
    }

    /**
     * @brief Get the context object, or nullptr if unset or of another type
     *
     * Preferred over getContext() inside enter(), where a thrown exception is
     * silently converted into a transition failure.
     */
    template <typename T> std::shared_ptr<T> tryGetContext() const {
      return stateMachine ? stateMachine->template tryGetContext<T>() : nullptr;
    }

    /**
     * @brief Seconds elapsed since this state was entered
     *
     * Use instead of hand-rolling an entry timestamp in enter().
     * @code  if (timeInState() > kTimeoutSec) { ... }  @endcode
     */
    double timeInState() const {
      return stateMachine ? stateMachine->timeInState() : 0.0;
    }

    /**
     * @brief Clock reading captured when this state was entered
     */
    double stateEntryTime() const {
      return stateMachine ? stateMachine->stateEntryTime() : 0.0;
    }

    /**
     * @brief Current clock reading from the machine's clock source
     */
    double now() const { return stateMachine ? stateMachine->now() : 0.0; }

    // ── Liveness / cooperative update cancellation ──────────────────
    // update() runs without the machine lock, so another thread can transition
    // away while a long update() is still executing. These let update() notice
    // and bail out instead of acting on a state it has already left.

    /**
     * @brief The state ID this instance was registered under
     */
    const StateID &stateId() const { return myStateId; }

    /**
     * @brief Whether this state is still the machine's active state
     *
     * Cheap identity check. Note it cannot see an A->B->A round trip: after
     * one, the machine is in this state again but a fresh enter() has run, so
     * anything captured by the in-flight update() is stale. Use
     * transitionEpoch()/stillActive() when that distinction matters.
     *
     * Returns true inside enter(): arrival is committed before enter() runs.
     */
    bool isCurrentState() const {
      return stateMachine && stateMachine->getCurrentStateId() == myStateId;
    }

    /**
     * @brief Transition counter to pair with stillActive()
     *
     * Lock-free. Capture once at the top of a long operation.
     */
    std::uint64_t transitionEpoch() const {
      return stateMachine ? stateMachine->transitionEpoch() : 0;
    }

    /**
     * @brief Whether no transition has committed since `epoch` was captured
     *
     * Lock-free, and strictly stronger than isCurrentState(): it also detects
     * A->B->A round trips, state re-entry and stop().
     *
     * @code
     *   void update() override {
     *     const auto epoch = transitionEpoch();
     *     for (const auto &wp : waypoints_) {
     *       if (!stillActive(epoch)) return;  // superseded, drop the work
     *       process(wp);
     *     }
     *   }
     * @endcode
     */
    bool stillActive(std::uint64_t epoch) const {
      return stateMachine && stateMachine->stillActive(epoch);
    }

    /**
     * @brief Whether a transition committed since this update() cycle began
     *
     * Zero-bookkeeping alternative to transitionEpoch()/stillActive(), using
     * the epoch the machine captured when it dispatched this update(). Only
     * meaningful while called (directly or indirectly) from update().
     */
    bool shouldCancelUpdate() const {
      return stateMachine ? stateMachine->isUpdateCancellationRequested()
                          : false;
    }

    /**
     * @brief Slash-separated path of this state and any active descendants
     */
    std::string statePath() const {
      return stateMachine ? stateMachine->statePath() : "";
    }

    // ── Sub-state convenience methods ──────────────────────────────
    // These let any state interact with sub-states via sm_ directly.

    /**
     * @brief Change the active sub-state of the current child state machine
     * @code  changeSubState(FlySub::CRUISE, "altitude ok");  @endcode
     */
    template <typename ChildStateID>
    bool changeSubState(const ChildStateID &childState,
                        std::string reason = "") {
      if (stateMachine) {
        return stateMachine->template changeSubState<ChildStateID>(
            childState, std::move(reason));
      }
      return false;
    }

    /**
     * @brief Get the name of the currently active sub-state (or "")
     */
    std::string getActiveSubStateName() const {
      return stateMachine ? stateMachine->getActiveSubStateName() : "";
    }

    /**
     * @brief Check if the current state has an active child state machine
     */
    bool hasActiveSubMachine() const {
      return stateMachine ? stateMachine->hasActiveSubMachine() : false;
    }

    /**
     * @brief Get the typed child state machine for the current state
     * @code  auto* sub = getSubMachine<FlySub>();  @endcode
     */
    template <typename ChildStateID>
    StateMachine<ChildStateID> *getSubMachine() const {
      return stateMachine
                 ? stateMachine->template getTypedSubMachine<ChildStateID>()
                 : nullptr;
    }

    /**
     * @brief Change one named orthogonal region of this state
     */
    template <typename ChildStateID>
    bool changeRegionState(std::string_view regionName,
                           const ChildStateID &childState,
                           std::string reason = "") {
      return stateMachine
                 ? stateMachine->template changeRegionState<ChildStateID>(
                       regionName, childState, std::move(reason))
                 : false;
    }

    /**
     * @brief Get the active state name of a named orthogonal region
     */
    std::string
    getActiveRegionStateName(std::string_view regionName) const {
      return stateMachine
                 ? stateMachine->getActiveRegionStateName(regionName)
                 : "";
    }

    /**
     * @brief Get a typed named orthogonal region machine
     */
    template <typename ChildStateID>
    StateMachine<ChildStateID> *
    getRegionMachine(std::string_view regionName) const {
      return stateMachine
                 ? stateMachine->template getRegionMachine<ChildStateID>(
                       regionName)
                 : nullptr;
    }

    // ── Logging & conditional transition helpers ───────────────────

    /**
     * @brief Log a message through the owning machine's logging infrastructure
     *
     * The message respects withLogLevel() and withLogSink(), and is formatted
     * with the machine's name and color. Use this instead of std::cout so state
     * output is consistent with machine logging.
     *
     * @code  log(LogLevel::INFO, "Engine started");  @endcode
     */
    void log(LogLevel level, std::string_view message) const {
      if (stateMachine) {
        stateMachine->log(level, message);
      }
    }


  private:
    friend class StateMachine;
    StateMachine *stateMachine = nullptr;
    // The ID this instance was registered under. Assigned once by
    // registerState() and never mutated afterwards, so it needs no locking.
    StateID myStateId{};
  };

  // Callback types for external observers
  using StateChangeCallback = std::function<void(
      const StateID &from, const StateID &to, std::string_view fromName,
      std::string_view toName, std::string_view reason)>;
  using StateUpdateCallback =
      std::function<void(const StateID &current, std::string_view currentName)>;
  using ErrorCallback =
      std::function<void(std::string_view error, const StateID &currentState)>;

  /**
   * @brief One entry of the transition history
   */
  struct TransitionRecord {
    StateID state{};        // state that was entered
    double timestamp{0.0};  // machine clock reading at entry
    std::string reason;     // reason supplied to changeState()
  };

  /**
   * @brief Declarative per-state timeout
   */
  struct StateTimeout {
    double seconds{0.0};
    StateID target{};
    std::string reason;
  };

  /**
   * @brief Concrete ISubMachine wrapping a StateMachine<ChildStateID>
   */
  template <typename ChildStateID> class SubMachineImpl : public ISubMachine {
  public:
    explicit SubMachineImpl(std::unique_ptr<StateMachine<ChildStateID>> sm)
        : machine_(std::move(sm)) {
      if (!machine_) {
        throw std::invalid_argument("SubMachineImpl requires a child machine");
      }
    }

    bool start() override {
      machine_->start();
      return machine_->isReady();
    }

    void update() override { machine_->update(); }

    void stop() override { machine_->stop(); }

    std::string activeStateName() const override {
      return machine_->getCurrentStateName();
    }

    bool isActive() const override { return machine_->isReady(); }

    std::string statePath() const override { return machine_->statePath(); }

    bool validate() const override { return machine_->validate(); }

    void applyLogConfig(LogLevel level, LogSink sink,
                        bool useColors) override {
      machine_->withLogLevel(level);
      machine_->withLogSink(std::move(sink));
      machine_->withColors(useColors);
    }

    void applyClock(ClockFn clock) override {
      machine_->withClock(std::move(clock));
    }

    StateMachine<ChildStateID> &machine() { return *machine_; }
    const StateMachine<ChildStateID> &machine() const { return *machine_; }

  private:
    std::unique_ptr<StateMachine<ChildStateID>> machine_;
  };

private:
  static constexpr std::string_view DEFAULT_REGION_NAME = "Default";

  /**
   * @brief One orthogonal region owned by a state
   *
   * Regions are kept in registration order. This makes start/update order
   * deterministic and stop order the exact reverse.
   */
  struct RegionInfo {
    std::string name;
    std::unique_ptr<ISubMachine> machine;
    bool required{true};

    RegionInfo(std::string n, std::unique_ptr<ISubMachine> m,
               bool isRequired = true)
        : name(std::move(n)), machine(std::move(m)), required(isRequired) {}
  };

  struct StateInfo {
    std::string name;
    std::shared_ptr<State> state;
    std::vector<RegionInfo> regions;
    std::optional<StateTimeout> timeout;

    StateInfo() = default;

    // Accepts shared_ptr directly, or unique_ptr via implicit conversion
    StateInfo(std::string n, std::shared_ptr<State> s,
              std::unique_ptr<ISubMachine> sub = nullptr)
        : name(std::move(n)), state(std::move(s)) {
      if (sub) {
        regions.emplace_back(std::string(DEFAULT_REGION_NAME), std::move(sub));
      }
    }
  };

  // Core state machine data
  std::unordered_map<StateID, StateInfo> states;
  StateID currentStateId; // protected by stateMutex
  StateID initialStateId;
  std::optional<StateID> fallbackStateId;
  bool isInitialized{false}; // protected by stateMutex

  // Last state whose enter() succeeded. Rollback target when a transition
  // fails and no fallback is usable. Tracked separately from history so it
  // survives withHistorySize(0).
  StateID lastCommittedStateId;

  // Bumped on every committed transition. Used to detect that a nested
  // transition from within enter() has superseded the outer one.
  //
  // Atomic, and always mutated under stateMutex: the write side needs no extra
  // synchronization, but State::stillActive() reads it from update() (which
  // runs unlocked) and must not have to take the lock to poll.
  std::atomic<std::uint64_t> transitionSeq{0};

  // transitionSeq as it stood when the current update() cycle dispatched
  // State::update(). Backs State::shouldCancelUpdate().
  std::atomic<std::uint64_t> updateEpoch{0};

  // Clock reading captured when the active state was entered
  double stateEntryTimestamp{0.0};

  // Configuration
  std::string machineName;
  std::atomic<LogLevel> logLevel{LogLevel::INFO};
  Color machineColor{Color::BLUE};
  bool useColors{true};
  bool strictConfig{false};
  bool notifyInitialState{false};
  LogSink logSink;
  ClockFn clockFn;

  // History tracking (deque: capped history evicts from the front)
  std::deque<TransitionRecord> stateHistory;
  size_t maxHistorySize = 100;

  // Optional transition whitelist. Empty => every transition is permitted.
  std::unordered_map<StateID, std::unordered_set<StateID>> allowedTransitions;
  std::function<bool(const StateID &from, const StateID &to)> transitionGuard;

  // Thread safety - recursive mutex to allow re-entry (e.g. invalid transition
  // -> fallback)
  mutable std::recursive_mutex stateMutex;

  // Re-entrant transition depth guard
  int maxTransitionDepth{8};
  int transitionDepth{0};
  bool stateExitInProgress{false};
  bool stopInProgress{false};
  bool regionLifecycleInProgress{false};

  // Transition requested from within exit(): deferred until the current
  // transition completes, then executed at the top level.
  std::optional<std::pair<StateID, std::string>> pendingExitTransition;

  // Callbacks. Held by shared_ptr so update() can snapshot them under the lock
  // and invoke them unlocked without racing against re-registration.
  std::shared_ptr<StateChangeCallback> onStateChange;
  std::shared_ptr<StateUpdateCallback> onStateUpdate;
  std::shared_ptr<ErrorCallback> onErrorCallback;

  // Type-safe context storage
  std::shared_ptr<void> userContext;
  std::type_index contextType{typeid(void)};

  // Background run-thread support
  std::atomic<bool> runThreadRunning{false};
  std::thread runThread;

  /**
   * @brief True when a record at this level would actually be emitted.
   *
   * Call before building a message so suppressed logs cost nothing.
   */
  bool logEnabled(LogLevel level) const { return logLevel.load() >= level; }

  /**
   * @brief Wrap text in an ANSI code, or return it unchanged when colors are off
   *
   * Closes the span with the matching "off" code rather than a full reset, so
   * tinting a word in the middle of a colored line leaves the rest of that line
   * colored.
   */
  std::string tint(Color color, std::string_view text) const {
    if (!colorsActive()) {
      return std::string(text);
    }
    return std::string(colorToString(color)) + std::string(text) +
           std::string(colorOffToString(color));
  }

  /**
   * @brief Whether ANSI codes should be emitted
   *
   * Driven solely by withColors(), so installing a sink never silently changes
   * the appearance of the output. Call withColors(false) when the sink feeds
   * something that should stay ANSI-free, such as a log file or a GUI.
   */
  bool colorsActive() const { return useColors; }

  /**
   * @brief Render a complete log record, machine name included
   *
   * Single source of truth for the output format, so a sink sees exactly what
   * stdout would have shown. The machine name is always present: with nested
   * sub-machines it is the only thing distinguishing a parent's records from
   * its child's.
   */
  std::string formatRecord(Color color, std::string_view message,
                            bool colorsOn) const {
    if (!colorsOn) {
      return "[" + machineName + "] " + std::string(message);
    }
    return std::string(colorToString(machineColor)) + "[" + machineName + "] " +
           std::string(colorToString(color)) + std::string(message) +
           std::string(RESET);
  }

  std::string formatRecord(Color color, std::string_view message) const {
    return formatRecord(color, message, colorsActive());
  }

  /**
   * @brief Log a message with the specified level
   *
   * Goes to the sink when one is installed, otherwise to std::cout. Either way
   * the record is identical and is colored iff withColors() is enabled.
   */
  void log(LogLevel level, Color color, std::string_view message) const {
    if (!logEnabled(level)) {
      return;
    }
    LogSink sinkCopy;
    bool colorsOn;
    {
      std::lock_guard<std::recursive_mutex> lock(stateMutex);
      sinkCopy = logSink;
      colorsOn = useColors;
    }
    const std::string record = formatRecord(color, message, colorsOn);
    if (sinkCopy) {
      try {
        sinkCopy(level, record);
      } catch (...) {
      }
      return;
    }
    std::cout << record << std::endl;
  }

  /**
   * @brief Current reading of the machine's clock source
   */
  double clockNow() const {
    return clockFn ? clockFn() : steadyClockSeconds();
  }

  /**
   * @brief Append a provisional transition record
   *
   * Capping is deferred until the entry transaction succeeds. Otherwise a
   * failed transition at a full history size would evict a valid old record
   * that rollback could not restore.
   */
  void addToHistory(const StateID &stateId, double timestamp,
                    const std::string &reason) {
    if (maxHistorySize != 0) {
      stateHistory.push_back(TransitionRecord{stateId, timestamp, reason});
    }
  }

  void trimHistory() {
    while (stateHistory.size() > maxHistorySize) {
      stateHistory.pop_front();
    }
  }

  /**
   * @brief Invoke the error callback, isolating exceptions
   */
  void reportError(std::string_view message) const {
    auto callback = onErrorCallback;
    if (!callback || !*callback) {
      return;
    }
    try {
      (*callback)(message, currentStateId);
    } catch (const std::exception &e) {
      log(LogLevel::ERROR, Color::RED,
          std::string("Exception in error callback: ") + e.what());
    } catch (...) {
      log(LogLevel::ERROR, Color::RED, "Unknown exception in error callback");
    }
  }

  /**
   * @brief Call State::enter(), converting any exception into failure
   */
  bool safeEnter(State *state) const {
    if (!state) {
      return true;
    }
    try {
      return state->enter();
    } catch (const std::exception &e) {
      log(LogLevel::ERROR, Color::RED,
          std::string("Exception in state enter: ") + e.what());
    } catch (...) {
      log(LogLevel::ERROR, Color::RED, "Unknown exception in state enter");
    }
    return false;
  }

  /**
   * @brief Call State::exit(), swallowing any exception
   */
  void safeExit(State *state, std::string_view context) const {
    if (!state) {
      return;
    }
    try {
      state->exit();
    } catch (const std::exception &e) {
      log(LogLevel::ERROR, Color::RED, std::string("Exception in state exit (") +
                                           std::string(context) +
                                           "): " + e.what());
    } catch (...) {
      log(LogLevel::ERROR, Color::RED,
          std::string("Unknown exception in state exit (") +
              std::string(context) + ")");
    }
  }

  /**
   * @brief Run an exit hook while preventing transitions from that hook
   */
  void guardedExit(State *state, std::string_view context) {
    if (!state) {
      return;
    }
    const bool wasExiting = stateExitInProgress;
    stateExitInProgress = true;
    safeExit(state, context);
    stateExitInProgress = wasExiting;
  }

  /**
   * @brief Execute transitions deferred from exit() hooks
   *
   * Called at the top level (transitionDepth == 0) after changeStateInternal
   * completes. Each deferred transition may itself defer another, so loop
   * up to maxTransitionDepth times.
   */
  void executePendingExitTransitions() {
    std::lock_guard<std::recursive_mutex> lock(stateMutex);
    int iterations = 0;
    while (pendingExitTransition.has_value() && iterations < maxTransitionDepth) {
      auto pending = std::move(*pendingExitTransition);
      pendingExitTransition.reset();
      ++iterations;
      changeStateInternal(pending.first, std::move(pending.second));
    }
  }

  /**
   * @brief Start a child sub-machine, converting exceptions into failure
   */
  bool safeChildStart(ISubMachine *child, std::string_view context) const {
    if (!child) {
      return false;
    }
    try {
      return child->start();
    } catch (const std::exception &e) {
      log(LogLevel::ERROR, Color::RED,
          std::string("Exception in child SM (") + std::string(context) +
              "): " + e.what());
    } catch (...) {
      log(LogLevel::ERROR, Color::RED,
          std::string("Unknown exception in child SM (") +
              std::string(context) + ")");
    }
    return false;
  }

  /**
   * @brief Drive a child sub-machine lifecycle hook, swallowing any exception
   */
  void safeChild(ISubMachine *child, void (ISubMachine::*hook)(),
                 std::string_view context) const {
    if (!child) {
      return;
    }
    try {
      (child->*hook)();
    } catch (const std::exception &e) {
      log(LogLevel::ERROR, Color::RED, std::string("Exception in child SM (") +
                                           std::string(context) +
                                           "): " + e.what());
    } catch (...) {
      log(LogLevel::ERROR, Color::RED,
          std::string("Unknown exception in child SM (") +
              std::string(context) + ")");
    }
  }

  RegionInfo *findRegion(StateInfo &info, std::string_view regionName) {
    auto it = std::find_if(
        info.regions.begin(), info.regions.end(),
        [regionName](const RegionInfo &region) {
          return region.name == regionName;
        });
    return it == info.regions.end() ? nullptr : &*it;
  }

  const RegionInfo *findRegion(const StateInfo &info,
                               std::string_view regionName) const {
    auto it = std::find_if(
        info.regions.begin(), info.regions.end(),
        [regionName](const RegionInfo &region) {
          return region.name == regionName;
        });
    return it == info.regions.end() ? nullptr : &*it;
  }

  /**
   * @brief Start all regions in registration order
   *
   * Required-region startup is transactional. If a required region fails, all
   * regions started by this call are stopped in reverse order.
   */
  bool startRegions(StateInfo &info, std::string_view context) {
    struct RegionLifecycleGuard {
      bool &flag;
      bool previous;
      explicit RegionLifecycleGuard(bool &f) : flag(f), previous(f) {
        flag = true;
      }
      ~RegionLifecycleGuard() { flag = previous; }
    } lifecycleGuard{regionLifecycleInProgress};

    std::vector<ISubMachine *> started;
    started.reserve(info.regions.size());

    for (auto &region : info.regions) {
      const std::string regionContext =
          std::string(context) + " region " + region.name;
      if (safeChildStart(region.machine.get(), regionContext)) {
        started.push_back(region.machine.get());
        continue;
      }

      log(region.required ? LogLevel::ERROR : LogLevel::WARN,
          region.required ? Color::RED : Color::YELLOW,
          std::string(region.required ? "Required" : "Optional") +
              " region failed to start: " + region.name);

      if (!region.required) {
        continue;
      }

      for (auto it = started.rbegin(); it != started.rend(); ++it) {
        if ((*it)->isActive()) {
          safeChild(*it, &ISubMachine::stop, "region startup rollback");
        }
      }
      return false;
    }
    return true;
  }

  /**
   * @brief Stop every active region in reverse registration order
   */
  void stopRegions(StateInfo &info, std::string_view context) {
    struct RegionLifecycleGuard {
      bool &flag;
      bool previous;
      explicit RegionLifecycleGuard(bool &f) : flag(f), previous(f) {
        flag = true;
      }
      ~RegionLifecycleGuard() { flag = previous; }
    } lifecycleGuard{regionLifecycleInProgress};

    for (auto it = info.regions.rbegin(); it != info.regions.rend(); ++it) {
      if (it->machine && it->machine->isActive()) {
        safeChild(it->machine.get(), &ISubMachine::stop,
                  std::string(context) + " region " + it->name);
      }
    }
  }

  /**
   * @brief Commit arrival in a state: timestamp, history and sequence number
   *
   * Runs before enter() so re-entrant transitions from within enter() observe a
   * consistent machine and history stays in chronological order.
   * lastCommittedStateId is deliberately *not* updated here: it must keep
   * naming a state that was successfully entered so it stays a valid rollback
   * target if this enter() fails.
   */
  void commitEntry(const StateID &stateId, const std::string &reason) {
    currentStateId = stateId;
    stateEntryTimestamp = clockNow();
    addToHistory(stateId, stateEntryTimestamp, reason);
    ++transitionSeq;
  }

  /**
   * @brief Undo the history record written by commitEntry()
   */
  void rollbackHistory(const StateID &stateId) {
    if (!stateHistory.empty() && stateHistory.back().state == stateId) {
      stateHistory.pop_back();
    }
  }

  /**
   * @brief Fire the state-change observer (called with stateMutex held)
   */
  void notifyStateChange(const StateID &from, const StateID &to,
                         std::string_view fromName, std::string_view toName,
                         std::string_view reason) const {
    auto callback = onStateChange;
    if (!callback || !*callback) {
      return;
    }
    try {
      (*callback)(from, to, fromName, toName, reason);
    } catch (const std::exception &e) {
      log(LogLevel::ERROR, Color::RED,
          std::string("Exception in state change callback: ") + e.what());
    } catch (...) {
      log(LogLevel::ERROR, Color::RED,
          "Unknown exception in state change callback");
    }
  }

  /**
   * @brief Check a proposed transition against the guard and whitelist
   */
  bool isTransitionAllowed(const StateID &from, const StateID &to) const {
    if (transitionGuard && !transitionGuard(from, to)) {
      return false;
    }
    if (allowedTransitions.empty()) {
      return true; // no whitelist declared => permissive
    }
    auto it = allowedTransitions.find(from);
    return it != allowedTransitions.end() && it->second.count(to) > 0;
  }

  /**
   * @brief Last-resort recovery: restore the last successfully entered state.
   *
   * The failed state's exit() and child teardown have already run, so the
   * rollback target is re-entered to keep the state object's view of itself
   * consistent with the machine's. Never routes through
   * changeStateInternal(), so it cannot recurse back into failure handling.
   */
  void rollbackToLastCommitted(const std::string &reason) {
    if (!isInitialized) {
      return;
    }

    auto it = states.find(lastCommittedStateId);
    if (it == states.end()) {
      log(LogLevel::ERROR, Color::RED,
          std::string("No recoverable state, stopping state machine: ") +
              reason);
      isInitialized = false;
      reportError(reason);
      return;
    }

    log(LogLevel::ERROR, Color::RED,
        std::string("State transition failed with no usable fallback, rolling "
                    "back to ") +
            it->second.name + ": " + reason);

    currentStateId = lastCommittedStateId;
    stateEntryTimestamp = clockNow();
    ++transitionSeq;

    if (safeEnter(it->second.state.get()) &&
        startRegions(it->second, "rollback start")) {
      lastCommittedStateId = currentStateId;
    } else {
      stopRegions(it->second, "rollback failure");
      guardedExit(it->second.state.get(), "rollback failure");
      log(LogLevel::ERROR, Color::RED,
          "Rollback state or one of its required regions failed to start - "
          "stopping state machine");
      isInitialized = false;
    }

    reportError(reason);
  }

  /**
   * @brief Handle state transition failure with fallback, then rollback
   *
   * @return false always — the original transition failed, even if the fallback
   *         recovery succeeded. Callers must know the requested state was not reached.
   */
  bool handleTransitionFailure(const StateID &failedState,
                               const std::string &reason) {
    const bool fallbackRegistered =
        fallbackStateId.has_value() &&
        states.find(fallbackStateId.value()) != states.end();

    if (fallbackStateId.has_value() && !fallbackRegistered) {
      log(LogLevel::ERROR, Color::RED,
          "Fallback state is not registered - cannot fall back");
    }

    const std::uint64_t seqBeforeFallback = transitionSeq;

    if (fallbackRegistered && fallbackStateId.value() != failedState) {
      log(LogLevel::WARN, Color::YELLOW,
          std::string("State transition failed, falling back: ") + reason);
      if (changeStateInternal(fallbackStateId.value(), "Fallback after failure",
                              /*bypassGuard=*/true)) {
        return false;
      }
      // The fallback failed too. If it moved the machine at all it has already
      // run its own recovery, so don't roll back a second time.
      if (transitionSeq != seqBeforeFallback) {
        return false;
      }
    }

    // No transition moved us away from the failed state, so balance the failed
    // enter() with teardown before restoring the previous committed state.
    auto failedIt = states.find(failedState);
    if (currentStateId == failedState && failedIt != states.end()) {
      stopRegions(failedIt->second, "failed state cleanup");
      guardedExit(failedIt->second.state.get(), "failed state cleanup");
    }

    rollbackToLastCommitted(reason);
    return false;
  }

  /**
   * @brief Internal state change implementation
   */
  bool changeStateInternal(const StateID &newStateId, std::string reason,
                           bool bypassGuard = false, bool forceReenter = false) {
    std::lock_guard<std::recursive_mutex> lock(stateMutex);

    if (!isInitialized) {
      log(LogLevel::WARN, Color::YELLOW,
          "Attempted state change before initialization");
      return false;
    }

    if (stopInProgress || regionLifecycleInProgress) {
      log(LogLevel::WARN, Color::YELLOW,
          "Rejected transition requested during state teardown");
      return false;
    }

    if (stateExitInProgress) {
      pendingExitTransition = {newStateId, reason};
      log(LogLevel::DEBUG, Color::CYAN,
          "Transition requested during exit() - deferred");
      return false;
    }

    if (transitionDepth >= maxTransitionDepth) {
      log(LogLevel::ERROR, Color::RED,
          "Max transition depth exceeded - aborting to prevent infinite "
          "recursion");
      reportError("Max transition depth exceeded");
      return false;
    }
    ++transitionDepth;
    // RAII guard to decrement depth on all exit paths
    struct DepthGuard {
      int &depth;
      ~DepthGuard() { --depth; }
    } depthGuard{transitionDepth};

    auto oldStateIter = states.find(currentStateId);
    auto newStateIter = states.find(newStateId);

    if (oldStateIter == states.end() || newStateIter == states.end()) {
      log(LogLevel::ERROR, Color::RED, "State transition between invalid states");
      return false;
    }

    // No need to change to the same state unless a re-entry was requested
    if (currentStateId == newStateId && !forceReenter) {
      log(LogLevel::DEBUG, Color::CYAN, "Ignored transition to same state");
      return true;
    }

    if (!bypassGuard && !isTransitionAllowed(currentStateId, newStateId)) {
      log(LogLevel::WARN, Color::YELLOW,
          std::string("Rejected disallowed transition: ") +
              oldStateIter->second.name + " -> " + newStateIter->second.name);
      return false;
    }

    const StateID oldStateId = currentStateId;
    // Copies, not references: enter() may register states and the names are
    // handed to observers after it returns.
    const std::string oldStateName = oldStateIter->second.name;
    const std::string newStateName = newStateIter->second.name;

    if (logEnabled(LogLevel::INFO)) {
      const std::string reasonText =
          reason.empty() ? "" : std::string("\t(Reason: ") + reason + ")";
      // ASCII only: this line travels through the ROS logging pipeline, which
      // mangles multi-byte UTF-8 under a non-UTF-8 locale (the default in the
      // containers), and it has to stay greppable in flight logs.
      log(LogLevel::INFO, Color::BLUE,
          "|" + tint(Color::STRIKETHROUGH, oldStateName) + "|  ->  |" +
              newStateName + "|" + reasonText);
    }

    // Bottom-up teardown: child sub-machine first, then the state itself
    stopRegions(oldStateIter->second, "transition");
    guardedExit(oldStateIter->second.state.get(), "transition");

    // Commit arrival before enter() so nested transitions see a consistent
    // machine and history stays ordered.
    commitEntry(newStateId, reason);
    const std::uint64_t seqAtCommit = transitionSeq;

    if (!safeEnter(newStateIter->second.state.get())) {
      if (transitionSeq != seqAtCommit) {
        // enter() failed *after* re-entrantly transitioning elsewhere. That
        // nested transition already committed, so leave the machine where it
        // put us rather than unwinding on top of it.
        log(LogLevel::WARN, Color::YELLOW,
            "enter() failed after a nested transition already committed - "
            "keeping the nested target");
        return false;
      }
      rollbackHistory(newStateId);
      return handleTransitionFailure(newStateId,
                                     "State enter() returned false");
    }

    if (transitionSeq != seqAtCommit) {
      // A nested transition from within enter() superseded this one. It already
      // started its own child sub-machine, updated the rollback target and
      // notified observers; repeating any of that here would start the wrong
      // child and report a stale arrival.
      log(LogLevel::DEBUG, Color::CYAN,
          "Transition superseded by a nested transition from enter()");
      return true;
    }

    // Top-down start of every orthogonal region. Required regions form part of
    // the state's entry transaction.
    if (!startRegions(newStateIter->second, "transition start")) {
      rollbackHistory(newStateId);
      return handleTransitionFailure(newStateId,
                                     "Required region failed to start");
    }

    // The state and all required regions entered successfully and stuck.
    lastCommittedStateId = newStateId;
    trimHistory();

    notifyStateChange(oldStateId, newStateId, oldStateName, newStateName,
                      reason);
    return true;
  }

  /**
   * @brief Validate the configuration. Caller must hold stateMutex.
   *
   * Covers the initial/current/fallback states, every declared timeout target,
   * every whitelisted transition endpoint, and recurses into child machines.
   */
  bool validateLocked() const {
    bool valid = true;

    if (states.find(initialStateId) == states.end()) {
      log(LogLevel::ERROR, Color::RED,
          "Initial state not found in state registry");
      valid = false;
    }

    if (states.find(currentStateId) == states.end()) {
      log(LogLevel::ERROR, Color::RED,
          "Current state not found in state registry");
      valid = false;
    }

    if (fallbackStateId.has_value() &&
        states.find(fallbackStateId.value()) == states.end()) {
      log(LogLevel::ERROR, Color::RED,
          "Fallback state not found in state registry");
      valid = false;
    }

    for (const auto &entry : states) {
      const StateInfo &info = entry.second;

      if (info.timeout.has_value() &&
          states.find(info.timeout->target) == states.end()) {
        log(LogLevel::ERROR, Color::RED,
            std::string("Timeout target of state ") + info.name +
                " is not registered");
        valid = false;
      }

      std::unordered_set<std::string> regionNames;
      for (const auto &region : info.regions) {
        if (!region.machine) {
          log(LogLevel::ERROR, Color::RED,
              std::string("Region ") + region.name + " of state " + info.name +
                  " has no machine");
          valid = false;
          continue;
        }
        if (region.name.empty() || !regionNames.insert(region.name).second) {
          log(LogLevel::ERROR, Color::RED,
              std::string("Invalid or duplicate region name in state ") +
                  info.name);
          valid = false;
        }
        if (!region.machine->validate()) {
          log(LogLevel::ERROR, Color::RED,
              std::string("Region ") + region.name + " of state " + info.name +
                  " is invalid");
          valid = false;
        }
      }
    }

    for (const auto &entry : allowedTransitions) {
      if (states.find(entry.first) == states.end()) {
        log(LogLevel::ERROR, Color::RED,
            "Whitelisted transition source is not registered");
        valid = false;
      }
      for (const auto &target : entry.second) {
        if (states.find(target) == states.end()) {
          log(LogLevel::ERROR, Color::RED,
              "Whitelisted transition target is not registered");
          valid = false;
        }
      }
    }

    return valid;
  }

  /**
   * @brief Push the current logging configuration to all child sub-machines
   *
   * Caller must hold stateMutex.
   */
  void propagateLogConfig() {
    for (auto &entry : states) {
      for (auto &region : entry.second.regions) {
        if (region.machine) {
          region.machine->applyLogConfig(logLevel.load(), logSink, useColors);
        }
      }
    }
  }

  /**
   * @brief Warn about (and optionally reject) configuration changes after start
   *
   * @return true if the caller must abandon the change
   */
  bool configChangeRejected(std::string_view what) const {
    if (!isInitialized) {
      return false;
    }
    log(strictConfig ? LogLevel::ERROR : LogLevel::WARN,
        strictConfig ? Color::RED : Color::YELLOW,
        std::string(what) + " after start() - configuration should be frozen "
                            "before the machine runs");
    return strictConfig;
  }

  /**
   * @brief Single insertion point for all addState overloads
   *
   * Caller must hold stateMutex.
   */
  bool registerState(const StateID &id, std::string_view name,
                     std::unique_ptr<State> state,
                     std::unique_ptr<ISubMachine> sub) {
    if (configChangeRejected(std::string("Adding state ") +
                             std::string(name))) {
      return false;
    }

    if (states.find(id) != states.end()) {
      log(LogLevel::WARN, Color::YELLOW,
          std::string("State ") + std::string(name) +
              " already exists, skipping...");
      return false;
    }

    if (!state) {
      log(LogLevel::ERROR, Color::RED,
          std::string("Refusing to add null state ") + std::string(name));
      return false;
    }

    state->stateMachine = this;
    state->myStateId = id;
    const bool hasChild = sub != nullptr;
    if (hasChild) {
      sub->applyLogConfig(logLevel.load(), logSink, useColors);
      sub->applyClock(clockFn);
    }
    states.emplace(id,
                   StateInfo(std::string(name), std::move(state),
                             std::move(sub)));

    log(LogLevel::DEBUG, Color::CYAN,
        std::string(hasChild ? "Added state with child SM: " : "Added state: ") +
            std::string(name));
    return true;
  }

  /**
   * @brief Get the child machine attached to a parent state, creating it if
   *        this is the first sub-state registered for that parent.
   *
   * Caller must hold stateMutex. Returns nullptr if the parent does not exist
   * or already owns a child machine of a different StateID type.
   */
  template <typename ChildStateID>
  StateMachine<ChildStateID> *getOrCreateChildSM(const StateID &parentId,
                                                 const ChildStateID &initialChild,
                                                 std::string_view caller) {
    auto it = states.find(parentId);
    if (it == states.end()) {
      log(LogLevel::ERROR, Color::RED,
          std::string(caller) + ": parent state not found");
      return nullptr;
    }

    RegionInfo *region = findRegion(it->second, DEFAULT_REGION_NAME);
    if (!region) {
      auto newChild = std::make_unique<StateMachine<ChildStateID>>(
          initialChild, it->second.name + "/Sub");
      auto *raw = newChild.get();
      auto sub =
          std::make_unique<SubMachineImpl<ChildStateID>>(std::move(newChild));
      sub->applyLogConfig(logLevel.load(), logSink, useColors);
      sub->applyClock(clockFn);
      it->second.regions.emplace_back(std::string(DEFAULT_REGION_NAME),
                                      std::move(sub));
      return raw;
    }

    auto *impl =
        dynamic_cast<SubMachineImpl<ChildStateID> *>(region->machine.get());
    if (!impl) {
      log(LogLevel::ERROR, Color::RED,
          std::string(caller) + ": child state type mismatch");
      return nullptr;
    }
    return &impl->machine();
  }

public:
  /**
   * @brief Create a state machine starting in the specified state
   * @param initialState The state ID to start in
   * @param name Name for this state machine instance
   * @param color Color for state machine output
   */
  explicit StateMachine(const StateID &initialState,
                        std::string name = "StateMachine",
                        Color color = Color::BLUE)
      : currentStateId(initialState), initialStateId(initialState),
        lastCommittedStateId(initialState), machineName(std::move(name)),
        machineColor(color) {}

  /**
   * @brief Exit the active state on teardown
   *
   * Without this, destroying a running machine would skip exit() and child
   * stop(), leaking whatever the active state acquired in enter(). Safe to call
   * here: `states` (and therefore the State objects) outlive the destructor
   * body.
   */
  ~StateMachine() {
    stopThread();
    stop();
    std::lock_guard<std::recursive_mutex> lock(stateMutex);
    for (auto &entry : states) {
      if (entry.second.state) {
        entry.second.state->stateMachine = nullptr;
      }
    }
  }

  // Disable copy constructor and assignment
  StateMachine(const StateMachine &) = delete;
  StateMachine &operator=(const StateMachine &) = delete;

  // Move is disabled (recursive_mutex and atomic are not movable)
  StateMachine(StateMachine &&) = delete;
  StateMachine &operator=(StateMachine &&) = delete;

  /**
   * @brief Add a state to the state machine
   * @tparam StateType The class implementing the state
   * @param id The unique state ID
   * @param name A human-readable name for the state
   * @return StateMachine& This state machine for method chaining
   */
  template <typename StateType>
  StateMachine &addState(const StateID &id, std::string_view name) {
    static_assert(std::is_base_of_v<State, StateType>,
                  "StateType must inherit from State");
    return addState(id, name, std::make_unique<StateType>());
  }

  /**
   * @brief Add a pre-configured state to the state machine
   * @param id The unique state ID
   * @param name A human-readable name for the state
   * @param state A pre-configured state instance
   * @return StateMachine& This state machine for method chaining
   */
  StateMachine &addState(const StateID &id, std::string_view name,
                         std::unique_ptr<State> state) {
    std::lock_guard<std::recursive_mutex> lock(stateMutex);
    registerState(id, name, std::move(state), nullptr);
    return *this;
  }

  /**
   * @brief Add a state with an associated child state machine
   * @tparam ChildStateID The state ID type of the child state machine
   * @param id The unique state ID
   * @param name A human-readable name for the state
   * @param state A pre-configured state instance
   * @param childMachine The child state machine (heap-allocated)
   * @return StateMachine& This state machine for method chaining
   */
  template <typename ChildStateID>
  StateMachine &
  addState(const StateID &id, std::string_view name,
           std::unique_ptr<State> state,
           std::unique_ptr<StateMachine<ChildStateID>> childMachine) {
    std::lock_guard<std::recursive_mutex> lock(stateMutex);
    if (!childMachine) {
      log(LogLevel::ERROR, Color::RED,
          std::string("Refusing to add state with null child machine: ") +
              std::string(name));
      return *this;
    }
    registerState(
        id, name, std::move(state),
        std::make_unique<SubMachineImpl<ChildStateID>>(std::move(childMachine)));
    return *this;
  }

  /**
   * @brief Add a state (by type) with an associated child state machine
   * @tparam StateType The class implementing the state
   * @tparam ChildStateID The state ID type of the child state machine
   * @param id The unique state ID
   * @param name A human-readable name for the state
   * @param childMachine The child state machine (heap-allocated)
   * @return StateMachine& This state machine for method chaining
   */
  template <typename StateType, typename ChildStateID>
  StateMachine &
  addState(const StateID &id, std::string_view name,
           std::unique_ptr<StateMachine<ChildStateID>> childMachine) {
    static_assert(std::is_base_of_v<State, StateType>,
                  "StateType must inherit from State");
    return addState(id, name, std::make_unique<StateType>(),
                    std::move(childMachine));
  }

  // ──────────────────────────────────────────────────────────────────
  //  addSubState — the simple way to attach sub-states
  // ──────────────────────────────────────────────────────────────────

  /**
   * @brief Add a sub-state (by type) to a parent state's child state machine.
   *
   * Creates the child SM automatically on the first call for each parent.
   * The first sub-state added becomes the initial sub-state (override with
   * setInitialSubState).
   *
   * @code
   *   sm.addState<FlyingState>(Main::FLYING, "Flying")
   *     .addSubState<TakeoffState, FlySub>(Main::FLYING, FlySub::TAKEOFF,
   * "Takeoff") .addSubState<CruiseState,  FlySub>(Main::FLYING, FlySub::CRUISE,
   * "Cruise") .start();
   * @endcode
   */
  template <typename SubStateType, typename ChildStateID>
  StateMachine &addSubState(const StateID &parentId,
                            const ChildStateID &childId,
                            std::string_view name) {
    static_assert(
        std::is_base_of_v<typename StateMachine<ChildStateID>::State,
                          SubStateType>,
        "SubStateType must inherit from StateMachine<ChildStateID>::State");
    return addSubState<ChildStateID>(parentId, childId, name,
                                     std::make_unique<SubStateType>());
  }

  /**
   * @brief Add a pre-built sub-state to a parent state's child state machine.
   *
   * @code
   *   auto takeoff = std::make_unique<TakeoffState>(some_arg);
   *   sm.addSubState(Main::FLYING, FlySub::TAKEOFF, "Takeoff",
   * std::move(takeoff));
   * @endcode
   */
  template <typename ChildStateID>
  StateMachine &addSubState(
      const StateID &parentId, const ChildStateID &childId,
      std::string_view name,
      std::unique_ptr<typename StateMachine<ChildStateID>::State> state) {
    std::lock_guard<std::recursive_mutex> lock(stateMutex);

    auto *childSM =
        getOrCreateChildSM<ChildStateID>(parentId, childId, "addSubState");
    if (!childSM) {
      return *this;
    }

    childSM->addState(childId, name, std::move(state));
    return *this;
  }

  /**
   * @brief Override the initial sub-state for a parent state's child SM.
   *        By default the first sub-state added is the initial one.
   */
  template <typename ChildStateID>
  StateMachine &setInitialSubState(const StateID &parentId,
                                   const ChildStateID &childId) {
    std::lock_guard<std::recursive_mutex> lock(stateMutex);

    auto *childSM = getTypedSubMachine<ChildStateID>(parentId);
    if (!childSM) {
      log(LogLevel::ERROR, Color::RED,
          "setInitialSubState: no child SM found for state");
      return *this;
    }
    childSM->setInitialState(childId);
    return *this;
  }

  /**
   * @brief Get the active sub-state name for the current state
   * @return std::string The name of the active sub-state, or empty if none
   */
  std::string getActiveSubStateName() const {
    std::lock_guard<std::recursive_mutex> lock(stateMutex);
    auto it = states.find(currentStateId);
    if (it != states.end()) {
      const RegionInfo *region =
          findRegion(it->second, DEFAULT_REGION_NAME);
      if (region && region->machine && region->machine->isActive()) {
        return region->machine->activeStateName();
      }
    }
    return "";
  }

  /**
   * @brief Slash-separated path of the active state and all active descendants
   *
   * @code  "Flying/Cruise"  @endcode
   * @return std::string The active state path, or "Unknown" if unresolvable
   */
  std::string statePath() const {
    std::lock_guard<std::recursive_mutex> lock(stateMutex);
    auto it = states.find(currentStateId);
    if (it == states.end()) {
      return "Unknown";
    }
    std::string path = it->second.name;

    std::vector<const RegionInfo *> activeRegions;
    for (const auto &region : it->second.regions) {
      if (region.machine && region.machine->isActive()) {
        activeRegions.push_back(&region);
      }
    }

    if (activeRegions.size() == 1 &&
        activeRegions.front()->name == DEFAULT_REGION_NAME) {
      path += "/" + activeRegions.front()->machine->statePath();
    } else if (!activeRegions.empty()) {
      path += "/{";
      for (std::size_t index = 0; index < activeRegions.size(); ++index) {
        if (index != 0) {
          path += ", ";
        }
        path += activeRegions[index]->name + ":" +
                activeRegions[index]->machine->statePath();
      }
      path += "}";
    }
    return path;
  }

  /**
   * @brief Check if the current state has an active sub-machine
   * @return bool True if the current state has an active child SM
   */
  bool hasActiveSubMachine() const {
    std::lock_guard<std::recursive_mutex> lock(stateMutex);
    auto it = states.find(currentStateId);
    if (it == states.end()) {
      return false;
    }
    return std::any_of(
        it->second.regions.begin(), it->second.regions.end(),
        [](const RegionInfo &region) {
          return region.machine && region.machine->isActive();
        });
  }

  /**
   * @brief Get the sub-machine interface for the current state
   * @return ISubMachine* Pointer to the sub-machine, or nullptr
   */
  ISubMachine *getSubMachine() const {
    std::lock_guard<std::recursive_mutex> lock(stateMutex);
    auto it = states.find(currentStateId);
    if (it != states.end()) {
      const RegionInfo *region =
          findRegion(it->second, DEFAULT_REGION_NAME);
      if (region) {
        return region->machine.get();
      }
    }
    return nullptr;
  }

  /**
   * @brief One active orthogonal region, returned by getActiveRegions()
   */
  struct ActiveRegion {
    std::string name;
    std::string stateName;
    std::string statePath;
    bool required{true};
  };

  /**
   * @brief Get every active region of the current state
   *
   * Returned in deterministic registration order.
   */
  std::vector<ActiveRegion> getActiveRegions() const {
    std::lock_guard<std::recursive_mutex> lock(stateMutex);
    std::vector<ActiveRegion> result;
    auto it = states.find(currentStateId);
    if (it == states.end()) {
      return result;
    }
    result.reserve(it->second.regions.size());
    for (const auto &region : it->second.regions) {
      if (region.machine && region.machine->isActive()) {
        result.push_back(ActiveRegion{region.name,
                                      region.machine->activeStateName(),
                                      region.machine->statePath(),
                                      region.required});
      }
    }
    return result;
  }

  /**
   * @brief Get the active state name of a named region on the current state
   */
  std::string getActiveRegionStateName(std::string_view regionName) const {
    std::lock_guard<std::recursive_mutex> lock(stateMutex);
    auto it = states.find(currentStateId);
    if (it == states.end()) {
      return "";
    }
    const RegionInfo *region = findRegion(it->second, regionName);
    return region && region->machine && region->machine->isActive()
               ? region->machine->activeStateName()
               : "";
  }

  /**
   * @brief Check whether a named region is active on the current state
   */
  bool hasActiveRegion(std::string_view regionName) const {
    std::lock_guard<std::recursive_mutex> lock(stateMutex);
    auto it = states.find(currentStateId);
    if (it == states.end()) {
      return false;
    }
    const RegionInfo *region = findRegion(it->second, regionName);
    return region && region->machine && region->machine->isActive();
  }

  /**
   * @brief Get a typed named region machine from the current state
   *
   * The region name disambiguates multiple regions using the same StateID type.
   */
  template <typename ChildStateID>
  StateMachine<ChildStateID> *
  getRegionMachine(std::string_view regionName) const {
    std::lock_guard<std::recursive_mutex> lock(stateMutex);
    auto it = states.find(currentStateId);
    if (it == states.end()) {
      return nullptr;
    }
    const RegionInfo *region = findRegion(it->second, regionName);
    if (!region || !region->machine) {
      return nullptr;
    }
    auto *impl =
        dynamic_cast<SubMachineImpl<ChildStateID> *>(region->machine.get());
    return impl ? &impl->machine() : nullptr;
  }

  /**
   * @brief Get a typed named region machine from a specific parent state
   */
  template <typename ChildStateID>
  StateMachine<ChildStateID> *
  getRegionMachine(const StateID &parentStateId,
                   std::string_view regionName) const {
    std::lock_guard<std::recursive_mutex> lock(stateMutex);
    auto it = states.find(parentStateId);
    if (it == states.end()) {
      return nullptr;
    }
    const RegionInfo *region = findRegion(it->second, regionName);
    if (!region || !region->machine) {
      return nullptr;
    }
    auto *impl =
        dynamic_cast<SubMachineImpl<ChildStateID> *>(region->machine.get());
    return impl ? &impl->machine() : nullptr;
  }

  /**
   * @brief Change one named region without affecting its sibling regions
   */
  template <typename ChildStateID>
  bool changeRegionState(std::string_view regionName,
                         const ChildStateID &childState,
                         std::string reason = "") {
    auto *region = getRegionMachine<ChildStateID>(regionName);
    return region
               ? region->changeState(childState, std::move(reason))
               : false;
  }

  // ──────────────────────────────────────────────────────────────────
  //  Ergonomic sub-state API
  // ──────────────────────────────────────────────────────────────────

  /**
   * @brief Attach an independently active named region to a parent state
   *
   * Every named region owns a complete StateMachine<ChildStateID>. All regions
   * of the active parent are started and updated in registration order and
   * stopped in reverse order. Region machines can themselves use withRegion()
   * or withSubStates(), so hierarchy depth is not limited.
   *
   * A required region participates in the parent's entry transaction: if it
   * cannot start, already-started sibling regions are stopped and the parent
   * transition enters normal fallback/rollback recovery. Optional regions log
   * a warning but do not fail the parent.
   *
   * Region topology is frozen once start() has been called.
   *
   * @code
   * sm.withRegion<MotionState>(Main::FLYING, "Motion",
   *                            MotionState::HOVER,
   *     [](auto& motion) {
   *       motion.template addState<Hover>(MotionState::HOVER, "Hover");
   *     });
   * @endcode
   */
  template <typename ChildStateID, typename ConfigFn>
  StateMachine &withRegion(const StateID &parentStateId,
                           std::string_view regionName,
                           const ChildStateID &initialChild,
                           ConfigFn &&configureFn, bool required = true) {
    std::lock_guard<std::recursive_mutex> lock(stateMutex);

    if (isInitialized) {
      log(LogLevel::ERROR, Color::RED,
          "withRegion: region topology cannot change after start()");
      return *this;
    }
    if (regionName.empty()) {
      log(LogLevel::ERROR, Color::RED,
          "withRegion: region name must not be empty");
      return *this;
    }

    auto parent = states.find(parentStateId);
    if (parent == states.end()) {
      log(LogLevel::ERROR, Color::RED, "withRegion: parent state not found");
      return *this;
    }
    if (findRegion(parent->second, regionName)) {
      log(LogLevel::ERROR, Color::RED,
          std::string("withRegion: duplicate region name: ") +
              std::string(regionName));
      return *this;
    }

    const std::string childMachineName =
        regionName == DEFAULT_REGION_NAME
            ? parent->second.name + "/Sub"
            : parent->second.name + "/" + std::string(regionName);
    auto child = std::make_unique<StateMachine<ChildStateID>>(
        initialChild, childMachineName);
    std::forward<ConfigFn>(configureFn)(*child);

    auto regionMachine =
        std::make_unique<SubMachineImpl<ChildStateID>>(std::move(child));
    regionMachine->applyLogConfig(logLevel.load(), logSink, useColors);
    regionMachine->applyClock(clockFn);
    parent->second.regions.emplace_back(std::string(regionName),
                                        std::move(regionMachine), required);

    log(LogLevel::DEBUG, Color::CYAN,
        std::string("Attached region ") + std::string(regionName) + " to " +
            parent->second.name);
    return *this;
  }

  /**
   * @brief Attach a child state machine to an existing state via a
   *        configuration lambda. The child SM is created internally.
   *
   * Usage:
   * @code
   *   sm.addState<FlyingState>(MainState::FLYING, "Flying")
   *     .withSubStates<FlySub>(MainState::FLYING, FlySub::TAKEOFF,
   *         [](auto& sub) {
   *           sub.template addState<TakeoffState>(FlySub::TAKEOFF, "Takeoff")
   *              .template addState<CruiseState>(FlySub::CRUISE, "Cruise");
   *         })
   *     .start();
   * @endcode
   *
   * @tparam ChildStateID  Enum type for the child states
   * @tparam ConfigFn      Lambda / callable taking StateMachine<ChildStateID>&
   * @param parentStateId  The parent state that will own the child SM
   * @param initialChild   Initial sub-state when the parent is entered
   * @param configureFn    Lambda that adds states to the child SM
   * @return StateMachine& This state machine for method chaining
   */
  template <typename ChildStateID, typename ConfigFn>
  StateMachine &withSubStates(const StateID &parentStateId,
                              const ChildStateID &initialChild,
                              ConfigFn &&configureFn) {
    return withRegion<ChildStateID>(
        parentStateId, DEFAULT_REGION_NAME, initialChild,
        std::forward<ConfigFn>(configureFn), true);
  }

  /**
   * @brief Attach a pre-built child state machine to an existing parent state.
   *
   * Unlike withSubStates(), which creates and configures the child SM via a
   * lambda (requiring .template syntax and causing deep nesting), attachSub()
   * accepts an independently constructed StateMachine. This enables a flat,
   * bottom-up construction style:
   *
   * @code
   *   auto core = sm::make<CoreState>(CoreState::INIT, "Core");
   *   core->addState<CoreInitState>(CoreState::INIT, "Init")
   *       .addState<CoreActiveState>(CoreState::ACTIVE, "Active");
   *
   *   sm.addState<RunningState>(Main::RUNNING, "Running")
   *     .attachSub(Main::RUNNING, std::move(core));
   * @endcode
   *
   * @tparam ChildStateID  Enum type for the child states
   * @param parentStateId  The parent state that will own the child SM
   * @param childMachine   A pre-built, fully configured child state machine
   * @return StateMachine& This state machine for method chaining
   */
  template <typename ChildStateID>
  StateMachine &attachSub(const StateID &parentStateId,
                          std::unique_ptr<StateMachine<ChildStateID>> childMachine) {
    std::lock_guard<std::recursive_mutex> lock(stateMutex);

    if (isInitialized) {
      log(LogLevel::ERROR, Color::RED,
          "attachSub: sub-machine topology cannot change after start()");
      return *this;
    }
    if (!childMachine) {
      log(LogLevel::ERROR, Color::RED,
          "attachSub: child machine must not be null");
      return *this;
    }

    auto parent = states.find(parentStateId);
    if (parent == states.end()) {
      log(LogLevel::ERROR, Color::RED,
          "attachSub: parent state not found");
      return *this;
    }
    if (findRegion(parent->second, DEFAULT_REGION_NAME)) {
      log(LogLevel::ERROR, Color::RED,
          "attachSub: parent state already has a default sub-machine");
      return *this;
    }

    auto regionMachine =
        std::make_unique<SubMachineImpl<ChildStateID>>(std::move(childMachine));
    regionMachine->applyLogConfig(logLevel.load(), logSink, useColors);
    regionMachine->applyClock(clockFn);
    parent->second.regions.emplace_back(std::string(DEFAULT_REGION_NAME),
                                        std::move(regionMachine), true);

    log(LogLevel::DEBUG, Color::CYAN,
        std::string("Attached pre-built sub-machine to ") +
            parent->second.name);
    return *this;
  }

  /**
   * @brief Get the typed child state machine for the current state
   *
   * Usage:
   * @code
   *   auto* child = sm.getTypedSubMachine<FlySub>();
   *   if (child) child->changeState(FlySub::CRUISE, "altitude ok");
   * @endcode
   *
   * @tparam ChildStateID  The child enum type
   * @return StateMachine<ChildStateID>* Pointer, or nullptr if no match
   */
  template <typename ChildStateID>
  StateMachine<ChildStateID> *getTypedSubMachine() const {
    auto *sub = getSubMachine();
    if (!sub)
      return nullptr;
    auto *impl = dynamic_cast<SubMachineImpl<ChildStateID> *>(sub);
    return impl ? &impl->machine() : nullptr;
  }

  /**
   * @brief Get the typed child state machine for a specific parent state
   *
   * @tparam ChildStateID  The child enum type
   * @param parentStateId  The parent state to look up
   * @return StateMachine<ChildStateID>* Pointer, or nullptr if no match
   */
  template <typename ChildStateID>
  StateMachine<ChildStateID> *
  getTypedSubMachine(const StateID &parentStateId) const {
    return getRegionMachine<ChildStateID>(parentStateId,
                                          DEFAULT_REGION_NAME);
  }

  /**
   * @brief Change the active sub-state of the current state's child SM
   *
   * Usage:
   * @code
   *   sm.changeSubState(FlySub::CRUISE, "altitude reached");
   * @endcode
   *
   * @tparam ChildStateID  The child enum type
   * @param childState     The sub-state to transition to
   * @param reason         Optional reason for the transition
   * @return bool True if successful, false if no matching child SM
   */
  template <typename ChildStateID>
  bool changeSubState(const ChildStateID &childState, std::string reason = "") {
    auto *child = getTypedSubMachine<ChildStateID>();
    if (!child)
      return false;
    return child->changeState(childState, std::move(reason));
  }

  /**
   * @brief Set the user context object with type safety
   * @tparam T Type of the context object
   * @param context Shared pointer to the context object
   * @return StateMachine& This state machine for method chaining
   */
  template <typename T> StateMachine &withContext(std::shared_ptr<T> context) {
    std::lock_guard<std::recursive_mutex> lock(stateMutex);
    userContext = std::static_pointer_cast<void>(context);
    contextType = std::type_index(typeid(T));
    return *this;
  }

  /**
   * @brief Get the user context with type safety
   * @tparam T The type to cast the context to
   * @return std::shared_ptr<T> Shared pointer to the context object
   * @throws std::runtime_error if context type doesn't match
   */
  template <typename T> std::shared_ptr<T> getContext() const {
    std::lock_guard<std::recursive_mutex> lock(stateMutex);
    if (!userContext) {
      throw std::runtime_error("No context set");
    }

    if (contextType != std::type_index(typeid(T))) {
      throw std::runtime_error("Context type mismatch");
    }

    return std::static_pointer_cast<T>(userContext);
  }

  /**
   * @brief Get the user context, or nullptr if unset or of a different type
   *
   * Non-throwing counterpart to getContext(), preferable inside enter() where
   * an exception is silently turned into a transition failure.
   */
  template <typename T> std::shared_ptr<T> tryGetContext() const {
    std::lock_guard<std::recursive_mutex> lock(stateMutex);
    if (!userContext || contextType != std::type_index(typeid(T))) {
      return nullptr;
    }
    return std::static_pointer_cast<T>(userContext);
  }

  /**
   * @brief Set fallback state for error recovery
   * @param fallbackState The state to fall back to on errors
   * @return StateMachine& This state machine for method chaining
   */
  StateMachine &withFallback(const StateID &fallbackState) {
    std::lock_guard<std::recursive_mutex> lock(stateMutex);
    if (configChangeRejected("withFallback")) {
      return *this;
    }
    fallbackStateId = fallbackState;
    return *this;
  }

  /**
   * @brief Set logging level
   * @param level The logging level to use
   * @return StateMachine& This state machine for method chaining
   */
  StateMachine &withLogLevel(LogLevel level) {
    std::lock_guard<std::recursive_mutex> lock(stateMutex);
    logLevel.store(level);
    propagateLogConfig();
    return *this;
  }

  /**
   * @brief Route log records to a custom sink instead of std::cout
   *
   * Independent of withColors(): the sink receives colored text unless colors
   * are turned off, so installing one does not change how the output looks.
   *
   * In a ROS node, prefer the ready-made adapter:
   * @code
   *   sm.withLogSink(sm_ros::logSink(get_logger()));
   *   sm.withColors(false); // only if the destination must stay ANSI-free
   * @endcode
   *
   * @param sink Callable receiving (level, message); pass {} to restore stdout
   * @return StateMachine& This state machine for method chaining
   */
  StateMachine &withLogSink(LogSink sink) {
    std::lock_guard<std::recursive_mutex> lock(stateMutex);
    logSink = std::move(sink);
    propagateLogConfig();
    return *this;
  }

  /**
   * @brief Enable or disable ANSI colors, on stdout and on any sink alike
   *
   * Disable when the output is redirected to a file, captured by a GUI, or
   * published on a topic, so escape codes don't leak into it.
   *
   * @param enable True to emit ANSI codes
   * @return StateMachine& This state machine for method chaining
   */
  StateMachine &withColors(bool enable) {
    std::lock_guard<std::recursive_mutex> lock(stateMutex);
    useColors = enable;
    propagateLogConfig();
    return *this;
  }

  /**
   * @brief Supply the time source used for timeouts and history timestamps
   *
   * Defaults to steady_clock. ROS nodes should inject the node clock so state
   * timing follows simulated time:
   * @code  sm.withClock([this] { return this->now().seconds(); });  @endcode
   *
   * @param clock Callable returning monotonically increasing seconds
   * @return StateMachine& This state machine for method chaining
   */
  StateMachine &withClock(ClockFn clock) {
    std::lock_guard<std::recursive_mutex> lock(stateMutex);
    if (isInitialized) {
      double oldElapsed = clockNow() - stateEntryTimestamp;
      clockFn = std::move(clock);
      stateEntryTimestamp = clockNow() - oldElapsed;
    } else {
      clockFn = std::move(clock);
    }
    for (auto &entry : states) {
      for (auto &region : entry.second.regions) {
        if (region.machine) {
          region.machine->applyClock(clockFn);
        }
      }
    }
    return *this;
  }

  /**
   * @brief Cap on nested transitions triggered from within enter()/exit()
   *
   * Guards against infinite transition recursion. Raise it if the design has
   * legitimately long enter()-driven cascades.
   *
   * @param depth Maximum nesting depth (values < 1 are clamped to 1)
   * @return StateMachine& This state machine for method chaining
   */
  StateMachine &withMaxTransitionDepth(int depth) {
    std::lock_guard<std::recursive_mutex> lock(stateMutex);
    maxTransitionDepth = depth < 1 ? 1 : depth;
    return *this;
  }

  /**
   * @brief Also fire the state-change callback for the initial state on start()
   *
   * Off by default, so onStateChange only ever reports genuine changes
   * (from != to). Enable it when an observer is purely event-driven and would
   * otherwise never learn which state the machine started in; it will receive
   * one callback with from == to == the initial state.
   *
   * @param enable True to announce the initial state
   * @return StateMachine& This state machine for method chaining
   */
  StateMachine &withNotifyInitialState(bool enable = true) {
    std::lock_guard<std::recursive_mutex> lock(stateMutex);
    notifyInitialState = enable;
    return *this;
  }

  /**
   * @brief Reject (rather than merely warn about) configuration changes made
   *        after start(), and refuse to start an invalid configuration.
   *
   * @param enable True to enforce strict configuration
   * @return StateMachine& This state machine for method chaining
   */
  StateMachine &withStrictConfig(bool enable = true) {
    std::lock_guard<std::recursive_mutex> lock(stateMutex);
    strictConfig = enable;
    return *this;
  }

  /**
   * @brief Automatically leave a state if it stays active for too long
   *
   * Replaces the common hand-rolled "record entry time in enter(), compare in
   * update()" pattern. Evaluated once per update() cycle.
   *
   * @code  sm.withStateTimeout(Main::PREPARE, 10.0, Main::IDLE, "prep timeout");
   * @endcode
   *
   * @param id      The state to time out
   * @param seconds Timeout in seconds, measured with the machine's clock
   * @param target  State to transition to when the timeout expires
   * @param reason  Reason recorded for the timeout transition
   * @return StateMachine& This state machine for method chaining
   */
  StateMachine &withStateTimeout(const StateID &id, double seconds,
                                 const StateID &target,
                                 std::string reason = "State timeout") {
    std::lock_guard<std::recursive_mutex> lock(stateMutex);
    if (configChangeRejected("withStateTimeout")) {
      return *this;
    }
    auto it = states.find(id);
    if (it == states.end()) {
      log(LogLevel::ERROR, Color::RED,
          "withStateTimeout: state not found - add the state first");
      return *this;
    }
    if (seconds <= 0.0) {
      log(LogLevel::ERROR, Color::RED,
          "withStateTimeout: timeout must be positive");
      return *this;
    }
    it->second.timeout = StateTimeout{seconds, target, std::move(reason)};
    return *this;
  }

  /**
   * @brief Whitelist a legal transition
   *
   * While no transition has been whitelisted the machine is fully permissive.
   * Declaring even one transition enables enforcement for *every* state, so
   * declare the complete graph once you start. Fallback and rollback
   * transitions always bypass the whitelist so recovery cannot be locked out.
   *
   * @param from Source state
   * @param to   Permitted destination state
   * @return StateMachine& This state machine for method chaining
   */
  StateMachine &withAllowedTransition(const StateID &from, const StateID &to) {
    std::lock_guard<std::recursive_mutex> lock(stateMutex);
    if (configChangeRejected("withAllowedTransition")) {
      return *this;
    }
    allowedTransitions[from].insert(to);
    return *this;
  }

  /**
   * @brief Install a predicate consulted before every transition
   *
   * Returning false rejects the transition. Applied in addition to the
   * whitelist, and likewise bypassed for fallback/rollback.
   *
   * @param guard Callable (from, to) -> bool; pass {} to remove
   * @return StateMachine& This state machine for method chaining
   */
  StateMachine &
  withTransitionGuard(std::function<bool(const StateID &, const StateID &)> guard) {
    std::lock_guard<std::recursive_mutex> lock(stateMutex);
    if (configChangeRejected("withTransitionGuard")) {
      return *this;
    }
    transitionGuard = std::move(guard);
    return *this;
  }

  /**
   * @brief Set maximum history size
   * @param size Maximum number of states to keep in history
   * @return StateMachine& This state machine for method chaining
   */
  StateMachine &withHistorySize(size_t size) {
    std::lock_guard<std::recursive_mutex> lock(stateMutex);
    maxHistorySize = size;
    trimHistory();
    return *this;
  }

  /**
   * @brief Set state change callback
   * @param callback The function to call on state changes
   * @return StateMachine& This state machine for method chaining
   */
  StateMachine &onStateChanged(StateChangeCallback callback) {
    std::lock_guard<std::recursive_mutex> lock(stateMutex);
    onStateChange =
        std::make_shared<StateChangeCallback>(std::move(callback));
    return *this;
  }

  /**
   * @brief Set state update callback
   * @param callback The function to call after state updates
   * @return StateMachine& This state machine for method chaining
   */
  StateMachine &onStateUpdated(StateUpdateCallback callback) {
    std::lock_guard<std::recursive_mutex> lock(stateMutex);
    onStateUpdate =
        std::make_shared<StateUpdateCallback>(std::move(callback));
    return *this;
  }

  /**
   * @brief Set error callback
   * @param callback The function to call on errors
   * @return StateMachine& This state machine for method chaining
   */
  StateMachine &onError(ErrorCallback callback) {
    std::lock_guard<std::recursive_mutex> lock(stateMutex);
    onErrorCallback = std::make_shared<ErrorCallback>(std::move(callback));
    return *this;
  }

  /**
   * @brief Override the initial state (must be called before start())
   * @param stateId The state ID to use as the new initial state
   */
  void setInitialState(const StateID &stateId) {
    std::lock_guard<std::recursive_mutex> lock(stateMutex);
    if (isInitialized) {
      log(LogLevel::WARN, Color::YELLOW,
          "Cannot change initial state after start");
      return;
    }
    initialStateId = stateId;
    currentStateId = stateId;
    lastCommittedStateId = stateId;
  }

  /**
   * @brief Initialize and start the state machine
   * @return StateMachine& This state machine for method chaining
   */
  StateMachine &start() {
    std::lock_guard<std::recursive_mutex> lock(stateMutex);

    if (isInitialized) {
      log(LogLevel::WARN, Color::YELLOW, "State machine already initialized");
      return *this;
    }

    // Surface configuration problems (missing fallback, dangling timeout
    // targets, invalid children) before they can bite at runtime.
    if (!validateLocked() && strictConfig) {
      log(LogLevel::ERROR, Color::RED,
          "Refusing to start: invalid configuration (strict mode)");
      return *this;
    }

    auto it = states.find(currentStateId);
    if (it == states.end()) {
      log(LogLevel::ERROR, Color::RED, "Initial state not found");
      return *this;
    }

    const std::string stateName = it->second.name;
    const StateID startStateId = currentStateId;

    // Mark initialized before enter() to allow transitions within enter()
    isInitialized = true;
    commitEntry(startStateId, "State machine started");
    const std::uint64_t seqAtCommit = transitionSeq;

    if (!safeEnter(it->second.state.get())) {
      if (transitionSeq != seqAtCommit) {
        // enter() moved us elsewhere before failing; that nested transition
        // committed, so stay there instead of unwinding on top of it.
        log(LogLevel::WARN, Color::YELLOW,
            "Initial enter() failed after a nested transition - keeping the "
            "nested target");
        return *this;
      }
      rollbackHistory(startStateId);
      guardedExit(it->second.state.get(), "initial enter failure");

      // Attempt fallback recovery before giving up.
      const bool fallbackRegistered =
          fallbackStateId.has_value() &&
          states.find(fallbackStateId.value()) != states.end() &&
          fallbackStateId.value() != startStateId;

      if (fallbackRegistered) {
        log(LogLevel::WARN, Color::YELLOW,
            "Initial state enter failed, falling back");
        auto fbIt = states.find(fallbackStateId.value());
        commitEntry(fallbackStateId.value(), "Fallback after initial failure");
        const std::uint64_t seqAtFallback = transitionSeq;
        if (safeEnter(fbIt->second.state.get()) &&
            startRegions(fbIt->second, "fallback start")) {
          lastCommittedStateId = fallbackStateId.value();
          trimHistory();
          notifyStateChange(startStateId, fallbackStateId.value(), stateName,
                            fbIt->second.name, "Fallback after initial failure");
          reportError("Initial state enter failed - fell back");
          return *this;
        }
        // Fallback also failed
        rollbackHistory(fallbackStateId.value());
        stopRegions(fbIt->second, "fallback failure");
        guardedExit(fbIt->second.state.get(), "fallback failure");
        if (transitionSeq != seqAtFallback) {
          isInitialized = false;
          reportError("Initial state and fallback both failed");
          return *this;
        }
      }

      isInitialized = false;
      log(LogLevel::ERROR, Color::RED,
          "Failed to start state machine - initial state enter failed");
      reportError("Initial state enter failed");
      return *this;
    }

    if (transitionSeq != seqAtCommit) {
      if (!isInitialized) {
        log(LogLevel::WARN, Color::YELLOW,
            "State machine was stopped from initial enter()");
        return *this;
      }
      // enter() transitioned away; the nested transition already started the
      // right child sub-machine and notified observers.
      log(LogLevel::INFO, Color::GREEN,
          std::string("State machine started in state: ") + stateName +
              " (immediately advanced by enter())");
      return *this;
    }

    if (!startRegions(it->second, "initial start")) {
      rollbackHistory(startStateId);
      guardedExit(it->second.state.get(), "initial region failure");
      isInitialized = false;
      log(LogLevel::ERROR, Color::RED,
          "Failed to start state machine - required region failed");
      return *this;
    }

    lastCommittedStateId = startStateId;
    trimHistory();

    log(LogLevel::INFO, Color::GREEN,
        std::string("State machine started in state: ") + stateName);

    // Opt-in: let purely event-driven observers learn about the initial state,
    // which they would otherwise never be told about.
    if (notifyInitialState) {
      notifyStateChange(startStateId, startStateId, stateName, stateName,
                        "State machine started");
    }
    return *this;
  }

  /**
   * @brief Update the state machine (call this in your main loop)
   *
   * Nothing below runs while holding stateMutex, so a slow state update, child
   * update or observer callback never blocks a changeState() from another
   * thread.
   */
  void update() {
    // Snapshot the active state under the lock, then release before calling
    // user code.
    std::shared_ptr<State> stateSnapshot;
    {
      std::lock_guard<std::recursive_mutex> lock(stateMutex);
      if (!isInitialized) {
        return;
      }
      auto it = states.find(currentStateId);
      if (it == states.end()) {
        return;
      }
      stateSnapshot = it->second.state;
      // Reference point for State::shouldCancelUpdate(): any transition that
      // commits from here on makes this update() cycle's work stale.
      updateEpoch.store(transitionSeq.load());
    }

    // State::update() runs without the machine lock.
    // Re-entrant changeState() calls from within update() acquire their own
    // lock.
    if (stateSnapshot) {
      try {
        stateSnapshot->update();
      } catch (const std::exception &e) {
        log(LogLevel::ERROR, Color::RED,
            std::string("Exception in state update: ") + e.what());
      } catch (...) {
        log(LogLevel::ERROR, Color::RED, "Unknown exception in state update");
      }
    }
    stateSnapshot.reset();

    // Re-snapshot: currentStateId may have changed during update(). Raw
    // ISubMachine* stays valid because states are never erased and
    // unordered_map rehashing does not invalidate element addresses.
    std::vector<ISubMachine *> activeRegions;
    StateID activeId{};
    std::string activeName;
    std::optional<StateTimeout> timeout;
    double elapsed = 0.0;
    std::uint64_t activeSeq = 0;
    {
      std::lock_guard<std::recursive_mutex> lock(stateMutex);
      if (!isInitialized) {
        return;
      }
      auto it = states.find(currentStateId);
      if (it == states.end()) {
        return;
      }
      activeId = currentStateId;
      activeName = it->second.name;
      timeout = it->second.timeout;
      elapsed = clockNow() - stateEntryTimestamp;
      activeSeq = transitionSeq;
      activeRegions.reserve(it->second.regions.size());
      for (auto &region : it->second.regions) {
        if (region.machine && region.machine->isActive()) {
          activeRegions.push_back(region.machine.get());
        }
      }
    }

    // Update orthogonal regions in deterministic registration order.
    for (ISubMachine *region : activeRegions) {
      safeChild(region, &ISubMachine::update, "region update");
    }

    if (timeout.has_value() && elapsed >= timeout->seconds &&
        timeout->target != activeId) {
      std::lock_guard<std::recursive_mutex> lock(stateMutex);
      // Sequence comparison prevents both a simple stale timeout and the
      // A->B->A ABA case, where the ID matches but the entry is newer.
      if (isInitialized && currentStateId == activeId &&
          transitionSeq == activeSeq) {
        const double currentElapsed = clockNow() - stateEntryTimestamp;
        if (currentElapsed >= timeout->seconds) {
          changeStateInternal(
              timeout->target,
              timeout->reason + " (" + std::to_string(currentElapsed) +
                  "s in " + activeName + ")");
        }
      }
    }

    // Re-snapshot after region updates and timeout handling, both of which may
    // have changed the state.
    StateID callbackId{};
    std::string callbackName;
    std::shared_ptr<StateUpdateCallback> updateCallback;
    {
      std::lock_guard<std::recursive_mutex> lock(stateMutex);
      if (!isInitialized) {
        return;
      }
      auto it = states.find(currentStateId);
      if (it == states.end()) {
        return;
      }
      callbackId = currentStateId;
      callbackName = it->second.name;
      updateCallback = onStateUpdate;
    }

    // Call update callback with the state active at callback time.
    if (updateCallback && *updateCallback) {
      try {
        (*updateCallback)(callbackId, callbackName);
      } catch (const std::exception &e) {
        log(LogLevel::ERROR, Color::RED,
            std::string("Exception in state update callback: ") + e.what());
      } catch (...) {
        log(LogLevel::ERROR, Color::RED,
            "Unknown exception in state update callback");
      }
    }
  }

  /**
   * @brief Change the current state (thread-safe)
   * @param newStateId The ID of the state to transition to
   * @param reason Optional reason for the transition
   * @return bool True if the state change was successful
   */
  bool changeState(const StateID &newStateId, std::string reason = "") {
    std::lock_guard<std::recursive_mutex> lock(stateMutex);
    bool result = changeStateInternal(newStateId, std::move(reason));
    if (transitionDepth == 0) {
      executePendingExitTransitions();
    }
    return result;
  }

  /**
   * @brief Change state only if the machine is currently in `expectedFrom`
   *
   * Atomic compare-and-transition. Use instead of the racy
   * `if (getCurrentStateId() == A) changeState(B)` pattern, which can be
   * interleaved by another thread between the read and the write.
   *
   * @param expectedFrom State the machine must currently be in
   * @param newStateId   Destination state
   * @param reason       Optional reason for the transition
   * @return bool True if the machine was in expectedFrom and the change applied
   */
  bool changeStateIf(const StateID &expectedFrom, const StateID &newStateId,
                     std::string reason = "") {
    std::lock_guard<std::recursive_mutex> lock(stateMutex);
    if (currentStateId != expectedFrom) {
      return false;
    }
    bool result = changeStateInternal(newStateId, std::move(reason));
    if (transitionDepth == 0) {
      executePendingExitTransitions();
    }
    return result;
  }

  /**
   * @brief Exit and re-enter the active state
   *
   * changeState() to the active state is a no-op by design; use this when the
   * intent really is to restart the state.
   *
   * @param reason Optional reason for the re-entry
   * @return bool True if the state was successfully re-entered
   */
  bool reenterState(std::string reason = "Re-enter state") {
    std::lock_guard<std::recursive_mutex> lock(stateMutex);
    bool result = changeStateInternal(currentStateId, std::move(reason),
                               /*bypassGuard=*/true, /*forceReenter=*/true);
    if (transitionDepth == 0) {
      executePendingExitTransitions();
    }
    return result;
  }

  /**
   * @brief Stop the state machine, exiting the current state
   *
   * Exits the active state (and its child SM if any), then marks the
   * machine as not initialized. Safe to call when already stopped.
   */
  void stop() {
    stopThread();
    std::lock_guard<std::recursive_mutex> lock(stateMutex);
    if (!isInitialized) {
      return;
    }
    if (stateExitInProgress || regionLifecycleInProgress || stopInProgress) {
      log(LogLevel::WARN, Color::YELLOW,
          "Rejected stop requested during state teardown");
      return;
    }

    stopInProgress = true;
    // Publish the stopped lifecycle state before user teardown hooks run, so
    // they cannot start a new transition underneath stop().
    isInitialized = false;
    ++transitionSeq;

    auto it = states.find(currentStateId);
    if (it != states.end()) {
      // Bottom-up teardown: child sub-machine first, then the state
      stopRegions(it->second, "stop");
      guardedExit(it->second.state.get(), "stop");
    }

    pendingExitTransition.reset();
    currentStateId = initialStateId;
    lastCommittedStateId = initialStateId;
    stopInProgress = false;
    log(LogLevel::INFO, Color::BLUE, "State machine stopped");
  }

  /**
   * @brief Get current state ID (thread-safe)
   * @return StateID The current state ID
   */
  StateID getCurrentStateId() const {
    std::lock_guard<std::recursive_mutex> lock(stateMutex);
    return currentStateId;
  }

  /**
   * @brief Get current state name (thread-safe)
   * @return std::string The name of the current state
   */
  std::string getCurrentStateName() const {
    std::lock_guard<std::recursive_mutex> lock(stateMutex);
    auto it = states.find(currentStateId);
    return (it != states.end()) ? it->second.name : "Unknown";
  }

  /**
   * @brief Get current state instance (thread-safe)
   *
   * Returns a shared_ptr rather than a raw pointer: the lock is released on
   * return, so ownership must outlive the call.
   *
   * @return std::shared_ptr<State> The current state object, or nullptr
   */
  std::shared_ptr<State> getCurrentState() const {
    std::lock_guard<std::recursive_mutex> lock(stateMutex);
    auto it = states.find(currentStateId);
    return (it != states.end()) ? it->second.state : nullptr;
  }

  /**
   * @brief Check if state exists
   * @param id The state ID to check
   * @return bool True if the state exists
   */
  bool hasState(const StateID &id) const {
    std::lock_guard<std::recursive_mutex> lock(stateMutex);
    return states.find(id) != states.end();
  }

  /**
   * @brief Current reading of the machine's clock source
   * @return double Seconds, per the injected clock (default steady_clock)
   */
  double now() const {
    std::lock_guard<std::recursive_mutex> lock(stateMutex);
    return clockNow();
  }

  /**
   * @brief Clock reading captured when the active state was entered
   */
  double stateEntryTime() const {
    std::lock_guard<std::recursive_mutex> lock(stateMutex);
    return stateEntryTimestamp;
  }

  /**
   * @brief Seconds the machine has spent in the active state
   */
  double timeInState() const {
    std::lock_guard<std::recursive_mutex> lock(stateMutex);
    return clockNow() - stateEntryTimestamp;
  }

  /**
   * @brief Monotonic counter of committed transitions
   *
   * Bumped by every committed transition, re-entry, rollback and by stop().
   * Deliberately lock-free so it can be polled from a long-running
   * State::update(), which runs without the machine lock.
   *
   * @return std::uint64_t Opaque epoch; only ever compare it for equality
   */
  std::uint64_t transitionEpoch() const { return transitionSeq.load(); }

  /**
   * @brief Whether no transition has committed since `epoch` was captured
   *
   * The thread-safe way for long-running work to check that it has not been
   * superseded. Unlike comparing getCurrentStateId(), this also catches
   * A->B->A round trips, state re-entry and stop().
   *
   * @param epoch A value previously returned by transitionEpoch()
   * @return bool True if the machine has not transitioned since
   */
  bool stillActive(std::uint64_t epoch) const {
    return transitionSeq.load() == epoch;
  }

  /**
   * @brief Whether a transition committed since the current update() dispatched
   *
   * Backs State::shouldCancelUpdate(). Only meaningful during an update()
   * cycle; outside one the comparison is against a stale epoch.
   *
   * @return bool True if in-flight update work should be abandoned
   */
  bool isUpdateCancellationRequested() const {
    return transitionSeq.load() != updateEpoch.load();
  }

  /**
   * @brief Get state history
   * @return std::vector<StateID> Copy of the visited state IDs, oldest first
   */
  std::vector<StateID> getStateHistory() const {
    std::lock_guard<std::recursive_mutex> lock(stateMutex);
    std::vector<StateID> ids;
    ids.reserve(stateHistory.size());
    for (const auto &record : stateHistory) {
      ids.push_back(record.state);
    }
    return ids;
  }

  /**
   * @brief Get the full transition history with timestamps and reasons
   *
   * Richer counterpart to getStateHistory(), intended for post-flight
   * debriefing and event logs.
   *
   * @return std::vector<TransitionRecord> Copy of the history, oldest first
   */
  std::vector<TransitionRecord> getTransitionHistory() const {
    std::lock_guard<std::recursive_mutex> lock(stateMutex);
    return std::vector<TransitionRecord>(stateHistory.begin(),
                                         stateHistory.end());
  }

  /**
   * @brief Reset the state machine to its initial state
   *
   * Bypasses the transition whitelist and forces a re-entry, so it works even
   * when the machine is already sitting in the initial state.
   *
   * @return bool True if the reset was successful
   */
  bool reset() {
    std::lock_guard<std::recursive_mutex> lock(stateMutex);
    bool result = changeStateInternal(initialStateId, "Reset to initial state",
                               /*bypassGuard=*/true, /*forceReenter=*/true);
    if (transitionDepth == 0) {
      executePendingExitTransitions();
    }
    return result;
  }

  /**
   * @brief Validate state machine configuration
   * @return bool True if configuration is valid
   */
  bool validate() const {
    std::lock_guard<std::recursive_mutex> lock(stateMutex);
    return validateLocked();
  }

  /**
   * @brief Get the number of registered states
   * @return size_t Number of states
   */
  size_t getStateCount() const {
    std::lock_guard<std::recursive_mutex> lock(stateMutex);
    return states.size();
  }

  /**
   * @brief Check if the state machine is initialized
   * @return bool True if initialized
   */
  bool isReady() const {
    std::lock_guard<std::recursive_mutex> lock(stateMutex);
    return isInitialized;
  }

  // ──────────────────────────────────────────────────────────────────
  //  Convenience helpers
  // ──────────────────────────────────────────────────────────────────

  /**
   * @brief Check if the machine is currently in the given state
   *
   * Shorthand for getCurrentStateId() == id. Thread-safe.
   *
   * @code  if (sm.isInState(DroneState::IDLE)) { ... }  @endcode
   */
  bool isInState(const StateID &id) const {
    std::lock_guard<std::recursive_mutex> lock(stateMutex);
    return currentStateId == id;
  }

  /**
   * @brief Get the name of any registered state by ID
   *
   * Unlike getCurrentStateName(), this works for any registered state,
   * not just the active one.
   */
  std::string getStateName(const StateID &id) const {
    std::lock_guard<std::recursive_mutex> lock(stateMutex);
    auto it = states.find(id);
    return it != states.end() ? it->second.name : "Unknown";
  }

  /**
   * @brief Get all registered state IDs
   *
   * Returned in unspecified order. Useful for introspection and debugging.
   */
  std::vector<StateID> getRegisteredStateIds() const {
    std::lock_guard<std::recursive_mutex> lock(stateMutex);
    std::vector<StateID> ids;
    ids.reserve(states.size());
    for (const auto &entry : states) {
      ids.push_back(entry.first);
    }
    return ids;
  }

  /**
   * @brief Get the machine's name
   */
  std::string getMachineName() const {
    std::lock_guard<std::recursive_mutex> lock(stateMutex);
    return machineName;
  }

  /**
   * @brief Get the initial state ID
   */
  StateID getInitialStateId() const {
    std::lock_guard<std::recursive_mutex> lock(stateMutex);
    return initialStateId;
  }

  /**
   * @brief Check if a fallback state is configured
   */
  bool hasFallback() const {
    std::lock_guard<std::recursive_mutex> lock(stateMutex);
    return fallbackStateId.has_value();
  }

  /**
   * @brief Get the configured fallback state ID, if any
   */
  std::optional<StateID> getFallbackStateId() const {
    std::lock_guard<std::recursive_mutex> lock(stateMutex);
    return fallbackStateId;
  }

  /**
   * @brief Whether the machine is stopped (inverse of isReady())
   */
  bool isStopped() const {
    std::lock_guard<std::recursive_mutex> lock(stateMutex);
    return !isInitialized;
  }

  /**
   * @brief Number of entries currently in the transition history
   */
  size_t getHistorySize() const {
    std::lock_guard<std::recursive_mutex> lock(stateMutex);
    return stateHistory.size();
  }

  /**
   * @brief Clear the transition history without resetting the machine
   *
   * Useful when reusing a long-running machine for a new mission and the old
   * history is no longer relevant.
   */
  void clearHistory() {
    std::lock_guard<std::recursive_mutex> lock(stateMutex);
    stateHistory.clear();
  }

  /**
   * @brief Log a message through the machine's logging infrastructure
   *
   * Color is auto-selected based on the log level. Use this from application
   * code (or from State subclasses via State::log()) to emit messages that
   * respect withLogLevel() and withLogSink().
   *
   * @code  sm.log(LogLevel::INFO, "Mission started");  @endcode
   */
  void log(LogLevel level, std::string_view message) const {
    Color color = Color::BLUE;
    switch (level) {
    case LogLevel::ERROR: color = Color::RED; break;
    case LogLevel::WARN:  color = Color::YELLOW; break;
    case LogLevel::INFO:  color = Color::GREEN; break;
    case LogLevel::DEBUG: color = Color::CYAN; break;
    default: break;
    }
    log(level, color, message);
  }

  /**
   * @brief Repeatedly call update() until the machine reaches the given state
   *
   * Convenience for the common "run until done" loop that appears in most
   * examples and integration code. Returns true if the target state was
   * reached, false if maxUpdates was hit first.
   *
   * @param targetState   State to wait for
   * @param maxUpdates    Maximum number of update() calls (0 = unlimited)
   * @param sleepMs       Milliseconds to sleep between updates (0 = no sleep)
   * @return bool True if the target state was reached
   *
   * @code
   * sm.start();
   * if (!sm.runUntil(DroneState::MISSION_COMPLETE, 200, 50)) { ... }
   * @endcode
   */
  bool runUntil(const StateID &targetState, size_t maxUpdates = 1000,
                unsigned sleepMs = 0) {
    for (size_t i = 0; maxUpdates == 0 || i < maxUpdates; ++i) {
      update();
      if (getCurrentStateId() == targetState) {
        return true;
      }
      if (sleepMs > 0) {
        std::this_thread::sleep_for(std::chrono::milliseconds(sleepMs));
      }
    }
    return false;
  }

  /**
   * @brief Run the state machine for a fixed wall-clock duration
   *
   * Calls update() in a loop until the given number of seconds elapses.
   * Uses steady_clock regardless of the machine's injected clock so the
   * duration is always wall-clock time.
   *
   * @param seconds           Duration to run for
   * @param updateIntervalMs  Milliseconds between update() calls (0 = tight loop)
   *
   * @code  sm.runFor(5.0, 100);  // run for 5 seconds at 10 Hz  @endcode
   */
  void runFor(double seconds, unsigned updateIntervalMs = 100) {
    const double start = steadyClockSeconds();
    while (steadyClockSeconds() - start < seconds) {
      update();
      if (updateIntervalMs > 0) {
        std::this_thread::sleep_for(
            std::chrono::milliseconds(updateIntervalMs));
      }
    }
  }

  /**
   * @brief Start calling update() on a background thread at a fixed rate
   *
   * Spawns a dedicated thread that calls update() every dt seconds. The
   * thread is joined automatically by stop() and the destructor, so callers
   * can also simply let the machine go out of scope.
   *
   * Must be called after start(). If a run-thread is already active, this
   * is a no-op.
   *
   * @param dt  Update period in seconds (e.g. 0.1 for 10 Hz)
   *
   * @code
   *   sm.start();
   *   sm.runInThread(0.02);  // 50 Hz in the background
   *   // ... do other work ...
   *   sm.stop();              // joins the thread automatically
   * @endcode
   */
  void runInThread(double dt) {
    if (runThreadRunning.load()) {
      return;
    }
    if (dt <= 0.0) {
      dt = 0.1; // default to 10 Hz
    }
    runThreadRunning.store(true);
    runThread = std::thread([this, dt]() {
      using clock = std::chrono::steady_clock;
      auto period = std::chrono::duration_cast<clock::duration>(
          std::chrono::duration<double>(dt));
      auto next = clock::now();
      while (runThreadRunning.load()) {
        update();
        next += period;
        std::this_thread::sleep_until(next);
      }
    });
  }

  /**
   * @brief Stop the background update thread if one is running
   *
   * Called automatically by stop() and the destructor. Safe to call when
   * no thread is active.
   */
  void stopThread() {
    if (!runThreadRunning.load()) {
      return;
    }
    runThreadRunning.store(false);
    if (runThread.joinable()) {
      runThread.join();
    }
  }
};

// ──────────────────────────────────────────────────────────────────────
//  sm::make — free helper for bottom-up sub-machine construction
// ──────────────────────────────────────────────────────────────────────

/**
 * @brief Namespace for state machine construction helpers
 */
namespace sm {
/**
 * @brief Create a heap-allocated StateMachine for use with attachSub().
 *
 * Returns a unique_ptr so the machine can be configured fluently and then
 * moved into a parent via attachSub(). This avoids the nested-lambda and
 * .template syntax required by withSubStates().
 *
 * @code
 *   auto core = sm::make<CoreState>(CoreState::INIT, "Core");
 *   core->addState<CoreInitState>(CoreState::INIT, "Init")
 *       .addState<CoreActiveState>(CoreState::ACTIVE, "Active");
 *
 *   system.attachSub(Main::RUNNING, std::move(core));
 * @endcode
 */
template <typename StateID>
std::unique_ptr<StateMachine<StateID>> make(StateID initial,
                                            std::string name = "SubMachine") {
  return std::make_unique<StateMachine<StateID>>(initial, std::move(name));
}
} // namespace sm

#endif // ROS_COMMON2_STATE_MACHINE_HPP
