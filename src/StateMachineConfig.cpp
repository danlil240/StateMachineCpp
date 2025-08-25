#include "StateMachineConfig.hpp"

StateMachineConfig::StateMachineConfig(StateID initialState, std::string name)
    : machineName(std::move(name)), initialStateId(initialState), logger(StateMachineLogger::createConsoleLogger())
{
}

StateMachineConfig& StateMachineConfig::withFallback(StateID fallbackState)
{
    fallbackStateId = fallbackState;
    return *this;
}

StateMachineConfig& StateMachineConfig::withLogLevel(LogLevel level)
{
    logLevel = level;
    return *this;
}

StateMachineConfig& StateMachineConfig::withHistorySize(size_t size)
{
    maxHistorySize = size;
    return *this;
}

StateMachineConfig& StateMachineConfig::withLogger(std::unique_ptr<StateMachineLogger::ILogger> customLogger)
{
    logger = std::move(customLogger);
    return *this;
}

StateMachineConfig& StateMachineConfig::onStateChanged(StateChangeCallback callback)
{
    onStateChange = std::move(callback);
    return *this;
}

StateMachineConfig& StateMachineConfig::onStateUpdated(StateUpdateCallback callback)
{
    onStateUpdate = std::move(callback);
    return *this;
}

StateMachineConfig& StateMachineConfig::onError(ErrorCallback callback)
{
    onErrorCb = std::move(callback);
    return *this;
}

bool StateMachineConfig::validate(const std::unordered_map<StateID, StateInfo>& states) const
{
    bool valid = true;

    // Check if initial state exists
    if (states.find(initialStateId) == states.end())
    {
        if (logger && StateMachineLogger::shouldLog(logLevel, LogLevel::ERROR))
        {
            logger->log(LogLevel::ERROR, StateMachineLogger::RED, machineName, 
                       "Initial state not found in state registry");
        }
        valid = false;
    }

    // Check if fallback state exists (if set)
    if (fallbackStateId.has_value() && states.find(fallbackStateId.value()) == states.end())
    {
        if (logger && StateMachineLogger::shouldLog(logLevel, LogLevel::ERROR))
        {
            logger->log(LogLevel::ERROR, StateMachineLogger::RED, machineName,
                       "Fallback state not found in state registry");
        }
        valid = false;
    }

    return valid;
}
