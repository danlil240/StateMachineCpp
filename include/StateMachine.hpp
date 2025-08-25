#ifndef STATE_MACHINE_HPP
#define STATE_MACHINE_HPP

#include "StateMachineConfig.hpp"
#include "StateMachineTypes.hpp"
#include <atomic>
#include <memory>
#include <mutex>
#include <stdexcept>
#include <string>
#include <type_traits>
#include <unordered_map>
#include <vector>

/**
 * @brief A high-performance, thread-safe state machine implementation with fluent API
 *
 * Features:
 * - Type-safe states via enum class
 * - Thread-safe operations with minimal locking
 * - Fluent API with method chaining
 * - Customizable state transitions with callbacks
 * - Type-safe context objects with validation
 * - Error recovery with fallback states
 * - State history tracking
 * - Configurable logging
 * - Modular design with pluggable components
 *
 * @tparam StateID The type used for state identifiers (typically enum class)
 */
template<typename StateID>
class StateMachine
{
public:
    using State = StateMachineTypes::State<StateID>;
    using LogLevel = StateMachineTypes::LogLevel;
    using StateChangeCallback = StateMachineTypes::StateChangeCallback<StateID>;
    using StateUpdateCallback = StateMachineTypes::StateUpdateCallback<StateID>;
    using ErrorCallback = StateMachineTypes::ErrorCallback<StateID>;

private:
    using StateInfo = StateMachineTypes::StateInfo<StateID>;

    // Core state machine data
    std::unordered_map<StateID, StateInfo> states;
    std::atomic<StateID> currentStateId;
    std::atomic<bool> isInitialized{false};
    std::atomic<uint64_t> stateVersion{0}; // Incremented on each state change

    // Configuration
    std::unique_ptr<StateMachineConfig<StateID>> config;

    // History tracking
    std::vector<StateID> stateHistory;

    // Transition queuing system
    struct PendingTransition
    {
        StateID targetStateId;
        std::string reason;
        PendingTransition(const StateID &id, std::string_view r) : targetStateId(id), reason(r) {}
    };
    std::vector<PendingTransition> pendingTransitions;
    std::atomic<bool> updateInProgress{false};
    std::atomic<bool> updateShouldCancel{false}; // Signal to cancel ongoing update

    // Thread safety - hierarchical locking to prevent deadlocks
    // Lock order: statesMutex -> currentStateMutex -> historyMutex -> configMutex
    mutable std::mutex statesMutex;          // Protects states map (Level 1)
    mutable std::mutex currentStateMutex;    // Protects currentStateId (Level 2)
    mutable std::mutex historyMutex;         // Protects stateHistory (Level 3)
    mutable std::mutex configMutex;          // Protects config operations (Level 4)
    mutable std::mutex updateMutex;          // Protects update execution (Independent)
    mutable std::mutex transitionQueueMutex; // Protects pendingTransitions (Independent)

    // RAII guard for safe state access outside locks
    struct StateGuard
    {
        std::shared_ptr<State> state;
        StateID stateId;
        std::string stateName;

        StateGuard(std::shared_ptr<State> s, StateID id, std::string name)
            : state(std::move(s)), stateId(id), stateName(std::move(name))
        {
        }

        State* get() const { return state.get(); }
        State* operator->() const { return state.get(); }
        State &operator*() const { return *state; }
        explicit operator bool() const { return state != nullptr; }
    };

    // Get a safe state guard for any state by ID
    StateGuard getStateGuard(const StateID &stateId) const
    {
        std::lock_guard<std::mutex> lock(statesMutex);
        auto it = states.find(stateId);
        if (it != states.end())
        {
            return StateGuard(it->second.state, stateId, it->second.name);
        }
        return StateGuard(nullptr, StateID{}, "");
    }

    // Private helper methods (implemented inline for template)
    void log(LogLevel level, std::string_view message) const
    {
        // Config access is safe - config is immutable after construction
        // and logger/logLevel are thread-safe to read
        if (auto* logger = config->getLogger(); logger && StateMachineLogger::shouldLog(config->getLogLevel(), level))
        {
            logger->log(level, config->getMachineName(), message);
        }
    }

    void addToHistory(const StateID &stateId)
    {
        // Note: This method should only be called when historyMutex is already held
        // to maintain lock hierarchy and prevent races
        if (stateHistory.empty() || stateHistory.back() != stateId)
        {
            stateHistory.push_back(stateId);
        }
        // Safe config access - config is immutable after construction
        if (stateHistory.size() > config->getMaxHistorySize())
        {
            stateHistory.erase(stateHistory.begin());
        }
    }

    bool handleTransitionFailure(const StateID &failedState, std::string_view reason)
    {
        // Config access is thread-safe for reads after construction
        const auto& fallbackStateId = config->getFallbackStateId();
        if (fallbackStateId.has_value() && fallbackStateId.value() != failedState)
        {
            log(LogLevel::WARN, std::string("State transition failed, falling back: ") + std::string(reason));

            // Check if we're in an update cycle - if so, queue it; otherwise execute immediately
            if (updateInProgress.load())
            {
                queueTransition(fallbackStateId.value(), "Fallback after failure", true); // Cancel update for fallback
                return true;                                                              // Will be processed later
            }
            else
            {
                // Execute fallback immediately
                return changeStateInternal(fallbackStateId.value(), "Fallback after failure");
            }
        }

        log(LogLevel::ERROR, std::string("State transition failed with no fallback: ") + std::string(reason));
        // Config access is thread-safe for reads after construction
        if (auto cb = config->getErrorCallback(); cb)
        {
            cb(reason, currentStateId.load());
        }
        return false;
    }

    void queueTransition(const StateID &stateId, std::string_view reason, bool cancelUpdate = false)
    {
        std::lock_guard<std::mutex> lock(transitionQueueMutex);
        pendingTransitions.emplace_back(stateId, reason);

        if (cancelUpdate)
        {
            updateShouldCancel.store(true); // Signal running update to cancel
            log(LogLevel::DEBUG, std::string("Queued transition to state (cancelling update): ") + std::string(reason));
        }
        else
        {
            log(LogLevel::DEBUG,
                std::string("Queued transition to state (waiting for update): ") + std::string(reason));
        }
    }

    void processPendingTransitions()
    {
        std::vector<PendingTransition> transitionsToProcess;

        // Move pending transitions to local vector
        {
            std::lock_guard<std::mutex> lock(transitionQueueMutex);
            if (pendingTransitions.empty())
            {
                return;
            }
            transitionsToProcess = std::move(pendingTransitions);
            pendingTransitions.clear();
        }

        // Process transitions outside of the queue lock
        for (const auto &transition : transitionsToProcess)
        {
            log(LogLevel::DEBUG, std::string("Processing queued transition: ") + transition.reason);
            changeStateInternal(transition.targetStateId, transition.reason);
            // Only process the first valid transition to avoid cascading changes
            break;
        }
    }

    bool changeStateInternal(const StateID &newStateId, std::string_view reason)
    {
        StateGuard oldStateGuard(nullptr, StateID{}, "");
        StateGuard newStateGuard(nullptr, StateID{}, "");
        StateID oldStateId;
        StateChangeCallback stateChangeCb; // local copies of callbacks

        {
            std::lock_guard<std::mutex> lock(statesMutex);

            if (!isInitialized.load())
            {
                log(LogLevel::WARN, "Attempted state change before initialization");
                return false;
            }

            auto oldStateIter = states.find(currentStateId.load());
            auto newStateIter = states.find(newStateId);

            if (oldStateIter == states.end() || newStateIter == states.end())
            {
                log(LogLevel::ERROR, "State transition between invalid states");
                return false;
            }

            // No need to change to the same state
            if (currentStateId.load() == newStateId)
            {
                log(LogLevel::DEBUG, "Ignored transition to same state");
                return true;
            }

            // Create safe state guards
            oldStateId = currentStateId.load();
            oldStateGuard = StateGuard(oldStateIter->second.state, oldStateId, oldStateIter->second.name);
            newStateGuard = StateGuard(newStateIter->second.state, newStateId, newStateIter->second.name);

            // Copy callback to local variable (config is thread-safe for reads after construction)
            stateChangeCb = config->getStateChangeCallback();
        }

        // ---- OUTSIDE LOCK ----

        // Log state change
        std::string reasonText = reason.empty() ? "" : std::string("\t(Reason: ") + std::string(reason) + ")";
        log(LogLevel::INFO, "|" + std::string(StateMachineLogger::STRIKETHROUGH) + oldStateGuard.stateName +
                                std::string(StateMachineLogger::RESET) + std::string(StateMachineLogger::BOLD) +
                                std::string(StateMachineLogger::BLUE) + "| ➔ |" + newStateGuard.stateName + "|" +
                                reasonText);

        // Exit old state using safe guard
        try
        {
            if (oldStateGuard)
                oldStateGuard->exit();
        }
        catch (const std::exception& e)
        {
            log(LogLevel::ERROR, std::string("Exception in state exit: ") + e.what());
        }

        // Enter new state using safe guard
        bool enterSuccess = true;
        try
        {
            if (newStateGuard)
            {
                // Set the state ID so the state knows which state it represents
                newStateGuard->myStateId = newStateId;
                enterSuccess = newStateGuard->enter();
            }
        }
        catch (const std::exception& e)
        {
            log(LogLevel::ERROR, std::string("Exception in state enter: ") + e.what());
            enterSuccess = false;
        }

        if (!enterSuccess)
        {
            // Rollback - restore old state since enter() failed
            {
                std::lock_guard<std::mutex> lock(currentStateMutex);
                currentStateId = oldStateId;
            }
            return handleTransitionFailure(newStateId, "State enter() returned false");
        }

        // State transition successful - now update the state
        // Follow lock hierarchy: currentStateMutex -> historyMutex
        {
            std::lock_guard<std::mutex> currentLock(currentStateMutex);
            {
                std::lock_guard<std::mutex> historyLock(historyMutex);
                currentStateId = newStateId;
                stateVersion.fetch_add(1); // Increment version on state change
                addToHistory(newStateId);
            }
        }

        // Callback outside lock
        if (stateChangeCb)
        {
            try
            {
                stateChangeCb(oldStateId, newStateId, oldStateGuard.stateName, newStateGuard.stateName, reason);
            }
            catch (const std::exception& e)
            {
                log(LogLevel::ERROR, std::string("Exception in state change callback: ") + e.what());
            }
        }

        return true;
    }

public:
    /**
     * @brief Get a safe state guard for the current state
     * @return StateGuard RAII guard for safe state access
     */
    StateGuard getCurrentStateGuard() const
    {
        std::lock_guard<std::mutex> lock(statesMutex);
        auto it = states.find(currentStateId.load());
        if (it != states.end())
        {
            return StateGuard(it->second.state, currentStateId.load(), it->second.name);
        }
        return StateGuard(nullptr, StateID{}, "");
    }

    /**
     * @brief Create a state machine starting in the specified state
     * @param initialState The state ID to start in
     * @param name Name for this state machine instance
     */
    explicit StateMachine(const StateID& initialState, std::string name = "StateMachine")
        : currentStateId(initialState), config(std::make_unique<StateMachineConfig<StateID>>(initialState, std::move(name)))
    {
        static_assert(std::is_trivially_copyable_v<StateID>, "StateID must be trivially copyable");
        stateHistory.reserve(config->getMaxHistorySize());
    }

    // Disable copy constructor and assignment
    StateMachine(const StateMachine&) = delete;
    StateMachine& operator=(const StateMachine&) = delete;

    // Enable move constructor and assignment
    StateMachine(StateMachine&&) = default;
    StateMachine &operator=(StateMachine &&) = default;

    /**
     * @brief Add a state to the state machine
     * @tparam StateType The class implementing the state
     * @param id The unique state ID
     * @param name A human-readable name for the state
     * @return StateMachine& This state machine for method chaining
     */
    template <typename StateType>
    StateMachine& addState(const StateID& id, std::string_view name)
    {
        static_assert(std::is_base_of_v<State, StateType>, "StateType must inherit from State");

        std::lock_guard<std::mutex> lock(statesMutex);

        if (states.find(id) != states.end())
        {
            log(LogLevel::WARN, std::string("State ") + std::string(name) + " already exists, skipping...");
            return *this;
        }

        auto state = std::make_shared<StateType>();
        state->setStateMachine(this);
        states.emplace(id, StateInfo(std::string(name), state));

        log(LogLevel::DEBUG, std::string("Added state: ") + std::string(name));
        return *this;
    }

    /**
     * @brief Add a pre-configured state to the state machine
     * @param id The unique state ID
     * @param name A human-readable name for the state
     * @param state A pre-configured state instance
     * @return StateMachine& This state machine for method chaining
     */
    StateMachine &addState(const StateID &id, std::string_view name, std::unique_ptr<State> state)
    {
        std::lock_guard<std::mutex> lock(statesMutex);

        if (states.find(id) != states.end())
        {
            log(LogLevel::WARN, std::string("State ") + std::string(name) + " already exists, skipping...");
            return *this;
        }

        state->setStateMachine(this);
        states.emplace(id, StateInfo(std::string(name), std::move(state)));

        log(LogLevel::DEBUG, std::string("Added state: ") + std::string(name));
        return *this;
    }

    /**
     * @brief Set the user context object with type safety
     * @tparam T Type of the context object
     * @param context Shared pointer to the context object
     * @return StateMachine& This state machine for method chaining
     */
    template <typename T>
    StateMachine& withContext(std::shared_ptr<T> context)
    {
        config->withContext(context);
        return *this;
    }

    /**
     * @brief Get the user context with type safety
     * @tparam T The type to cast the context to
     * @return std::shared_ptr<T> Shared pointer to the context object
     * @throws std::runtime_error if context type doesn't match
     */
    template <typename T>
    std::shared_ptr<T> getContext() const
    {
        return config->template getContext<T>();
    }

    /**
     * @brief Set fallback state for error recovery
     * @param fallbackState The state to fall back to on errors
     * @return StateMachine& This state machine for method chaining
     */
    StateMachine &withFallback(const StateID &fallbackState)
    {
        config->withFallback(fallbackState);
        return *this;
    }

    /**
     * @brief Set logging level
     * @param level The logging level to use
     * @return StateMachine& This state machine for method chaining
     */
    StateMachine &withLogLevel(LogLevel level)
    {
        config->withLogLevel(level);
        return *this;
    }

    /**
     * @brief Set custom logger
     * @param logger Custom logger implementation
     * @return StateMachine& This state machine for method chaining
     */
    StateMachine &withLogger(std::unique_ptr<StateMachineLogger::ILogger> logger)
    {
        config->withLogger(std::move(logger));
        return *this;
    }

    /**
     * @brief Set maximum history size
     * @param size Maximum number of states to keep in history
     * @return StateMachine& This state machine for method chaining
     */
    StateMachine &withHistorySize(size_t size)
    {
        config->withHistorySize(size);
        return *this;
    }

    /**
     * @brief Set state change callback
     * @param callback The function to call on state changes
     * @return StateMachine& This state machine for method chaining
     */
    StateMachine &onStateChanged(StateChangeCallback callback)
    {
        config->onStateChanged(std::move(callback));
        return *this;
    }

    /**
     * @brief Set state update callback
     * @param callback The function to call after state updates
     * @return StateMachine& This state machine for method chaining
     */
    StateMachine &onStateUpdated(StateUpdateCallback callback)
    {
        config->onStateUpdated(std::move(callback));
        return *this;
    }

    /**
     * @brief Set error callback
     * @param callback The function to call on errors
     * @return StateMachine& This state machine for method chaining
     */
    StateMachine &onError(ErrorCallback callback)
    {
        config->onError(std::move(callback));
        return *this;
    }

    /**
     * @brief Initialize and start the state machine
     * @return StateMachine& This state machine for method chaining
     */
    StateMachine &start()
    {
        // Gather state under lock, but call enter() outside to avoid deadlocks
        State* state = nullptr;
        std::string stateName;
        {
            std::lock_guard<std::mutex> lock(statesMutex);
            if (isInitialized.load())
            {
                log(LogLevel::WARN, "State machine already initialized");
                return *this;
            }
            auto it = states.find(currentStateId.load());
            if (it == states.end())
            {
                log(LogLevel::ERROR, "Initial state not found");
                return *this;
            }
            state = it->second.state.get();
            stateName = it->second.name;
        }

        bool enterSuccess = true;
        try
        {
            if (state)
            {
                // Set the state ID for the initial state
                state->myStateId = currentStateId.load();
                enterSuccess = state->enter();
            }
        }
        catch (const std::exception& e)
        {
            log(LogLevel::ERROR, std::string("Exception in initial state enter: ") + e.what());
            enterSuccess = false;
        }

        if (enterSuccess)
        {
            {
                std::lock_guard<std::mutex> lock(historyMutex);
                isInitialized.store(true);
                addToHistory(currentStateId.load());
            }
            log(LogLevel::INFO, std::string("✓ State machine started in state: ") + stateName);
        }
        else
        {
            log(LogLevel::ERROR, "Failed to start state machine - initial state enter failed");
        }

        return *this;
    }

    /**
     * @brief Update the state machine (call this in your main loop)
     */
    void update()
    {
        if (!isInitialized.load())
        {
            return;
        }

        // Use update-level locking to prevent race conditions
        std::lock_guard<std::mutex> updateLock(updateMutex);

        // Set update in progress flag and clear cancel flag
        updateInProgress.store(true);
        updateShouldCancel.store(false);

        StateGuard currentStateGuard(nullptr, StateID{}, "");
        StateID current;
        uint64_t versionBeforeUpdate;

        {
            std::lock_guard<std::mutex> lock(statesMutex);
            auto it = states.find(currentStateId.load());
            if (it != states.end())
            {
                current = currentStateId.load();
                currentStateGuard = StateGuard(it->second.state, current, it->second.name);
                versionBeforeUpdate = stateVersion.load();
            }
        }

        // Update the state with protection against concurrent state changes
        if (currentStateGuard)
        {
            try
            {
                currentStateGuard->update();

                // Check if state changed during update() - race condition detection
                uint64_t versionAfterUpdate = stateVersion.load();
                if (versionAfterUpdate != versionBeforeUpdate)
                {
                    log(LogLevel::WARN, std::string("State changed during update() - race condition detected. ") +
                                            "Old state update completed after transition but has been invalidated.");
                    // Note: The old state's update() already ran to completion,
                    // but we know it was operating on a stale state
                }
            }
            catch (const std::exception& e)
            {
                log(LogLevel::ERROR, std::string("Exception in state update: ") + e.what());
            }
        }

        // Clear update in progress flag before processing transitions
        updateInProgress.store(false);

        // Process any pending transitions that were queued during update
        processPendingTransitions();

        // Call update callback with the state that was active when update started
        // This ensures consistency even if state changed during update
        // Config access is thread-safe for reads after construction
        if (auto cb = config->getStateUpdateCallback(); cb)
        {
            try
            {
                cb(current, currentStateGuard.stateName);
            }
            catch (const std::exception& e)
            {
                log(LogLevel::ERROR, std::string("Exception in state update callback: ") + e.what());
            }
        }
    }

    /**
     * @brief Change the current state (thread-safe)
     * @param newStateId The ID of the state to transition to
     * @param reason Optional reason for the transition
     * @param cancelUpdate If true (default), cancels current update to transition immediately;
     *                     if false, waits for current update to complete before transitioning
     * @return bool True if the state change was successful
     */
    bool changeState(const StateID &newStateId, std::string_view reason = "", bool cancelUpdate = true)
    {
        // Check if update is currently in progress (lock-free check first)
        if (updateInProgress.load())
        {
            // Queue the transition to be processed after update completes
            queueTransition(newStateId, reason, cancelUpdate);
            return true; // Return true as transition is queued (will be processed)
        }

        // Use update mutex to ensure mutual exclusion with update()
        std::lock_guard<std::mutex> updateLock(updateMutex);

        // Double-check update status after acquiring lock
        if (updateInProgress.load())
        {
            // Queue the transition to be processed after update completes
            queueTransition(newStateId, reason, cancelUpdate);
            return true; // Return true as transition is queued (will be processed)
        }

        // No update in progress, process transition immediately
        return changeStateInternal(newStateId, reason);
    }

    /**
     * @brief Get current state ID (thread-safe)
     * @return StateID The current state ID
     */
    StateID getCurrentStateId() const
    {
        return currentStateId.load();
    }

    /**
     * @brief Get current state name (thread-safe)
     * @return std::string The name of the current state
     */
    std::string getCurrentStateName() const
    {
        StateID currentId = currentStateId.load();
        std::lock_guard<std::mutex> lock(statesMutex);
        auto it = states.find(currentId);
        return (it != states.end()) ? it->second.name : "Unknown";
    }

    /**
     * @brief Get current state instance (thread-safe)
     * @return State* Pointer to the current state object
     */
    State* getCurrentState() const
    {
        StateID currentId = currentStateId.load();
        std::lock_guard<std::mutex> lock(statesMutex);
        auto it = states.find(currentId);
        return (it != states.end()) ? it->second.state.get() : nullptr;
    }

    /**
     * @brief Check if state exists
     * @param id The state ID to check
     * @return bool True if the state exists
     */
    bool hasState(const StateID &id) const
    {
        std::lock_guard<std::mutex> lock(statesMutex);
        return states.find(id) != states.end();
    }

    /**
     * @brief Get state history
     * @return std::vector<StateID> Copy of the state history
     */
    std::vector<StateID> getStateHistory() const
    {
        std::lock_guard<std::mutex> lock(historyMutex);
        return stateHistory;
    }

    /**
     * @brief Reset the state machine to its initial state
     * @return bool True if the reset was successful
     */
    bool reset()
    {
        return changeState(config->getInitialStateId(), "Reset to initial state");
    }

    /**
     * @brief Validate state machine configuration
     * @return bool True if configuration is valid
     */
    bool validate() const
    {
        std::lock_guard<std::mutex> lock(statesMutex);
        return config->validate(states);
    }

    /**
     * @brief Get the number of registered states
     * @return size_t Number of states
     */
    size_t getStateCount() const
    {
        std::lock_guard<std::mutex> lock(statesMutex);
        return states.size();
    }

    /**
     * @brief Check if the state machine is initialized
     * @return bool True if initialized
     */
    bool isReady() const
    {
        return isInitialized.load();
    }

    /**
     * @brief Check if update cancellation has been requested
     * @return bool True if update should be cancelled due to pending transition
     */
    bool isUpdateCancellationRequested() const { return updateShouldCancel.load(); }
};

// Template method implementations for State class
template <typename StateID>
void StateMachineTypes::State<StateID>::changeToState(const StateID &newStateId,
                                                      std::string_view reason,
                                                      bool cancelUpdate)
{
    if (stateMachine)
    {
        stateMachine->changeState(newStateId, reason, cancelUpdate);
    }
}

template<typename StateID>
template<typename T>
std::shared_ptr<T> StateMachineTypes::State<StateID>::getContext() const
{
    if (!stateMachine)
    {
        throw std::runtime_error("State not associated with a state machine");
    }
    return stateMachine->template getContext<T>();
}

template <typename StateID>
bool StateMachineTypes::State<StateID>::isCurrentState() const
{
    if (!stateMachine)
    {
        return false;
    }
    return stateMachine->getCurrentStateId() == myStateId;
}

template <typename StateID>
bool StateMachineTypes::State<StateID>::shouldCancelUpdate() const
{
    if (!stateMachine)
    {
        return false;
    }
    // Access through public method to respect encapsulation
    return stateMachine->isUpdateCancellationRequested();
}

#endif // STATE_MACHINE_HPP
