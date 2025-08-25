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

    // Configuration
    std::unique_ptr<StateMachineConfig<StateID>> config;

    // History tracking
    std::vector<StateID> stateHistory;

    // Thread safety
    mutable std::mutex stateMutex;

    // Private helper methods (implemented inline for template)
    void log(LogLevel level, std::string_view color, std::string_view message) const
    {
        if (auto* logger = config->getLogger(); logger && StateMachineLogger::shouldLog(config->getLogLevel(), level))
        {
            logger->log(level, color, config->getMachineName(), message);
        }
    }

    void addToHistory(const StateID &stateId)
    {
        if (stateHistory.empty() || stateHistory.back() != stateId)
        {
            stateHistory.push_back(stateId);
        }
        if (stateHistory.size() > config->getMaxHistorySize())
        {
            stateHistory.erase(stateHistory.begin());
        }
    }

    bool handleTransitionFailure(const StateID &failedState, std::string_view reason)
    {
        const auto& fallbackStateId = config->getFallbackStateId();
        if (fallbackStateId.has_value() && fallbackStateId.value() != failedState)
        {
            log(LogLevel::WARN, StateMachineLogger::YELLOW,
                std::string("State transition failed, falling back: ") + std::string(reason));
            return changeStateInternal(fallbackStateId.value(), "Fallback after failure");
        }

        log(LogLevel::ERROR, StateMachineLogger::RED,
            std::string("State transition failed with no fallback: ") + std::string(reason));
        if (auto cb = config->getErrorCallback(); cb)
        {
            cb(reason, currentStateId.load());
        }
        return false;
    }

    bool changeStateInternal(const StateID &newStateId, std::string_view reason)
    {
        State* oldState = nullptr;
        State* newState = nullptr;
        StateID oldStateId;
        std::string oldStateName, newStateName;
        StateChangeCallback stateChangeCb; // local copies of callbacks

        {
            std::lock_guard<std::mutex> lock(stateMutex);

            if (!isInitialized.load())
            {
                log(LogLevel::WARN, StateMachineLogger::YELLOW, "Attempted state change before initialization");
                return false;
            }

            auto oldStateIter = states.find(currentStateId.load());
            auto newStateIter = states.find(newStateId);

            if (oldStateIter == states.end() || newStateIter == states.end())
            {
                log(LogLevel::ERROR, StateMachineLogger::RED, "State transition between invalid states");
                return false;
            }

            // No need to change to the same state
            if (currentStateId.load() == newStateId)
            {
                log(LogLevel::DEBUG, StateMachineLogger::CYAN, "Ignored transition to same state");
                return true;
            }

            // Prepare info for outside use
            oldState = oldStateIter->second.state.get();
            newState = newStateIter->second.state.get();
            oldStateId = currentStateId.load();
            oldStateName = oldStateIter->second.name;
            newStateName = newStateIter->second.name;

            // Copy callback to local variable
            stateChangeCb = config->getStateChangeCallback();
        }

        // ---- OUTSIDE LOCK ----

        // Log state change
        std::string reasonText = reason.empty() ? "" : std::string("\t(Reason: ") + std::string(reason) + ")";
        log(LogLevel::INFO, StateMachineLogger::BLUE,
            "|" + std::string(StateMachineLogger::STRIKETHROUGH) + oldStateName + 
            std::string(StateMachineLogger::RESET) + std::string(StateMachineLogger::BOLD) +
            std::string(StateMachineLogger::BLUE) + "| ➔ |" + newStateName + "|" + reasonText);

        // Exit old state
        try
        {
            if (oldState)
                oldState->exit();
        }
        catch (const std::exception& e)
        {
            log(LogLevel::ERROR, StateMachineLogger::RED, std::string("Exception in state exit: ") + e.what());
        }

        // Enter new state
        bool enterSuccess = true;
        try
        {
            if (newState)
                enterSuccess = newState->enter();
        }
        catch (const std::exception& e)
        {
            log(LogLevel::ERROR, StateMachineLogger::RED, std::string("Exception in state enter: ") + e.what());
            enterSuccess = false;
        }

        if (!enterSuccess)
        {
            // Rollback - restore old state since enter() failed
            {
                std::lock_guard<std::mutex> lock(stateMutex);
                currentStateId = oldStateId;
            }
            return handleTransitionFailure(newStateId, "State enter() returned false");
        }

        // State transition successful - now update the state
        {
            std::lock_guard<std::mutex> lock(stateMutex);
            currentStateId = newStateId;
            addToHistory(newStateId);
        }

        // Callback outside lock
        if (stateChangeCb)
        {
            try
            {
                stateChangeCb(oldStateId, newStateId, oldStateName, newStateName, reason);
            }
            catch (const std::exception& e)
            {
                log(LogLevel::ERROR, StateMachineLogger::RED, std::string("Exception in state change callback: ") + e.what());
            }
        }

        return true;
    }

public:
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
    StateMachine& operator=(StateMachine&&) = default;

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

        std::lock_guard<std::mutex> lock(stateMutex);

        if (states.find(id) != states.end())
        {
            log(LogLevel::WARN, StateMachineLogger::YELLOW,
                std::string("State ") + std::string(name) + " already exists, skipping...");
            return *this;
        }

        auto state = std::make_unique<StateType>();
        state->setStateMachine(this);
        states.emplace(id, StateInfo(std::string(name), std::move(state)));

        log(LogLevel::DEBUG, StateMachineLogger::CYAN, std::string("Added state: ") + std::string(name));
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
        std::lock_guard<std::mutex> lock(stateMutex);

        if (states.find(id) != states.end())
        {
            log(LogLevel::WARN, StateMachineLogger::YELLOW,
                std::string("State ") + std::string(name) + " already exists, skipping...");
            return *this;
        }

        state->setStateMachine(this);
        states.emplace(id, StateInfo(std::string(name), std::move(state)));

        log(LogLevel::DEBUG, StateMachineLogger::CYAN, std::string("Added state: ") + std::string(name));
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
            std::lock_guard<std::mutex> lock(stateMutex);
            if (isInitialized.load())
            {
                log(LogLevel::WARN, StateMachineLogger::YELLOW, "State machine already initialized");
                return *this;
            }
            auto it = states.find(currentStateId.load());
            if (it == states.end())
            {
                log(LogLevel::ERROR, StateMachineLogger::RED, "Initial state not found");
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
                enterSuccess = state->enter();
            }
        }
        catch (const std::exception& e)
        {
            log(LogLevel::ERROR, StateMachineLogger::RED, std::string("Exception in initial state enter: ") + e.what());
            enterSuccess = false;
        }

        if (enterSuccess)
        {
            {
                std::lock_guard<std::mutex> lock(stateMutex);
                isInitialized.store(true);
                addToHistory(currentStateId.load());
            }
            log(LogLevel::INFO, StateMachineLogger::GREEN, std::string("✓ State machine started in state: ") + stateName);
        }
        else
        {
            log(LogLevel::ERROR, StateMachineLogger::RED, "Failed to start state machine - initial state enter failed");
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

        State* currentState = nullptr;
        StateID current;
        std::string currentName;

        {
            std::lock_guard<std::mutex> lock(stateMutex);
            auto it = states.find(currentStateId.load());
            if (it != states.end())
            {
                currentState = it->second.state.get();
                current = currentStateId.load();
                currentName = it->second.name;
            }
        }

        // Update the state outside of the lock to prevent deadlocks
        if (currentState)
        {
            try
            {
                currentState->update();
            }
            catch (const std::exception& e)
            {
                log(LogLevel::ERROR, StateMachineLogger::RED, std::string("Exception in state update: ") + e.what());
            }
        }

        // Call update callback
        if (auto cb = config->getStateUpdateCallback(); cb)
        {
            try
            {
                cb(current, currentName);
            }
            catch (const std::exception& e)
            {
                log(LogLevel::ERROR, StateMachineLogger::RED, std::string("Exception in state update callback: ") + e.what());
            }
        }
    }

    /**
     * @brief Change the current state (thread-safe)
     * @param newStateId The ID of the state to transition to
     * @param reason Optional reason for the transition
     * @return bool True if the state change was successful
     */
    bool changeState(const StateID &newStateId, std::string_view reason = "")
    {
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
        std::lock_guard<std::mutex> lock(stateMutex);
        auto it = states.find(currentStateId.load());
        return (it != states.end()) ? it->second.name : "Unknown";
    }

    /**
     * @brief Get current state instance (thread-safe)
     * @return State* Pointer to the current state object
     */
    State* getCurrentState() const
    {
        std::lock_guard<std::mutex> lock(stateMutex);
        auto it = states.find(currentStateId.load());
        return (it != states.end()) ? it->second.state.get() : nullptr;
    }

    /**
     * @brief Check if state exists
     * @param id The state ID to check
     * @return bool True if the state exists
     */
    bool hasState(const StateID &id) const
    {
        std::lock_guard<std::mutex> lock(stateMutex);
        return states.find(id) != states.end();
    }

    /**
     * @brief Get state history
     * @return std::vector<StateID> Copy of the state history
     */
    std::vector<StateID> getStateHistory() const
    {
        std::lock_guard<std::mutex> lock(stateMutex);
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
        std::lock_guard<std::mutex> lock(stateMutex);
        return config->validate(states);
    }

    /**
     * @brief Get the number of registered states
     * @return size_t Number of states
     */
    size_t getStateCount() const
    {
        std::lock_guard<std::mutex> lock(stateMutex);
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
};

// Template method implementations for State class
template<typename StateID>
void StateMachineTypes::State<StateID>::changeToState(const StateID& newStateId, std::string_view reason)
{
    if (stateMachine)
    {
        stateMachine->changeState(newStateId, reason);
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

#endif // STATE_MACHINE_HPP
