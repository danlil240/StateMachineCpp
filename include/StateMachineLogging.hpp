#ifndef ROS_COMMON2_STATE_MACHINE_LOGGING_HPP
#define ROS_COMMON2_STATE_MACHINE_LOGGING_HPP

#include <chrono>
#include <functional>
#include <string_view>

namespace sm_detail {

/**
 * @brief Non-template facilities shared by every StateMachine<StateID>.
 *
 * Keeping this vocabulary out of the class template avoids duplicating it per
 * instantiation and, more importantly, makes LogLevel/LogSink/ClockFn single
 * types, so a parent machine can propagate its configuration to a child of a
 * different StateID type without casting through int.
 */
class StateMachineLogging {
public:
  /**
   * @brief Logging levels for state machine output
   */
  enum class LogLevel {
    NONE,  // No logging
    ERROR, // Only errors
    WARN,  // Warnings and errors
    INFO,  // Info, warnings, and errors
    DEBUG  // All logging including debug info
  };

  /**
   * @brief Color options for state machine output
   */
  enum class Color {
    RESET,         // Reset color
    BOLD,          // Bold text
    STRIKETHROUGH, // Strikethrough text
    RED,           // Red text
    GREEN,         // Green text
    YELLOW,        // Yellow text
    BLUE,          // Blue text
    MAGENTA,       // Magenta text
    CYAN           // Cyan text
  };

  /**
   * @brief Sink for formatted log records.
   *
   * Receives the fully formatted message, colored according to withColors().
   * Disable colors when the destination should stay ANSI-free.
   */
  using LogSink = std::function<void(LogLevel, std::string_view)>;

  /**
   * @brief Monotonic time source, in seconds.
   *
   * Injectable so ROS nodes can supply node->now().seconds() and tests can
   * supply a virtual clock.
   */
  using ClockFn = std::function<double()>;

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

  // Targeted "off" codes. Ending a span with these instead of RESET turns off
  // just that one attribute, so an inner span does not clear the color of the
  // line it sits inside.
  static constexpr std::string_view NOT_BOLD = "\033[22m";
  static constexpr std::string_view NOT_STRIKETHROUGH = "\033[29m";
  static constexpr std::string_view DEFAULT_FG = "\033[39m";

  /**
   * @brief Convert Color enum to its ANSI escape sequence
   */
  static constexpr std::string_view colorToString(Color color) {
    switch (color) {
    case Color::RESET:
      return RESET;
    case Color::BOLD:
      return BOLD;
    case Color::STRIKETHROUGH:
      return STRIKETHROUGH;
    case Color::RED:
      return RED;
    case Color::GREEN:
      return GREEN;
    case Color::YELLOW:
      return YELLOW;
    case Color::BLUE:
      return BLUE;
    case Color::MAGENTA:
      return MAGENTA;
    case Color::CYAN:
      return CYAN;
    default:
      return BLUE;
    }
  }

  /**
   * @brief The ANSI sequence that undoes colorToString(color), and nothing else
   *
   * Attributes have dedicated off-codes (22 for bold, 29 for strikethrough) and
   * foreground colors revert with 39, so a span can be closed without disturbing
   * the styling around it.
   */
  static constexpr std::string_view colorOffToString(Color color) {
    switch (color) {
    case Color::RESET:
      return RESET;
    case Color::BOLD:
      return NOT_BOLD;
    case Color::STRIKETHROUGH:
      return NOT_STRIKETHROUGH;
    case Color::RED:
    case Color::GREEN:
    case Color::YELLOW:
    case Color::BLUE:
    case Color::MAGENTA:
    case Color::CYAN:
    default:
      return DEFAULT_FG;
    }
  }

  /**
   * @brief Default clock: steady_clock since epoch, in seconds
   */
  static double steadyClockSeconds() {
    return std::chrono::duration<double>(
               std::chrono::steady_clock::now().time_since_epoch())
        .count();
  }
};

} // namespace sm_detail

#endif // ROS_COMMON2_STATE_MACHINE_LOGGING_HPP
