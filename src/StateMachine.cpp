#include "StateMachine.hpp"
#include "StateMachineLogger.hpp"
#include <stdexcept>

// Implementation of StateMachineTypes::State::changeToState method
void StateMachineTypes::State::changeToState(const StateID& newStateId, std::string_view reason)
{
    if (stateMachine)
    {
        stateMachine->changeState(newStateId, reason);
    }
}


// StateMachine implementation
StateMachine::StateMachine(const StateID& initialState, std::string name)
    : currentStateId(initialState), config(std::make_unique<StateMachineConfig>(initialState, std::move(name)))
{
    stateHistory.reserve(config->getMaxHistorySize());
}

void StateMachine::log(LogLevel level, std::string_view color, std::string_view message) const
{
    if (auto* logger = config->getLogger(); logger && StateMachineLogger::shouldLog(config->getLogLevel(), level))
    {
        logger->log(level, color, config->getMachineName(), message);
    }
}

void StateMachine::addToHistory(const StateID& stateId)
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

bool StateMachine::handleTransitionFailure(const StateID& failedState, std::string_view reason)
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
    
    if (const auto& onErrorCb = config->getErrorCallback(); onErrorCb)
    {
        onErrorCb(reason, currentStateId.load());
    }
    return false;
}

bool StateMachine::changeStateInternal(const StateID& newStateId, std::string_view reason)
{
    State* oldState = nullptr;
    State* newState = nullptr;
    StateID oldStateId;
    std::string oldStateName, newStateName;
    StateChangeCallback stateChangeCb;

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

StateMachine& StateMachine::addState(const StateID& id, std::string_view name, std::unique_ptr<State> state)
{
    std::lock_guard<std::mutex> lock(stateMutex);

    if (states.find(id) != states.end())
    {
        log(LogLevel::WARN, StateMachineLogger::YELLOW,
            std::string("State ") + std::string(name) + " already exists, skipping...");
        return *this;
    }

    // Set the state machine pointer
    state->setStateMachine(this);
    states.emplace(id, StateInfo(std::string(name), std::move(state)));

    log(LogLevel::DEBUG, StateMachineLogger::CYAN, std::string("Added state: ") + std::string(name));
    return *this;
}

StateMachine& StateMachine::withFallback(const StateID& fallbackState)
{
    config->withFallback(fallbackState);
    return *this;
}

StateMachine& StateMachine::withLogLevel(LogLevel level)
{
    config->withLogLevel(level);
    return *this;
}

StateMachine& StateMachine::withLogger(std::unique_ptr<StateMachineLogger::ILogger> logger)
{
    config->withLogger(std::move(logger));
    return *this;
}

StateMachine& StateMachine::withHistorySize(size_t size)
{
    config->withHistorySize(size);
    stateHistory.reserve(size);
    return *this;
}

StateMachine& StateMachine::onStateChanged(StateChangeCallback callback)
{
    config->onStateChanged(std::move(callback));
    return *this;
}

StateMachine& StateMachine::onStateUpdated(StateUpdateCallback callback)
{
    config->onStateUpdated(std::move(callback));
    return *this;
}

StateMachine& StateMachine::onError(ErrorCallback callback)
{
    config->onError(std::move(callback));
    return *this;
}

StateMachine& StateMachine::start()
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

void StateMachine::update()
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
    if (const auto& onStateUpdate = config->getStateUpdateCallback(); onStateUpdate)
    {
        try
        {
            onStateUpdate(current, currentName);
        }
        catch (const std::exception& e)
        {
            log(LogLevel::ERROR, StateMachineLogger::RED, std::string("Exception in state update callback: ") + e.what());
        }
    }
}

bool StateMachine::changeState(const StateID& newStateId, std::string_view reason)
{
    return changeStateInternal(newStateId, reason);
}

StateMachine::StateID StateMachine::getCurrentStateId() const
{
    return currentStateId.load();
}

std::string StateMachine::getCurrentStateName() const
{
    std::lock_guard<std::mutex> lock(stateMutex);
    auto it = states.find(currentStateId.load());
    return (it != states.end()) ? it->second.name : "Unknown";
}

StateMachine::State* StateMachine::getCurrentState() const
{
    std::lock_guard<std::mutex> lock(stateMutex);
    auto it = states.find(currentStateId.load());
    return (it != states.end()) ? it->second.state.get() : nullptr;
}

bool StateMachine::hasState(const StateID& id) const
{
    std::lock_guard<std::mutex> lock(stateMutex);
    return states.find(id) != states.end();
}

std::vector<StateMachine::StateID> StateMachine::getStateHistory() const
{
    std::lock_guard<std::mutex> lock(stateMutex);
    return stateHistory;
}

bool StateMachine::reset()
{
    return changeState(config->getInitialStateId(), "Reset to initial state");
}

bool StateMachine::validate() const
{
    std::lock_guard<std::mutex> lock(stateMutex);
    return config->validate(states);
}

size_t StateMachine::getStateCount() const
{
    std::lock_guard<std::mutex> lock(stateMutex);
    return states.size();
}

bool StateMachine::isReady() const
{
    return isInitialized.load();
}
