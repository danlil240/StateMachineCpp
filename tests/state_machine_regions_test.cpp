#include <StateMachine/StateMachine.hpp>

#include <algorithm>
#include <cmath>
#include <iostream>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

namespace {

class TestRunner {
public:
  void check(bool condition, const char *expression, int line) {
    ++checks_;
    if (!condition) {
      ++failures_;
      std::cerr << "FAIL line " << line << ": " << expression << '\n';
    }
  }

  int finish() const {
    if (failures_ == 0) {
      std::cout << "PASS: " << checks_ << " checks\n";
      return 0;
    }
    std::cerr << "FAILED: " << failures_ << " of " << checks_ << " checks\n";
    return 1;
  }

private:
  int checks_{0};
  int failures_{0};
};

#define CHECK(expr) runner.check(static_cast<bool>(expr), #expr, __LINE__)

struct Trace {
  std::vector<std::string> events;

  void add(std::string event) {
    std::lock_guard<std::mutex> lock(mutex);
    events.push_back(std::move(event));
  }

  int count(const std::string &event) const {
    std::lock_guard<std::mutex> lock(mutex);
    return static_cast<int>(std::count(events.begin(), events.end(), event));
  }

  bool contains(const std::string &event) const {
    std::lock_guard<std::mutex> lock(mutex);
    return std::find(events.begin(), events.end(), event) != events.end();
  }

  bool endsWith(std::initializer_list<std::string> suffix) const {
    std::lock_guard<std::mutex> lock(mutex);
    if (suffix.size() > events.size()) {
      return false;
    }
    auto actual = events.end() - static_cast<std::ptrdiff_t>(suffix.size());
    return std::equal(suffix.begin(), suffix.end(), actual);
  }

  mutable std::mutex mutex;
};

template <typename Id>
class TraceState : public StateMachine<Id>::State {
public:
  TraceState(Trace &trace, std::string name, bool enterResult = true)
      : trace_(trace), name_(std::move(name)), enterResult_(enterResult) {}

  bool enter() override {
    trace_.add(name_ + ".enter");
    return enterResult_;
  }

  void update() override { trace_.add(name_ + ".update"); }

  void exit() override { trace_.add(name_ + ".exit"); }

private:
  Trace &trace_;
  std::string name_;
  bool enterResult_;
};

template <typename Id>
std::unique_ptr<typename StateMachine<Id>::State>
traceState(Trace &trace, std::string name, bool enterResult = true) {
  return std::make_unique<TraceState<Id>>(trace, std::move(name), enterResult);
}

enum class RootId { ROOT };
enum class Level1Id { L1 };
enum class Level2Id { L2 };
enum class Level3Id { L3 };
enum class LegacyId { ONE, TWO };

class LegacyOne final : public StateMachine<LegacyId>::State {};
class LegacyTwo final : public StateMachine<LegacyId>::State {};

void testUnlimitedDepth(TestRunner &runner) {
  Trace trace;
  StateMachine<RootId> sm(RootId::ROOT, "RootSM");
  sm.withLogLevel(StateMachine<RootId>::LogLevel::NONE)
      .addState(RootId::ROOT, "Root", traceState<RootId>(trace, "Root"))
      .withSubStates<Level1Id>(
          RootId::ROOT, Level1Id::L1,
          [&](auto &level1) {
            level1
                .addState(Level1Id::L1, "Level1",
                          traceState<Level1Id>(trace, "Level1"))
                .template withSubStates<Level2Id>(
                    Level1Id::L1, Level2Id::L2,
                    [&](auto &level2) {
                      level2
                          .addState(Level2Id::L2, "Level2",
                                    traceState<Level2Id>(trace, "Level2"))
                          .template withSubStates<Level3Id>(
                              Level2Id::L2, Level3Id::L3,
                              [&](auto &level3) {
                                level3.addState(
                                    Level3Id::L3, "Level3",
                                    traceState<Level3Id>(trace, "Level3"));
                              });
                    });
          })
      .start();

  CHECK(sm.isReady());
  CHECK(sm.validate());
  CHECK(sm.statePath() == "Root/Level1/Level2/Level3");
  CHECK(sm.getActiveSubStateName() == "Level1");

  auto *level1 = sm.getTypedSubMachine<Level1Id>();
  CHECK(level1 != nullptr);
  auto *level2 =
      level1 ? level1->getTypedSubMachine<Level2Id>() : nullptr;
  CHECK(level2 != nullptr);
  auto *level3 =
      level2 ? level2->getTypedSubMachine<Level3Id>() : nullptr;
  CHECK(level3 != nullptr);
  CHECK(level3 && level3->getCurrentStateName() == "Level3");

  sm.update();
  CHECK(trace.endsWith({"Root.update", "Level1.update", "Level2.update",
                        "Level3.update"}));

  sm.stop();
  CHECK(trace.endsWith({"Level3.exit", "Level2.exit", "Level1.exit",
                        "Root.exit"}));
}

void testLegacySubStateApis(TestRunner &runner) {
  Trace trace;
  StateMachine<RootId> simple(RootId::ROOT, "LegacySimple");
  simple.withLogLevel(StateMachine<RootId>::LogLevel::NONE)
      .addState(RootId::ROOT, "Root", traceState<RootId>(trace, "Root"))
      .addSubState<LegacyOne, LegacyId>(
          RootId::ROOT, LegacyId::ONE, "One")
      .addSubState<LegacyTwo, LegacyId>(
          RootId::ROOT, LegacyId::TWO, "Two")
      .setInitialSubState<LegacyId>(RootId::ROOT, LegacyId::TWO)
      .start();

  CHECK(simple.isReady());
  CHECK(simple.getActiveSubStateName() == "Two");
  CHECK(simple.statePath() == "Root/Two");
  CHECK(simple.changeSubState<LegacyId>(LegacyId::ONE, "legacy API"));
  CHECK(simple.getActiveSubStateName() == "One");

  auto child = std::make_unique<StateMachine<LegacyId>>(
      LegacyId::ONE, "PrebuiltChild");
  child->withLogLevel(StateMachine<LegacyId>::LogLevel::NONE)
      .addState<LegacyOne>(LegacyId::ONE, "One");

  StateMachine<RootId> prebuilt(RootId::ROOT, "LegacyPrebuilt");
  prebuilt.withLogLevel(StateMachine<RootId>::LogLevel::NONE)
      .addState(RootId::ROOT, "Root", traceState<RootId>(trace, "Root2"),
                std::move(child))
      .start();

  CHECK(prebuilt.isReady());
  CHECK(prebuilt.statePath() == "Root/One");
  CHECK(prebuilt.getTypedSubMachine<LegacyId>() != nullptr);
}

enum class MainId { FLYING, IDLE };
enum class MotionId { HOVER, CRUISE };
enum class PayloadId { IDLE, TRACKING };
enum class SafetyId { NOMINAL, EMERGENCY };
enum class NavigationId { ROUTE, AVOID };

void testParallelRegionsAndNestedRegion(TestRunner &runner) {
  Trace trace;
  StateMachine<MainId> sm(MainId::FLYING, "Drone");
  sm.withLogLevel(StateMachine<MainId>::LogLevel::NONE)
      .addState(MainId::FLYING, "Flying",
                traceState<MainId>(trace, "Flying"))
      .addState(MainId::IDLE, "Idle", traceState<MainId>(trace, "Idle"))
      .withRegion<MotionId>(
          MainId::FLYING, "Motion", MotionId::HOVER,
          [&](auto &motion) {
            motion
                .addState(MotionId::HOVER, "Hover",
                          traceState<MotionId>(trace, "Hover"))
                .addState(MotionId::CRUISE, "Cruise",
                          traceState<MotionId>(trace, "Cruise"))
                .template withRegion<NavigationId>(
                    MotionId::CRUISE, "Navigation", NavigationId::ROUTE,
                    [&](auto &navigation) {
                      navigation
                          .addState(
                              NavigationId::ROUTE, "Route",
                              traceState<NavigationId>(trace, "Route"))
                          .addState(
                              NavigationId::AVOID, "Avoid",
                              traceState<NavigationId>(trace, "Avoid"));
                    });
          })
      .withRegion<PayloadId>(
          MainId::FLYING, "Payload", PayloadId::IDLE,
          [&](auto &payload) {
            payload
                .addState(PayloadId::IDLE, "PayloadIdle",
                          traceState<PayloadId>(trace, "PayloadIdle"))
                .addState(PayloadId::TRACKING, "Tracking",
                          traceState<PayloadId>(trace, "Tracking"));
          })
      .withRegion<SafetyId>(
          MainId::FLYING, "Safety", SafetyId::NOMINAL,
          [&](auto &safety) {
            safety
                .addState(SafetyId::NOMINAL, "Nominal",
                          traceState<SafetyId>(trace, "Nominal"))
                .addState(SafetyId::EMERGENCY, "Emergency",
                          traceState<SafetyId>(trace, "Emergency"));
          })
      .start();

  CHECK(sm.isReady());
  CHECK(sm.statePath() ==
        "Flying/{Motion:Hover, Payload:PayloadIdle, Safety:Nominal}");
  const auto active = sm.getActiveRegions();
  CHECK(active.size() == 3);
  CHECK(active[0].name == "Motion");
  CHECK(active[1].name == "Payload");
  CHECK(active[2].name == "Safety");
  CHECK(sm.hasActiveRegion("Motion"));
  CHECK(!sm.hasActiveRegion("Missing"));
  CHECK(sm.getActiveRegionStateName("Payload") == "PayloadIdle");

  CHECK(sm.changeRegionState<MotionId>("Motion", MotionId::CRUISE,
                                       "airspeed reached"));
  CHECK(sm.getActiveRegionStateName("Motion") == "Cruise");
  CHECK(sm.getActiveRegionStateName("Payload") == "PayloadIdle");
  CHECK(sm.statePath() ==
        "Flying/{Motion:Cruise/{Navigation:Route}, "
        "Payload:PayloadIdle, Safety:Nominal}");

  auto *motion = sm.getRegionMachine<MotionId>("Motion");
  CHECK(motion != nullptr);
  auto *navigation =
      motion ? motion->getRegionMachine<NavigationId>("Navigation") : nullptr;
  CHECK(navigation != nullptr);
  CHECK(navigation &&
        navigation->changeState(NavigationId::AVOID, "obstacle"));
  CHECK(sm.statePath() ==
        "Flying/{Motion:Cruise/{Navigation:Avoid}, "
        "Payload:PayloadIdle, Safety:Nominal}");

  trace.events.clear();
  sm.update();
  CHECK(trace.events ==
        std::vector<std::string>(
            {"Flying.update", "Cruise.update", "Avoid.update",
             "PayloadIdle.update", "Nominal.update"}));

  trace.events.clear();
  CHECK(sm.changeState(MainId::IDLE, "landed"));
  CHECK(trace.events ==
        std::vector<std::string>(
            {"Nominal.exit", "PayloadIdle.exit", "Avoid.exit",
             "Cruise.exit", "Flying.exit", "Idle.enter"}));

  trace.events.clear();
  sm.update();
  CHECK(trace.events == std::vector<std::string>({"Idle.update"}));
}

enum class SharedRegionId { A, B };

class RegionController final : public StateMachine<RootId>::State {
public:
  explicit RegionController(bool &changed) : changed_(changed) {}

  void update() override {
    changed_ = changeRegionState<SharedRegionId>(
        "Controlled", SharedRegionId::B, "state convenience API");
    observedName_ = getActiveRegionStateName("Controlled");
    observedMachine_ =
        getRegionMachine<SharedRegionId>("Controlled") != nullptr;
  }

  const std::string &observedName() const { return observedName_; }
  bool observedMachine() const { return observedMachine_; }

private:
  bool &changed_;
  std::string observedName_;
  bool observedMachine_{false};
};

void testStateRegionConvenienceApis(TestRunner &runner) {
  Trace trace;
  bool changed = false;
  auto controller = std::make_unique<RegionController>(changed);
  RegionController *controllerRaw = controller.get();

  StateMachine<RootId> sm(RootId::ROOT, "StateConvenience");
  sm.withLogLevel(StateMachine<RootId>::LogLevel::NONE)
      .addState(RootId::ROOT, "Root", std::move(controller))
      .withRegion<SharedRegionId>(
          RootId::ROOT, "Controlled", SharedRegionId::A,
          [&](auto &region) {
            region
                .addState(SharedRegionId::A, "A",
                          traceState<SharedRegionId>(trace, "A"))
                .addState(SharedRegionId::B, "B",
                          traceState<SharedRegionId>(trace, "B"));
          })
      .start();

  sm.update();
  CHECK(changed);
  CHECK(controllerRaw->observedName() == "B");
  CHECK(controllerRaw->observedMachine());
  CHECK(sm.getActiveRegionStateName("Controlled") == "B");
}

void testSameTypeRegionDisambiguationAndConcurrency(TestRunner &runner) {
  Trace trace;
  StateMachine<RootId> sm(RootId::ROOT, "SharedTypes");
  sm.withLogLevel(StateMachine<RootId>::LogLevel::NONE)
      .addState(RootId::ROOT, "Root", traceState<RootId>(trace, "Root"))
      .withRegion<SharedRegionId>(
          RootId::ROOT, "Left", SharedRegionId::A,
          [&](auto &left) {
            left
                .addState(SharedRegionId::A, "LeftA",
                          traceState<SharedRegionId>(trace, "LeftA"))
                .addState(SharedRegionId::B, "LeftB",
                          traceState<SharedRegionId>(trace, "LeftB"));
          })
      .withRegion<SharedRegionId>(
          RootId::ROOT, "Right", SharedRegionId::A,
          [&](auto &right) {
            right
                .addState(SharedRegionId::A, "RightA",
                          traceState<SharedRegionId>(trace, "RightA"))
                .addState(SharedRegionId::B, "RightB",
                          traceState<SharedRegionId>(trace, "RightB"));
          })
      .start();

  CHECK(sm.getRegionMachine<SharedRegionId>("Left") != nullptr);
  CHECK(sm.getRegionMachine<SharedRegionId>("Right") != nullptr);
  CHECK(sm.getRegionMachine<MotionId>("Left") == nullptr);

  bool leftOk = false;
  bool rightOk = false;
  std::thread left([&] {
    leftOk = sm.changeRegionState<SharedRegionId>(
        "Left", SharedRegionId::B, "left thread");
  });
  std::thread right([&] {
    rightOk = sm.changeRegionState<SharedRegionId>(
        "Right", SharedRegionId::B, "right thread");
  });
  left.join();
  right.join();

  CHECK(leftOk);
  CHECK(rightOk);
  CHECK(sm.getActiveRegionStateName("Left") == "LeftB");
  CHECK(sm.getActiveRegionStateName("Right") == "RightB");
}

enum class GoodId { GOOD };
enum class BadId { BAD };

void testOptionalRegionFailure(TestRunner &runner) {
  Trace trace;
  StateMachine<RootId> sm(RootId::ROOT, "OptionalFailure");
  sm.withLogLevel(StateMachine<RootId>::LogLevel::NONE)
      .addState(RootId::ROOT, "Root", traceState<RootId>(trace, "Root"))
      .withRegion<GoodId>(
          RootId::ROOT, "Required", GoodId::GOOD,
          [&](auto &good) {
            good.addState(GoodId::GOOD, "Good",
                          traceState<GoodId>(trace, "Good"));
          })
      .withRegion<BadId>(
          RootId::ROOT, "Optional", BadId::BAD,
          [&](auto &bad) {
            bad.addState(BadId::BAD, "Bad",
                         traceState<BadId>(trace, "Bad", false));
          },
          false)
      .start();

  CHECK(sm.isReady());
  CHECK(sm.hasActiveRegion("Required"));
  CHECK(!sm.hasActiveRegion("Optional"));
  CHECK(trace.count("Bad.enter") == 1);
  CHECK(trace.count("Bad.exit") == 1);
  CHECK(trace.count("Good.exit") == 0);
}

void testRequiredRegionFailureIsTransactional(TestRunner &runner) {
  Trace trace;
  StateMachine<RootId> sm(RootId::ROOT, "RequiredFailure");
  sm.withLogLevel(StateMachine<RootId>::LogLevel::NONE)
      .addState(RootId::ROOT, "Root", traceState<RootId>(trace, "Root"))
      .withRegion<GoodId>(
          RootId::ROOT, "First", GoodId::GOOD,
          [&](auto &good) {
            good.addState(GoodId::GOOD, "Good",
                          traceState<GoodId>(trace, "Good"));
          })
      .withRegion<BadId>(
          RootId::ROOT, "Second", BadId::BAD,
          [&](auto &bad) {
            bad.addState(BadId::BAD, "Bad",
                         traceState<BadId>(trace, "Bad", false));
          })
      .start();

  CHECK(!sm.isReady());
  CHECK(trace.events ==
        std::vector<std::string>({"Root.enter", "Good.enter", "Bad.enter",
                                  "Bad.exit", "Good.exit", "Root.exit"}));
  CHECK(sm.getTransitionHistory().empty());
}

enum class RecoveryRootId { IDLE, ACTIVE };

void testTransitionRegionFailureRollsBack(TestRunner &runner) {
  Trace trace;
  StateMachine<RecoveryRootId> sm(RecoveryRootId::IDLE, "Recovery");
  sm.withLogLevel(StateMachine<RecoveryRootId>::LogLevel::NONE)
      .addState(RecoveryRootId::IDLE, "Idle",
                traceState<RecoveryRootId>(trace, "Idle"))
      .addState(RecoveryRootId::ACTIVE, "Active",
                traceState<RecoveryRootId>(trace, "Active"))
      .withRegion<BadId>(
          RecoveryRootId::ACTIVE, "RequiredBad", BadId::BAD,
          [&](auto &bad) {
            bad.addState(BadId::BAD, "Bad",
                         traceState<BadId>(trace, "Bad", false));
          })
      .start();

  trace.events.clear();
  CHECK(!sm.changeState(RecoveryRootId::ACTIVE));
  CHECK(sm.isReady());
  CHECK(sm.getCurrentStateId() == RecoveryRootId::IDLE);
  CHECK(trace.events ==
        std::vector<std::string>({"Idle.exit", "Active.enter", "Bad.enter",
                                  "Bad.exit", "Active.exit", "Idle.enter"}));
  CHECK(sm.getTransitionHistory().size() == 1);
}

enum class InvalidChildId { EXPECTED, REGISTERED };

void testRecursiveValidationAndFrozenTopology(TestRunner &runner) {
  Trace trace;
  StateMachine<RootId> invalid(RootId::ROOT, "Invalid");
  invalid.withLogLevel(StateMachine<RootId>::LogLevel::NONE)
      .withStrictConfig()
      .addState(RootId::ROOT, "Root", traceState<RootId>(trace, "Root"))
      .withRegion<InvalidChildId>(
          RootId::ROOT, "InvalidDeep", InvalidChildId::EXPECTED,
          [&](auto &child) {
            child.addState(
                InvalidChildId::REGISTERED, "Registered",
                traceState<InvalidChildId>(trace, "Registered"));
          });

  CHECK(!invalid.validate());
  invalid.start();
  CHECK(!invalid.isReady());

  StateMachine<RootId> frozen(RootId::ROOT, "Frozen");
  frozen.withLogLevel(StateMachine<RootId>::LogLevel::NONE)
      .addState(RootId::ROOT, "Root", traceState<RootId>(trace, "FrozenRoot"))
      .start();
  frozen.withRegion<GoodId>(
      RootId::ROOT, "TooLate", GoodId::GOOD,
      [&](auto &region) {
        region.addState(GoodId::GOOD, "Good",
                        traceState<GoodId>(trace, "LateGood"));
      });
  CHECK(frozen.getRegionMachine<GoodId>("TooLate") == nullptr);
}

void testClockAndLoggingPropagation(TestRunner &runner) {
  Trace trace;
  double clock = 42.0;
  std::vector<std::string> logs;

  StateMachine<RootId> sm(RootId::ROOT, "Propagation");
  sm.addState(RootId::ROOT, "Root", traceState<RootId>(trace, "Root"))
      .withRegion<GoodId>(
          RootId::ROOT, "Outer", GoodId::GOOD,
          [&](auto &outer) {
            outer
                .addState(GoodId::GOOD, "Good",
                          traceState<GoodId>(trace, "Good"))
                .template withSubStates<Level1Id>(
                    GoodId::GOOD, Level1Id::L1,
                    [&](auto &inner) {
                      inner.addState(
                          Level1Id::L1, "Inner",
                          traceState<Level1Id>(trace, "Inner"));
                    });
          })
      .withClock([&] { return clock; })
      .withColors(false)
      .withLogSink([&](auto, std::string_view message) {
        logs.emplace_back(message);
      })
      .withLogLevel(StateMachine<RootId>::LogLevel::DEBUG)
      .start();

  auto *outer = sm.getRegionMachine<GoodId>("Outer");
  auto *inner =
      outer ? outer->getTypedSubMachine<Level1Id>() : nullptr;
  CHECK(outer != nullptr);
  CHECK(inner != nullptr);
  CHECK(outer && std::abs(outer->now() - 42.0) < 1e-9);
  CHECK(inner && std::abs(inner->now() - 42.0) < 1e-9);

  clock = 45.5;
  CHECK(inner && std::abs(inner->timeInState() - 3.5) < 1e-9);
  CHECK(std::any_of(logs.begin(), logs.end(), [](const std::string &record) {
    return record.find("[Good/Sub]") != std::string::npos;
  }));
  CHECK(std::none_of(logs.begin(), logs.end(), [](const std::string &record) {
    return record.find('\033') != std::string::npos;
  }));
}

enum class ExitId { A, B, C };

class ExitTransitionAttempt final : public StateMachine<ExitId>::State {
public:
  explicit ExitTransitionAttempt(Trace &trace) : trace_(trace) {}

  bool enter() override {
    trace_.add("A.enter");
    return true;
  }

  void exit() override {
    trace_.add("A.exit");
    changeToState(ExitId::C, "not allowed from exit");
  }

private:
  Trace &trace_;
};

void testExitTransitionIsDeferred(TestRunner &runner) {
  Trace trace;
  StateMachine<ExitId> sm(ExitId::A, "ExitGuard");
  sm.withLogLevel(StateMachine<ExitId>::LogLevel::NONE)
      .addState(ExitId::A, "A",
                std::make_unique<ExitTransitionAttempt>(trace))
      .addState(ExitId::B, "B", traceState<ExitId>(trace, "B"))
      .addState(ExitId::C, "C", traceState<ExitId>(trace, "C"))
      .start();

  CHECK(sm.changeState(ExitId::B));
  CHECK(sm.getCurrentStateId() == ExitId::C);
  CHECK(trace.count("C.enter") == 1);
}

enum class ReenterId { A };

class FailSecondEnter final : public StateMachine<ReenterId>::State {
public:
  bool enter() override {
    ++enters_;
    return enters_ != 2;
  }

private:
  int enters_{0};
};

void testFailedReentryPreservesHistory(TestRunner &runner) {
  StateMachine<ReenterId> sm(ReenterId::A, "History");
  sm.withLogLevel(StateMachine<ReenterId>::LogLevel::NONE)
      .addState(ReenterId::A, "A", std::make_unique<FailSecondEnter>())
      .start();

  CHECK(sm.getTransitionHistory().size() == 1);
  CHECK(!sm.reenterState());
  CHECK(sm.isReady());
  CHECK(sm.getTransitionHistory().size() == 1);
}

enum class TimeoutId { A, B };

void testTimeoutAndCallbackUseCurrentGeneration(TestRunner &runner) {
  Trace trace;
  double clock = 0.0;
  TimeoutId callbackState = TimeoutId::A;
  StateMachine<TimeoutId> sm(TimeoutId::A, "Timeout");
  sm.withLogLevel(StateMachine<TimeoutId>::LogLevel::NONE)
      .withClock([&] { return clock; })
      .addState(TimeoutId::A, "A", traceState<TimeoutId>(trace, "A"))
      .addState(TimeoutId::B, "B", traceState<TimeoutId>(trace, "B"))
      .withStateTimeout(TimeoutId::A, 1.0, TimeoutId::B)
      .onStateUpdated([&](const TimeoutId &state, std::string_view) {
        callbackState = state;
      })
      .start();

  clock = 2.0;
  sm.update();
  CHECK(sm.getCurrentStateId() == TimeoutId::B);
  CHECK(callbackState == TimeoutId::B);
}

void testDuplicateRegionName(TestRunner &runner) {
  Trace trace;
  StateMachine<RootId> sm(RootId::ROOT, "Duplicate");
  sm.withLogLevel(StateMachine<RootId>::LogLevel::NONE)
      .addState(RootId::ROOT, "Root", traceState<RootId>(trace, "Root"))
      .withRegion<GoodId>(
          RootId::ROOT, "Region", GoodId::GOOD,
          [&](auto &region) {
            region.addState(GoodId::GOOD, "First",
                            traceState<GoodId>(trace, "First"));
          })
      .withRegion<GoodId>(
          RootId::ROOT, "Region", GoodId::GOOD,
          [&](auto &region) {
            region.addState(GoodId::GOOD, "Second",
                            traceState<GoodId>(trace, "Second"));
          })
      .start();

  CHECK(sm.isReady());
  CHECK(sm.getActiveRegions().size() == 1);
  CHECK(sm.getActiveRegionStateName("Region") == "First");
  CHECK(trace.count("Second.enter") == 0);
}

} // namespace

int main() {
  TestRunner runner;

  testUnlimitedDepth(runner);
  testLegacySubStateApis(runner);
  testParallelRegionsAndNestedRegion(runner);
  testStateRegionConvenienceApis(runner);
  testSameTypeRegionDisambiguationAndConcurrency(runner);
  testOptionalRegionFailure(runner);
  testRequiredRegionFailureIsTransactional(runner);
  testTransitionRegionFailureRollsBack(runner);
  testRecursiveValidationAndFrozenTopology(runner);
  testClockAndLoggingPropagation(runner);
  testExitTransitionIsDeferred(runner);
  testFailedReentryPreservesHistory(runner);
  testTimeoutAndCallbackUseCurrentGeneration(runner);
  testDuplicateRegionName(runner);

  return runner.finish();
}