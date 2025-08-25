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
 */
class StateMachine
{
public:
    using StateID = StateMachineTypes::StateID;
    using State = StateMachineTypes::State;
    using LogLevel = StateMachineTypes::LogLevel;
    using StateChangeCallback = StateMachineTypes::StateChangeCallback;
    using StateUpdateCallback = StateMachineTypes::StateUpdateCallback;
    using ErrorCallback = StateMachineTypes::ErrorCallback;

private:
    using StateInfo = StateMachineTypes::StateInfo;

    // Core state machine data
    std::unordered_map<StateID, StateInfo> states;
    std::atomic<StateID> currentStateId;
    std::atomic<bool> isInitialized{false};

    // Configuration
    std::unique_ptr<StateMachineConfig> config;

    // History tracking
    std::vector<StateID> stateHistory;

    // Thread safety
    mutable std::mutex stateMutex;

    // Private helper methods (implemented in .cpp)
    void log(LogLevel level, std::string_view color, std::string_view message) const;
    void addToHistory(const StateID &stateId);
    bool handleTransitionFailure(const StateID &failedState, std::string_view reason);
    bool changeStateInternal(const StateID &newStateId, std::string_view reason);

public:
    /**
     * @brief Create a state machine starting in the specified state
     * @param initialState The state ID to start in
     * @param name Name for this state machine instance
     */
    explicit StateMachine(const StateID &initialState, std::string name = "StateMachine");

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
    StateMachine &addState(const StateID &id, std::string_view name, std::unique_ptr<State> state);

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
        return config->getContext<T>();
    }

    /**
     * @brief Set fallback state for error recovery
     * @param fallbackState The state to fall back to on errors
     * @return StateMachine& This state machine for method chaining
     */
    StateMachine &withFallback(const StateID &fallbackState);

    /**
     * @brief Set logging level
     * @param level The logging level to use
     * @return StateMachine& This state machine for method chaining
     */
    StateMachine &withLogLevel(LogLevel level);

    /**
     * @brief Set custom logger
     * @param logger Custom logger implementation
     * @return StateMachine& This state machine for method chaining
     */
    StateMachine &withLogger(std::unique_ptr<StateMachineLogger::ILogger> logger);

    /**
     * @brief Set maximum history size
     * @param size Maximum number of states to keep in history
     * @return StateMachine& This state machine for method chaining
     */
    StateMachine &withHistorySize(size_t size);

    /**
     * @brief Set state change callback
     * @param callback The function to call on state changes
     * @return StateMachine& This state machine for method chaining
     */
    StateMachine &onStateChanged(StateChangeCallback callback);

    /**
     * @brief Set state update callback
     * @param callback The function to call after state updates
     * @return StateMachine& This state machine for method chaining
     */
    StateMachine &onStateUpdated(StateUpdateCallback callback);

    /**
     * @brief Set error callback
     * @param callback The function to call on errors
     * @return StateMachine& This state machine for method chaining
     */
    StateMachine &onError(ErrorCallback callback);

    /**
     * @brief Initialize and start the state machine
     * @return StateMachine& This state machine for method chaining
     */
    StateMachine &start();

    /**
     * @brief Update the state machine (call this in your main loop)
     */
    void update();

    /**
     * @brief Change the current state (thread-safe)
     * @param newStateId The ID of the state to transition to
     * @param reason Optional reason for the transition
     * @return bool True if the state change was successful
     */
    bool changeState(const StateID &newStateId, std::string_view reason = "");

    /**
     * @brief Get current state ID (thread-safe)
     * @return StateID The current state ID
     */
    StateID getCurrentStateId() const;

    /**
     * @brief Get current state name (thread-safe)
     * @return std::string The name of the current state
     */
    std::string getCurrentStateName() const;

    /**
     * @brief Get current state instance (thread-safe)
     * @return State* Pointer to the current state object
     */
    State* getCurrentState() const;

    /**
     * @brief Check if state exists
     * @param id The state ID to check
     * @return bool True if the state exists
     */
    bool hasState(const StateID &id) const;

    /**
     * @brief Get state history
     * @return std::vector<StateID> Copy of the state history
     */
    std::vector<StateID> getStateHistory() const;

    /**
     * @brief Reset the state machine to its initial state
     * @return bool True if the reset was successful
     */
    bool reset();

    /**
     * @brief Validate state machine configuration
     * @return bool True if configuration is valid
     */
    bool validate() const;

    /**
     * @brief Get the number of registered states
     * @return size_t Number of states
     */
    size_t getStateCount() const;

    /**
     * @brief Check if the state machine is initialized
     * @return bool True if initialized
     */
    bool isReady() const;
};

// Template method implementation (must be after StateMachine class definition)
template <typename T>
std::shared_ptr<T> StateMachineTypes::State::getContext() const
{
    if (!stateMachine)
    {
        throw std::runtime_error("State not associated with a state machine");
    }
    return stateMachine->getContext<T>();
}

#endif // STATE_MACHINE_HPP
