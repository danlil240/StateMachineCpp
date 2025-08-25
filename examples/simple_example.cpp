/**
 * @file simple_example.cpp
 * @brief Simple state machine examples WITHOUT context
 */

#include "StateMachine.hpp"
#include <iostream>

// Simple traffic light example - NO context needed
enum TrafficLight
{
    RED,
    YELLOW,
    GREEN
};

class RedState : public StateMachine::State
{
public:
    bool enter() override {
        std::cout << "🔴 RED - Stop!" << std::endl;
        return true;
    }
    
    void update() override {
        static int counter = 0;
        if (++counter >= 3) {
            counter = 0;
            changeToState(GREEN, "Timer expired");
        }
    }
};

class YellowState : public StateMachine::State
{
public:
    bool enter() override {
        std::cout << "🟡 YELLOW - Prepare to stop" << std::endl;
        return true;
    }
    
    void update() override {
        static int counter = 0;
        if (++counter >= 1) {
            counter = 0;
            changeToState(RED, "Timer expired");
        }
    }
};

class GreenState : public StateMachine::State
{
public:
    bool enter() override {
        std::cout << "🟢 GREEN - Go!" << std::endl;
        return true;
    }
    
    void update() override {
        static int counter = 0;
        if (++counter >= 4) {
            counter = 0;
            changeToState(YELLOW, "Timer expired");
        }
    }
};

int main() {
    std::cout << " Simple Traffic Light (No Context Required)" << std::endl;
    
    // Create state machine WITHOUT any context
    StateMachine trafficSM(RED, "TrafficLight");

    trafficSM
        .addState<RedState>(RED, "Red")
        .addState<YellowState>(YELLOW, "Yellow") 
        .addState<GreenState>(GREEN, "Green")
        .onStateChanged([](const int&, const int&, 
                          auto fromName, auto toName, auto) {
            std::cout << "Light changed: " << fromName << " → " << toName << std::endl;
        });

    trafficSM.start(); // No context needed!

    // Run for 20 updates
    for (int i = 0; i < 20; ++i) {
        std::cout << "Update " << i+1 << ": ";
        trafficSM.update();
    }

    std::cout << "\n Simple state machine works perfectly!" << std::endl;
    return 0;
}
