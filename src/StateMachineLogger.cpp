#include "StateMachineLogger.hpp"

void StateMachineLogger::ConsoleLogger::log(StateMachineTypes::LogLevel level, std::string_view color, 
                                          std::string_view machineName, std::string_view message)
{
    std::cout << color << "[" << machineName << "] " << message << RESET << std::endl;
}

std::unique_ptr<StateMachineLogger::ILogger> StateMachineLogger::createConsoleLogger()
{
    return std::make_unique<ConsoleLogger>();
}

std::unique_ptr<StateMachineLogger::ILogger> StateMachineLogger::createSilentLogger()
{
    return std::make_unique<SilentLogger>();
}

std::string_view StateMachineLogger::getColorForLevel(StateMachineTypes::LogLevel level)
{
    switch (level)
    {
        case StateMachineTypes::LogLevel::ERROR:
            return RED;
        case StateMachineTypes::LogLevel::WARN:
            return YELLOW;
        case StateMachineTypes::LogLevel::INFO:
            return BLUE;
        case StateMachineTypes::LogLevel::DEBUG:
            return CYAN;
        case StateMachineTypes::LogLevel::NONE:
        default:
            return RESET;
    }
}

bool StateMachineLogger::shouldLog(StateMachineTypes::LogLevel currentLevel, StateMachineTypes::LogLevel messageLevel)
{
    return currentLevel >= messageLevel;
}
