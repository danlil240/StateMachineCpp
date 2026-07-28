#ifndef ROS_COMMON2_STATE_MACHINE_ROS_HPP
#define ROS_COMMON2_STATE_MACHINE_ROS_HPP

// Opt-in ROS 2 adapter for StateMachine<StateID>.
//
// Include this header (in addition to StateMachine.hpp) only from translation
// units that already depend on rclcpp. StateMachine.hpp itself stays
// rclcpp-free, so non-ROS consumers — including this package's own unit tests —
// never pay for the dependency.

#include "StateMachineLogging.hpp"

#include <rclcpp/logger.hpp>
#include <rclcpp/logging.hpp>

#include <string>
#include <string_view>
#include <utility>

namespace sm_ros {

/**
 * @brief Build a log sink that forwards state machine output to an rclcpp logger
 *
 * Maps the machine's levels onto the matching RCLCPP_* severities so FSM output
 * lands in rosout instead of stdout. Colors are passed through as-is; call
 * withColors(false) if the destination should stay ANSI-free.
 *
 * The logger is captured by value, so the sink does not depend on the lifetime
 * of the node that created it.
 *
 * @code
 *   sm.withLogSink(sm_ros::logSink(get_logger()));
 * @endcode
 *
 * Records already start with the machine's own "[name] " tag and the logger
 * supplies the node name, so no prefix is added by default.
 *
 * @param logger Logger to write to, usually node->get_logger()
 * @param prefix Optional extra text prepended to every record
 * @return A sink suitable for StateMachine<...>::withLogSink()
 */
inline sm_detail::StateMachineLogging::LogSink
logSink(rclcpp::Logger logger, std::string prefix = "") {
  using LogLevel = sm_detail::StateMachineLogging::LogLevel;

  return [logger = std::move(logger), prefix = std::move(prefix)](
             LogLevel level, std::string_view message) {
    // string_view is not guaranteed null-terminated, so bound it with "%.*s".
    const int length = static_cast<int>(message.size());
    switch (level) {
    case LogLevel::ERROR:
      RCLCPP_ERROR(logger, "%s%.*s", prefix.c_str(), length, message.data());
      break;
    case LogLevel::WARN:
      RCLCPP_WARN(logger, "%s%.*s", prefix.c_str(), length, message.data());
      break;
    case LogLevel::DEBUG:
      RCLCPP_DEBUG(logger, "%s%.*s", prefix.c_str(), length, message.data());
      break;
    case LogLevel::NONE:
      break; // never emitted; NONE is only a threshold
    case LogLevel::INFO:
    default:
      RCLCPP_INFO(logger, "%s%.*s", prefix.c_str(), length, message.data());
      break;
    }
  };
}

} // namespace sm_ros

#endif // ROS_COMMON2_STATE_MACHINE_ROS_HPP
