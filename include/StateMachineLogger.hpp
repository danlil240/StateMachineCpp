#ifndef STATE_MACHINE_LOGGER_HPP
#define STATE_MACHINE_LOGGER_HPP

#include "StateMachineTypes.hpp"
#include <string_view>
#include <iostream>

/**
 * @brief Logging interface and implementation for StateMachine
 */
class StateMachineLogger
{
public:
    // ANSI color codes for terminal output
    static constexpr std::string_view RESET = "\033[0m";
    static constexpr std::string_view BOLD = "\033[1m";
    static constexpr std::string_view STRIKETHROUGH = "\033[9m";
    static constexpr std::string_view RED = "\033[31m";
    static constexpr std::string_view GREEN = "\033[32m";
    static constexpr std::string_view YELLOW = "\033[33m";
    static constexpr std::string_view BLUE = "\033[34m";
    static constexpr std::string_view MAGENTA = "\033[35m";
    static constexpr std::string_view CYAN = "\033[36m";

    /**
     * @brief Interface for custom loggers
     */
    class ILogger
    {
    public:
        virtual ~ILogger() = default;
        virtual void log(StateMachineTypes::LogLevel level, std::string_view machineName, std::string_view message) = 0;
    };

    /**
     * @brief Default console logger implementation
     */
    class ConsoleLogger : public ILogger
    {
    public:
        void log(StateMachineTypes::LogLevel level, std::string_view machineName, std::string_view message) override
        {
            auto color = getColorForLevel(level);
            std::cout << color << "[" << machineName << "] " << message << RESET << std::endl;
        }
    };

    /**
     * @brief Silent logger (no output)
     */
    class SilentLogger : public ILogger
    {
    public:
        void log(StateMachineTypes::LogLevel /* level */,
                 std::string_view /* machineName */,
                 std::string_view /* message */) override
        {
        }
    };

    /**
     * @brief Create default console logger
     */
    static std::unique_ptr<ILogger> createConsoleLogger() {
        return std::make_unique<ConsoleLogger>();
    }

    /**
     * @brief Create silent logger
     */
    static std::unique_ptr<ILogger> createSilentLogger() {
        return std::make_unique<SilentLogger>();
    }
    
    /**
     * @brief Helper method to get color for log level
     */
    static std::string_view getColorForLevel(StateMachineTypes::LogLevel level) {
        switch (level) {
            case StateMachineTypes::LogLevel::ERROR:
                return RED;
            case StateMachineTypes::LogLevel::WARN:
                return YELLOW;
            case StateMachineTypes::LogLevel::INFO:
                return BLUE;
            case StateMachineTypes::LogLevel::DEBUG:
                return MAGENTA;
            case StateMachineTypes::LogLevel::NONE:
            default:
                return RESET;
        }
    }
    
    /**
     * @brief Check if level should be logged based on current log level
     */
    static bool shouldLog(StateMachineTypes::LogLevel currentLevel, StateMachineTypes::LogLevel messageLevel) {
        return currentLevel >= messageLevel;
    }
};

#endif // STATE_MACHINE_LOGGER_HPP
