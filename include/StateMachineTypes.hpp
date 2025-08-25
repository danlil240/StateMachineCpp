#ifndef STATE_MACHINE_TYPES_HPP
#define STATE_MACHINE_TYPES_HPP

#include <functional>
#include <memory>
#include <string>
#include <string_view>
#include <stdexcept>
#include <type_traits>

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
     * @brief Base state class that all states must inherit from - templated
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
         */
        void changeToState(const StateID& newStateId, std::string_view reason = "");

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

    private:
        friend class StateMachine<StateID>;
        StateMachine<StateID>* stateMachine = nullptr;
    };

    /**
     * @brief Internal structure to hold state information - templated
     */
    template<typename StateID>
    struct StateInfo
    {
        std::string name;
        std::unique_ptr<State<StateID>> state;

        StateInfo() = default;

        StateInfo(std::string n, std::unique_ptr<State<StateID>> s)
            : name(std::move(n)), state(std::move(s))
        {
        }
    };

} // namespace StateMachineTypes

#endif // STATE_MACHINE_TYPES_HPP
