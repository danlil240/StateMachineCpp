#ifndef STATE_MACHINE_TYPES_HPP
#define STATE_MACHINE_TYPES_HPP

#include <functional>
#include <memory>
#include <string>
#include <string_view>
#include <stdexcept>

// Forward declaration
class StateMachine;

/**
 * @brief Core types and interfaces for the StateMachine library
 */
namespace StateMachineTypes {

    using StateID = int;

    /**
     * @brief Callback types for external observers
     */
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
        void changeToState(const StateID& newStateId, std::string_view reason = "");

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
        std::shared_ptr<T> getContext() const;

        /**
         * @brief Set the state machine pointer (internal use only)
         */
        void setStateMachine(StateMachine* sm) { stateMachine = sm; }

    private:
        friend class StateMachine;
        StateMachine* stateMachine = nullptr;
    };

    /**
     * @brief Internal structure to hold state information
     */
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

} // namespace StateMachineTypes

#endif // STATE_MACHINE_TYPES_HPP
