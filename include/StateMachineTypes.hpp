#ifndef STATE_MACHINE_TYPES_HPP
#define STATE_MACHINE_TYPES_HPP

#include <atomic>
#include <functional>
#include <memory>
#include <string>
#include <string_view>

// Forward declarations
template<typename StateID>
class StateMachine;

/**
 * @brief Core types and interfaces for the StateMachine library
 */
namespace StateMachineTypes {

    // StateID is now a template parameter - no fixed type

    /**
     * @brief Callback types for external observers - templated
     */
    template<typename StateID>
    using StateChangeCallback = std::function<void(
        const StateID& from, const StateID& to,
        std::string_view fromName, std::string_view toName,
        std::string_view reason)>;
    
    template<typename StateID>
    using StateUpdateCallback = std::function<void(
        const StateID& current, std::string_view currentName)>;
    
    template<typename StateID>
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

    /**
     * @brief Performance metrics collection
     */
    struct PerformanceMetrics
    {
        // Timing metrics (in microseconds)
        std::atomic<uint64_t> totalStateTransitions{0};
        std::atomic<uint64_t> totalTransitionTime{0};
        std::atomic<uint64_t> maxTransitionTime{0};
        std::atomic<uint64_t> minTransitionTime{UINT64_MAX};

        // State-specific metrics
        std::atomic<uint64_t> totalStateEnterTime{0};
        std::atomic<uint64_t> totalStateExitTime{0};
        std::atomic<uint64_t> totalStateUpdateTime{0};

        // Call counters
        std::atomic<uint64_t> enterCallCount{0};
        std::atomic<uint64_t> exitCallCount{0};
        std::atomic<uint64_t> updateCallCount{0};

        // Error tracking
        std::atomic<uint64_t> failedTransitions{0};
        std::atomic<uint64_t> exceptionCount{0};

        // Concurrency metrics
        std::atomic<uint64_t> lockContentions{0};

        void reset()
        {
            totalStateTransitions = 0;
            totalTransitionTime = 0;
            maxTransitionTime = 0;
            minTransitionTime = UINT64_MAX;
            totalStateEnterTime = 0;
            totalStateExitTime = 0;
            totalStateUpdateTime = 0;
            enterCallCount = 0;
            exitCallCount = 0;
            updateCallCount = 0;
            failedTransitions = 0;
            exceptionCount = 0;
            lockContentions = 0;
        }
    };

    /**
     * @brief
     *
     */
    template<typename StateID>
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
        virtual void update() {}

        /**
         * @brief Called when exiting this state
         */
        virtual void exit() {}

        /**
         * @brief Request a state transition from within the state
         * @param newStateId The state ID to transition to
         * @param reason Optional reason for the transition
         * @param cancelUpdate If true, cancels current update; if false, waits for update to complete
         */
        void changeToState(const StateID &newStateId, std::string_view reason = "", bool cancelUpdate = false);

        /**
         * @brief Get the state machine this state belongs to
         */
        StateMachine<StateID>* getStateMachine() const { return stateMachine; }

        /**
         * @brief Get the context object with type safety
         * @tparam T The type to cast the context to
         * @return std::shared_ptr<T> Shared pointer to the context object
         * @throws std::runtime_error if context type doesn't match
         */
        template <typename T>
        std::shared_ptr<T> getContext() const;

        /**
         * @brief Set the state machine pointer (internal use only)
         */
        void setStateMachine(StateMachine<StateID>* sm) { stateMachine = sm; }

        /**
         * @brief Check if this state is still the current active state
         * @return true if this state is still current, false if a transition occurred
         *
         * Long-running update() methods should periodically call this to check
         * if they should continue executing or early-exit due to state transition.
         */
        bool isCurrentState() const;

        /**
         * @brief Check if the current update should be cancelled
         * @return true if update should stop immediately due to pending state transition
         *
         * Long-running update() methods should call this periodically and exit early
         * if it returns true to allow state transitions to proceed.
         */
        bool shouldCancelUpdate() const;

    protected:
        friend class StateMachine<StateID>;
        StateMachine<StateID>* stateMachine = nullptr;
        mutable StateID myStateId{}; // Set when state becomes active
    };

    /**
     * @brief Internal structure to hold state information - templated
     */
    template<typename StateID>
    struct StateInfo
    {
        std::string name;
        std::shared_ptr<State<StateID>> state;

        StateInfo() = default;

        StateInfo(std::string n, std::shared_ptr<State<StateID>> s) : name(std::move(n)), state(std::move(s)) {}

        // Constructor for unique_ptr compatibility - convert to shared_ptr
        StateInfo(std::string n, std::unique_ptr<State<StateID>> s)
            : name(std::move(n)), state(std::move(s))
        {
        }
    };

} // namespace StateMachineTypes

#endif // STATE_MACHINE_TYPES_HPP
