#ifndef ROS_COMMON2_STATE_MACHINE_ISUBMACHINE_HPP
#define ROS_COMMON2_STATE_MACHINE_ISUBMACHINE_HPP

#include "StateMachineLogging.hpp"

#include <string>

/**
 * @brief Type-erased interface for a child state machine.
 *
 * Allows a parent StateMachine to own and drive a child StateMachine
 * of a different StateID type without knowing the child's enum type.
 */
class ISubMachine {
public:
  using LogLevel = sm_detail::StateMachineLogging::LogLevel;
  using LogSink = sm_detail::StateMachineLogging::LogSink;
  using ClockFn = sm_detail::StateMachineLogging::ClockFn;

  virtual ~ISubMachine() = default;
  /**
   * @brief Start the child and report whether it became active
   */
  virtual bool start() = 0;
  virtual void update() = 0;
  virtual void stop() = 0;
  virtual std::string activeStateName() const = 0;
  virtual bool isActive() const = 0;

  /**
   * @brief Slash-separated path of the child's active state and its descendants
   */
  virtual std::string statePath() const = 0;

  /**
   * @brief Recursively validate the child's configuration
   */
  virtual bool validate() const = 0;

  /**
   * @brief Push the parent's logging configuration down to the child
   */
  virtual void applyLogConfig(LogLevel level, LogSink sink, bool useColors) = 0;

  /**
   * @brief Push the parent's clock down to the child
   */
  virtual void applyClock(ClockFn clock) = 0;
};

#endif // ROS_COMMON2_STATE_MACHINE_ISUBMACHINE_HPP
