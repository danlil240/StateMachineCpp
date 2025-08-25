#ifndef STATE_MACHINE_HPP
#define STATE_MACHINE_HPP

#include <atomic>
#include <functional>
#include <iostream>
#include <memory>
#include <mutex>
#include <optional>
#include <stdexcept>
#include <string>
#include <string_view>
#include <type_traits>
#include <typeindex>
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
 */
class StateMachine
{
public:
    using StateID = int;
    /**
     * @brief Base state class that all states must inherit from
     */
    class State
    {
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
        virtual void update()
        {
        }

        /**
         * @brief Called when exiting this state
         */
        virtual void exit()
        {
        }

        /**
         * @brief Request a state transition from within the state
         * @param newStateId The state ID to transition to
         * @param reason Optional reason for the transition
         */
        void changeToState(const StateID& newStateId, std::string_view reason = "")
        {
            if (stateMachine)
            {
                stateMachine->changeState(newStateId, reason);
            }
        }

        /**
         * @brief Get the state machine this state belongs to
         */
        StateMachine* getStateMachine() const { return stateMachine; }

        /**
         * @brief Get the context object with type safety
         * @tparam T The type to cast the context to
         * @return std::shared_ptr<T> Shared pointer to the context object
         * @throws std::runtime_error if context type doesn't match
         */
        template <typename T>
        std::shared_ptr<T> getContext() const
        {
            if (!stateMachine)
            {
                throw std::runtime_error("State not associated with a state machine");
            }
            return stateMachine->getContext<T>();
        }

    private:
        friend class StateMachine;
        StateMachine* stateMachine = nullptr;
    };

    // Callback types for external observers
    using StateChangeCallback = std::function<void(
        const StateID& from, const StateID& to,
        std::string_view fromName, std::string_view toName,
        std::string_view reason)>;
    using StateUpdateCallback = std::function<void(
        const StateID& current, std::string_view currentName)>;
    using ErrorCallback = std::function<void(
        std::string_view error, const StateID& currentState)>;

    /**
     * @brief Logging levels for state machine output
     */
    enum class LogLevel
    {
        NONE, // No logging
        ERROR, // Only errors
        WARN, // Warnings and errors
        INFO, // Info, warnings, and errors
        DEBUG // All logging including debug info
    };

private:
    // ANSI color codes for terminal output
    static constexpr std::string_view RESET = "\033[0m";
    static constexpr std::string_view BOLD = "\033[1m";
    static constexpr std::string_view STRIKETHROUGH = "\033[9m";
    static constexpr std::string_view RED = "\033[31m";
    static constexpr std::string_view GREEN = "\033[32m";
    static constexpr std::string_view YELLOW = "\033[33m";
    static constexpr std::string_view BLUE = "\033[34m";
    static constexpr std::string_view MAGENTA = "\033[35m";
    static constexpr std::string_view CYAN = "\033[36m";

    struct StateInfo
    {
        std::string name;
        std::unique_ptr<State> state;

        StateInfo() = default;

        StateInfo(std::string n, std::unique_ptr<State> s)
            : name(std::move(n)), state(std::move(s))
        {
        }
    };

    // Core state machine data
    std::unordered_map<StateID, StateInfo> states;
    std::atomic<StateID> currentStateId;
    StateID initialStateId;
    std::optional<StateID> fallbackStateId;
    std::atomic<bool> isInitialized{false};

    // Configuration
    std::string machineName;
    std::atomic<LogLevel> logLevel{LogLevel::INFO};

    // History tracking
    std::vector<StateID> stateHistory;
    size_t maxHistorySize = 100;

    // Thread safety - single mutex for simplicity and correctness
    mutable std::mutex stateMutex;

    // Callbacks
    StateChangeCallback onStateChange;
    StateUpdateCallback onStateUpdate;
    ErrorCallback onErrorCb;

    // Type-safe context storage
    std::shared_ptr<void> userContext;
    std::type_index contextType{typeid(void)};

    /**
     * @brief Log a message with the specified level
     */
    void log(LogLevel level, std::string_view color, std::string_view message) const
    {
        if (logLevel.load() >= level)
        {
            std::cout << color << "[" << machineName << "] " << message << RESET << std::endl;
        }
    }

    /**
     * @brief Add state to history with size management
     */
    void addToHistory(const StateID& stateId)
    {
        if (stateHistory.empty() || stateHistory.back() != stateId)
        {
            stateHistory.push_back(stateId);
        }
        if (stateHistory.size() > maxHistorySize)
        {
            stateHistory.erase(stateHistory.begin());
        }
    }

    /**
     * @brief Handle state transition failure with fallback
     */
    bool handleTransitionFailure(const StateID& failedState, std::string_view reason)
    {
        if (fallbackStateId.has_value() && fallbackStateId.value() != failedState)
        {
            log(LogLevel::WARN, YELLOW,
                std::string("State transition failed, falling back: ") + std::string(reason));
            return changeStateInternal(fallbackStateId.value(), "Fallback after failure");
        }

        log(LogLevel::ERROR, RED,
            std::string("State transition failed with no fallback: ") + std::string(reason));
        if (onErrorCb)
        {
            onErrorCb(reason, currentStateId.load());
        }
        return false;
    }

    /**
     * @brief Internal state change implementation
     */
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
                log(LogLevel::WARN, YELLOW, "Attempted state change before initialization");
                return false;
            }

            auto oldStateIter = states.find(currentStateId.load());
            auto newStateIter = states.find(newStateId);

            if (oldStateIter == states.end() || newStateIter == states.end())
            {
                log(LogLevel::ERROR, RED, "State transition between invalid states");
                return false;
            }

            // No need to change to the same state
            if (currentStateId.load() == newStateId)
            {
                log(LogLevel::DEBUG, CYAN, "Ignored transition to same state");
                return true;
            }

            // Prepare info for outside use
            oldState = oldStateIter->second.state.get();
            newState = newStateIter->second.state.get();
            oldStateId = currentStateId.load();
            oldStateName = oldStateIter->second.name;
            newStateName = newStateIter->second.name;

            // Copy callbacks to local variables
            stateChangeCb = onStateChange;
            
            // NOTE: State update will happen after successful enter()
        }

        // ---- OUTSIDE LOCK ----

        // Log state change
        std::string reasonText = reason.empty() ? "" : std::string("\t(Reason: ") + std::string(reason) + ")";
        log(LogLevel::INFO, BLUE,
            "|" + std::string(STRIKETHROUGH) + oldStateName + std::string(RESET) + std::string(BOLD) +
                std::string(BLUE) + "| ➔ |" + newStateName + "|" + reasonText);

        // Exit old state
        try
        {
            if (oldState)
                oldState->exit();
        }
        catch (const std::exception &e)
        {
            log(LogLevel::ERROR, RED, std::string("Exception in state exit: ") + e.what());
        }

        // Enter new state
        bool enterSuccess = true;
        try
        {
            if (newState)
                enterSuccess = newState->enter();
        }
        catch (const std::exception &e)
        {
            log(LogLevel::ERROR, RED, std::string("Exception in state enter: ") + e.what());
            enterSuccess = false;
        }

        if (!enterSuccess)
        {
            // Rollback - restore old state since enter() failed
            {
                std::lock_guard<std::mutex> lock(stateMutex);
                currentStateId = oldStateId;
                // Don't add failed state to history
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
            catch (const std::exception &e)
            {
                log(LogLevel::ERROR, RED, std::string("Exception in state change callback: ") + e.what());
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
        : currentStateId(initialState), initialStateId(initialState), machineName(std::move(name))
    {
        stateHistory.reserve(maxHistorySize);
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
            log(LogLevel::WARN, YELLOW,
                std::string("State ") + std::string(name) + " already exists, skipping...");
            return *this;
        }

        auto state = std::make_unique<StateType>();
        state->stateMachine = this;
        states.emplace(id, StateInfo(std::string(name), std::move(state)));

        log(LogLevel::DEBUG, CYAN, std::string("Added state: ") + std::string(name));
        return *this;
    }

    /**
     * @brief Add a pre-configured state to the state machine
     * @param id The unique state ID
     * @param name A human-readable name for the state
     * @param state A pre-configured state instance
     * @return StateMachine& This state machine for method chaining
     */
    StateMachine& addState(const StateID& id, std::string_view name, std::unique_ptr<State> state)
    {
        std::lock_guard<std::mutex> lock(stateMutex);

        if (states.find(id) != states.end())
        {
            log(LogLevel::WARN, YELLOW,
                std::string("State ") + std::string(name) + " already exists, skipping...");
            return *this;
        }

        state->stateMachine = this;
        states.emplace(id, StateInfo(std::string(name), std::move(state)));

        log(LogLevel::DEBUG, CYAN, std::string("Added state: ") + std::string(name));
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
        std::lock_guard<std::mutex> lock(stateMutex);
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
    template <typename T>
    std::shared_ptr<T> getContext() const
    {
        if (!userContext)
        {
            throw std::runtime_error("No context set");
        }

        if (contextType != std::type_index(typeid(T)))
        {
            throw std::runtime_error("Context type mismatch");
        }

        return std::static_pointer_cast<T>(userContext);
    }

    /**
     * @brief Set fallback state for error recovery
     * @param fallbackState The state to fall back to on errors
     * @return StateMachine& This state machine for method chaining
     */
    StateMachine& withFallback(const StateID& fallbackState)
    {
        std::lock_guard<std::mutex> lock(stateMutex);
        fallbackStateId = fallbackState;
        return *this;
    }

    /**
     * @brief Set logging level
     * @param level The logging level to use
     * @return StateMachine& This state machine for method chaining
     */
    StateMachine& withLogLevel(LogLevel level)
    {
        logLevel.store(level);
        return *this;
    }

    /**
     * @brief Set maximum history size
     * @param size Maximum number of states to keep in history
     * @return StateMachine& This state machine for method chaining
     */
    StateMachine& withHistorySize(size_t size)
    {
        std::lock_guard<std::mutex> lock(stateMutex);
        maxHistorySize = size;
        stateHistory.reserve(size);
        return *this;
    }

    /**
     * @brief Set state change callback
     * @param callback The function to call on state changes
     * @return StateMachine& This state machine for method chaining
     */
    StateMachine& onStateChanged(StateChangeCallback callback)
    {
        std::lock_guard<std::mutex> lock(stateMutex);
        onStateChange = std::move(callback);
        return *this;
    }

    /**
     * @brief Set state update callback
     * @param callback The function to call after state updates
     * @return StateMachine& This state machine for method chaining
     */
    StateMachine& onStateUpdated(StateUpdateCallback callback)
    {
        std::lock_guard<std::mutex> lock(stateMutex);
        onStateUpdate = std::move(callback);
        return *this;
    }

    /**
     * @brief Set error callback
     * @param callback The function to call on errors
     * @return StateMachine& This state machine for method chaining
     */
    StateMachine& onError(ErrorCallback callback)
    {
        std::lock_guard<std::mutex> lock(stateMutex);
        onErrorCb = std::move(callback);
        return *this;
    }

    /**
     * @brief Initialize and start the state machine
     * @return StateMachine& This state machine for method chaining
     */
    StateMachine& start()
    {
        // Gather state under lock, but call enter() outside to avoid deadlocks
        State* state = nullptr;
        std::string stateName;
        {
            std::lock_guard<std::mutex> lock(stateMutex);
            if (isInitialized.load())
            {
                log(LogLevel::WARN, YELLOW, "State machine already initialized");
                return *this;
            }
            auto it = states.find(currentStateId.load());
            if (it == states.end())
            {
                log(LogLevel::ERROR, RED, "Initial state not found");
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
            log(LogLevel::ERROR, RED, std::string("Exception in initial state enter: ") + e.what());
            enterSuccess = false;
        }

        if (enterSuccess)
        {
            {
                std::lock_guard<std::mutex> lock(stateMutex);
                isInitialized.store(true);
                addToHistory(currentStateId.load());
            }
            log(LogLevel::INFO, GREEN, std::string("✓ State machine started in state: ") + stateName);
        }
        else
        {
            log(LogLevel::ERROR, RED, "Failed to start state machine - initial state enter failed");
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
                log(LogLevel::ERROR, RED, std::string("Exception in state update: ") + e.what());
            }
        }

        // Call update callback
        if (onStateUpdate)
        {
            try
            {
                onStateUpdate(current, currentName);
            }
            catch (const std::exception& e)
            {
                log(LogLevel::ERROR, RED, std::string("Exception in state update callback: ") + e.what());
            }
        }
    }

    /**
     * @brief Change the current state (thread-safe)
     * @param newStateId The ID of the state to transition to
     * @param reason Optional reason for the transition
     * @return bool True if the state change was successful
     */
    bool changeState(const StateID& newStateId, std::string_view reason = "")
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
    bool hasState(const StateID& id) const
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
        return changeState(initialStateId, "Reset to initial state");
    }

    /**
     * @brief Validate state machine configuration
     * @return bool True if configuration is valid
     */
    bool validate() const
    {
        std::lock_guard<std::mutex> lock(stateMutex);

        bool valid = true;

        // Check if initial state exists
        if (states.find(initialStateId) == states.end())
        {
            log(LogLevel::ERROR, RED, "Initial state not found in state registry");
            valid = false;
        }

        // Check if current state exists
        if (states.find(currentStateId.load()) == states.end())
        {
            log(LogLevel::ERROR, RED, "Current state not found in state registry");
            valid = false;
        }

        // Check if fallback state exists (if set)
        if (fallbackStateId.has_value() && states.find(fallbackStateId.value()) == states.end())
        {
            log(LogLevel::ERROR, RED, "Fallback state not found in state registry");
            valid = false;
        }

        return valid;
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

#endif // STATE_MACHINE_HPP
