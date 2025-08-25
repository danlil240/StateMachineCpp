#ifndef STATE_MACHINE_CONFIG_HPP
#define STATE_MACHINE_CONFIG_HPP

#include "StateMachineTypes.hpp"
#include "StateMachineLogger.hpp"
#include <memory>
#include <optional>
#include <typeindex>
#include <unordered_map>

// Forward declaration
template<typename StateID>
class StateMachine;

/**
 * @brief Configuration and builder for StateMachine - templated
 */
template<typename StateID>
class StateMachineConfig
{
public:
    using State = StateMachineTypes::State<StateID>;
    using StateInfo = StateMachineTypes::StateInfo<StateID>;
    using LogLevel = StateMachineTypes::LogLevel;
    using StateChangeCallback = StateMachineTypes::StateChangeCallback<StateID>;
    using StateUpdateCallback = StateMachineTypes::StateUpdateCallback<StateID>;
    using ErrorCallback = StateMachineTypes::ErrorCallback<StateID>;

private:
    // Configuration data
    std::string machineName;
    StateID initialStateId;
    std::optional<StateID> fallbackStateId;
    LogLevel logLevel = LogLevel::INFO;
    size_t maxHistorySize = 100;

    // Callbacks
    StateChangeCallback onStateChange;
    StateUpdateCallback onStateUpdate;
    ErrorCallback onErrorCb;

    // Logger
    std::unique_ptr<StateMachineLogger::ILogger> logger;

    // Type-safe context storage
    std::shared_ptr<void> userContext;
    std::type_index contextType{typeid(void)};

public:
    /**
     * @brief Create configuration with initial state and name
     */
    StateMachineConfig(StateID initialState, std::string name = "StateMachine")
        : machineName(std::move(name)), initialStateId(initialState), logger(StateMachineLogger::createConsoleLogger())
    {
    }

    /**
     * @brief Set fallback state for error recovery
     */
    StateMachineConfig& withFallback(StateID fallbackState)
    {
        fallbackStateId = fallbackState;
        return *this;
    }

    /**
     * @brief Set logging level
     */
    StateMachineConfig& withLogLevel(LogLevel level)
    {
        logLevel = level;
        return *this;
    }

    /**
     * @brief Set maximum history size
     */
    StateMachineConfig& withHistorySize(size_t size)
    {
        maxHistorySize = size;
        return *this;
    }

    /**
     * @brief Set custom logger
     */
    StateMachineConfig& withLogger(std::unique_ptr<StateMachineLogger::ILogger> customLogger)
    {
        logger = std::move(customLogger);
        return *this;
    }

    /**
     * @brief Set state change callback
     */
    StateMachineConfig& onStateChanged(StateChangeCallback callback)
    {
        onStateChange = std::move(callback);
        return *this;
    }

    /**
     * @brief Set state update callback
     */
    StateMachineConfig& onStateUpdated(StateUpdateCallback callback)
    {
        onStateUpdate = std::move(callback);
        return *this;
    }

    /**
     * @brief Set error callback
     */
    StateMachineConfig& onError(ErrorCallback callback)
    {
        onErrorCb = std::move(callback);
        return *this;
    }

    /**
     * @brief Set user context with type safety
     */
    template <typename T>
    StateMachineConfig& withContext(std::shared_ptr<T> context)
    {
        userContext = std::static_pointer_cast<void>(context);
        contextType = std::type_index(typeid(T));
        return *this;
    }

    /**
     * @brief Get user context with type safety
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
     * @brief Validate configuration against registered states
     */
    bool validate(const std::unordered_map<StateID, StateInfo>& states) const
    {
        bool valid = true;

        // Check if initial state exists
        if (states.find(initialStateId) == states.end())
        {
            if (logger && StateMachineLogger::shouldLog(logLevel, LogLevel::ERROR))
            {
                logger->log(LogLevel::ERROR, machineName, 
                           "Initial state not found in state registry");
            }
            valid = false;
        }

        // Check if fallback state exists (if set)
        if (fallbackStateId.has_value() && states.find(fallbackStateId.value()) == states.end())
        {
            if (logger && StateMachineLogger::shouldLog(logLevel, LogLevel::ERROR))
            {
                logger->log(LogLevel::ERROR, machineName,
                           "Fallback state not found in state registry");
            }
            valid = false;
        }

        return valid;
    }

    // Getters for internal use
    const std::string& getMachineName() const { return machineName; }
    StateID getInitialStateId() const { return initialStateId; }
    const std::optional<StateID>& getFallbackStateId() const { return fallbackStateId; }
    LogLevel getLogLevel() const { return logLevel; }
    size_t getMaxHistorySize() const { return maxHistorySize; }
    const StateChangeCallback& getStateChangeCallback() const { return onStateChange; }
    const StateUpdateCallback& getStateUpdateCallback() const { return onStateUpdate; }
    const ErrorCallback& getErrorCallback() const { return onErrorCb; }
    StateMachineLogger::ILogger* getLogger() const { return logger.get(); }
    bool hasContext() const { return userContext != nullptr; }
    std::type_index getContextType() const { return contextType; }
    std::shared_ptr<void> getRawContext() const { return userContext; }
};

#endif // STATE_MACHINE_CONFIG_HPP
