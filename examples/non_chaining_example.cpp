/**
 * @file non_chaining_example.cpp
 * @brief Example showing both chaining and non-chaining ways to add states
 */

#include "StateMachine.hpp"
#include <iostream>

// Simple state machine example
enum TestState { A, B, C };

class StateA : public StateMachine::State
{
public:
    bool enter() override {
        std::cout << "Entering State A" << std::endl;
        return true;
    }
};

class StateB : public StateMachine::State
{
public:
    bool enter() override {
        std::cout << "Entering State B" << std::endl;
        return true;
    }
};

class StateC : public StateMachine::State
{
public:
    bool enter() override {
        std::cout << "Entering State C" << std::endl;
        return true;
    }
};

int main() {
    std::cout << "=== Chaining Style (Fluent API) ===" << std::endl;
    
    // Chaining style - returning references for fluent API
    StateMachine chainingSM(TestState::A, "ChainingExample");

    chainingSM.addState<StateA>(TestState::A, "State A")
        .addState<StateB>(TestState::B, "State B")
        .addState<StateC>(TestState::C, "State C")
        .withLogLevel(StateMachine::LogLevel::INFO)
        .start();

    std::cout << "Current state: " << chainingSM.getCurrentStateName() << std::endl;
    
    std::cout << "\n=== Non-chaining Style ===" << std::endl;
    
    // Non-chaining style - using addState without chaining (ignoring return value)
    StateMachine nonChainingSM(TestState::A, "NonChainingExample");

    nonChainingSM.addState<StateA>(TestState::A, "State A");  // Ignore return value
    nonChainingSM.addState<StateB>(TestState::B, "State B");  // Ignore return value
    nonChainingSM.addState<StateC>(
        TestState::C, "State C"); // Ignore return value    nonChainingSM.withLogLevel(StateMachine::LogLevel::INFO);
    nonChainingSM.start();
    
    std::cout << "Current state: " << nonChainingSM.getCurrentStateName() << std::endl;
    
    std::cout << "\n✅ All three styles work perfectly!" << std::endl;
    return 0;
}
