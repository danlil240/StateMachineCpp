/**
 * @file simple_example.cpp
 * @brief Simple state machine examples WITHOUT context
 */

#include "StateMachine.hpp"
#include <iostream>

// Simple traffic light example with enum class for type safety
enum class TrafficLight
{
    RED,
    YELLOW,
    GREEN
};

class RedState : public StateMachine<TrafficLight>::State
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
            changeToState(TrafficLight::GREEN, "Timer expired");
        }
    }
};

class YellowState : public StateMachine<TrafficLight>::State
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
            changeToState(TrafficLight::RED, "Timer expired");
        }
    }
};

class GreenState : public StateMachine<TrafficLight>::State
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
            changeToState(TrafficLight::YELLOW, "Timer expired");
        }
    }
};

int main() {
    std::cout << " Simple Traffic Light (Templated with enum class)" << std::endl;
    
    // Create state machine with type-safe enum class
    StateMachine<TrafficLight> trafficSM(TrafficLight::RED, "TrafficLight");

    trafficSM
        .addState<RedState>(TrafficLight::RED, "Red")
        .addState<YellowState>(TrafficLight::YELLOW, "Yellow") 
        .addState<GreenState>(TrafficLight::GREEN, "Green")
        .onStateChanged([](const TrafficLight&, const TrafficLight&, 
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
