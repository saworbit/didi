#pragma once

// Proof in one call (Q9 in docs/BUILD_QUEUE.md, principle P7).
//
// A scenario launches one game, pauses it, drives it frame by frame and checks
// what it was asked to check, then stops the game. The steps an author writes
// are only the middle: launch and teardown are the scenario's frame, so a
// teardown cannot be left out and runs on every path, a failed one included.
//
// Everything here is pure. The run loop talks to the game through
// IScenarioDriver, which the server implements over the bridge methods a game
// session already admits and a test implements with a script. The record half
// reads and hashes project files under a root it is given, never the process's
// working directory, so a test can point it at a directory of its own.

#include "didi/common/json.hpp"
#include "didi/common/types.hpp"

#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <optional>
#include <string>
#include <vector>

namespace didi::runtime {

struct ScenarioStep {
    enum class Kind { wait, wait_until, press, assert_value, assert_output, capture };
    Kind kind{Kind::wait};
    // What the author called the step, echoed in the report. Optional.
    std::string label;
    // wait: frames to run. press: frames the action is held. wait_until: the
    // most frames to run before giving up.
    int frames{0};
    // press
    std::string action;
    // assert and wait_until
    std::string expression;
    std::string context_node;
    std::optional<double> minimum;
    std::optional<double> maximum;
    // assert_output
    std::string text;
    std::string level;
    bool absent{false};

    // The wire spelling: wait, wait_until, press, assert, assert_output, capture.
    const char* kindName() const;
    bool isAssertion() const;
    // An assertion a game that did nothing would fail. An absence check is an
    // assertion, but a game that never started its loop passes it too.
    bool provesSomething() const;
};

struct ScenarioSpec {
    std::string name;
    std::string scene_path;
    bool headless{true};
    int timeout_seconds{60};
    std::vector<ScenarioStep> steps;
};

inline constexpr size_t kMaxScenarioSteps = 64;
inline constexpr int kMaxStepFrames = 600;
inline constexpr int kMaxScenarioFrames = 3600;
inline constexpr int kMinScenarioTimeoutSeconds = 5;
inline constexpr int kMaxScenarioTimeoutSeconds = 300;
// runtime.step takes 1 to 60 frames a call.
inline constexpr int kMaxFramesPerStepCall = 60;

// Validates a runtime_run_scenario call's arguments. Everything that can be
// refused without a game is refused here, before one is started: a scenario
// that proves nothing, a capture in a game that draws nothing, an expression
// the sandbox would refuse, a context path the bridge would refuse.
Result<ScenarioSpec> parseScenario(const json& arguments);

// The game a scenario drives. Every call but launch and teardown is made on a
// game the driver has paused, and none of them is made before launch succeeds.
class IScenarioDriver {
public:
    virtual ~IScenarioDriver() = default;
    // Starts the game and pauses it. The answer describes the game for the
    // report: pid, session, engine, build, the fixed frame rate it runs at.
    virtual Result<json> launch() = 0;
    // Runs 1 to 60 frames of the paused game and leaves it paused.
    virtual Result<json> step(int frames) = 0;
    // Queues an action event for the next frame that runs.
    virtual Result<json> press(const std::string& action, bool pressed) = 0;
    // The read-only sandbox's answer: value and value_type.
    virtual Result<json> evaluate(const std::string& expression,
                                  const std::string& context_node) = 0;
    // One page of the game's engine output from `cursor`, as runtime.getOutput
    // answers it: records, next_cursor, has_more, dropped_before_cursor.
    virtual Result<json> readOutput(uint64_t cursor, const std::string& minimum_level) = 0;
    // Captures the root viewport and keeps it as evidence: path, sha256,
    // width, height.
    virtual Result<json> capture(size_t step_index) = 0;
    // Stops the game if one was started and says whether it is gone. Called
    // exactly once on every path after launch was attempted, whatever launch
    // answered. An error means a game may still be running.
    virtual Result<json> teardown() = 0;
    // Whether the job running this scenario was asked to stop.
    virtual bool cancelled() const = 0;
    // Milliseconds since the run started, for the scenario's deadline.
    virtual int64_t elapsedMs() const = 0;
};

// Why a run did not pass, for the caller's error envelope.
struct ScenarioFailure {
    // assertion: a step's check did not hold (the behaviour was disproved).
    // step: a step was refused or the engine failed under it.
    // launch, teardown, cancelled, timeout: the run itself.
    std::string stage;
    // The step it happened at, when it happened at one.
    std::optional<size_t> step;
    // A short identifier: below_minimum, not_seen, never_held, ...
    std::string reason;
    std::string message;
    // The driver's error, when one caused it.
    std::optional<Error> cause;
};

struct ScenarioOutcome {
    // pass, fail or error. fail is a disproved behaviour; error is a run that
    // could not say either way.
    std::string verdict;
    json report;
    std::optional<ScenarioFailure> failure;
};

// Runs the scenario. Never throws: an exception from the driver is an error
// verdict, and teardown still runs.
ScenarioOutcome runScenario(const ScenarioSpec& spec, IScenarioDriver& driver);

// Whether an assertion's value holds: a finite number inside the bounds when
// either bound is given, boolean true when neither is. `reason` is empty when
// it holds.
struct ValueJudgement {
    bool held{false};
    std::string reason;
};
ValueJudgement judgeValue(const json& value, const std::optional<double>& minimum,
                          const std::optional<double>& maximum);

// --- What a run was true for -------------------------------------------------

struct ScenarioFile {
    // res:// spelling, or "project.godot" for the project file itself.
    std::string path;
    // Lowercase hex SHA-256 of the bytes, or empty when the file is missing.
    std::string sha256;
};

struct ScenarioFiles {
    std::vector<ScenarioFile> files;
    bool truncated{false};
};

inline constexpr size_t kMaxScenarioFiles = 256;

// project.godot, the scene, the project's autoloads, and every script, scene
// and text resource they reach: ext_resource lines in scenes and resources,
// res:// string literals in scripts and shaders, and the scripts behind the
// class_name identifiers a script uses. Textures, audio and other assets are
// not followed. Sorted by path.
ScenarioFiles collectScenarioFiles(const std::filesystem::path& project_root,
                                   const std::string& scene_path);

// The same, from any set of starting files: the test scripts a test run names,
// for project_run_tests. A .gd file also reaches every script whose class_name
// it uses. Nothing under addons/ is followed into unless it is a starting file
// or an autoload.
ScenarioFiles collectProofFiles(const std::filesystem::path& project_root,
                                const std::vector<std::string>& seeds);

// The files of `recorded` whose bytes differ now, or that are missing now, or
// that were missing then and exist now.
std::vector<ScenarioFile> changedScenarioFiles(const std::filesystem::path& project_root,
                                               const std::vector<ScenarioFile>& recorded);

json scenarioFilesJson(const ScenarioFiles& files);
// Reads the `files` array of a stored record. Entries that are not well formed
// are skipped, and `malformed` says so.
std::vector<ScenarioFile> scenarioFilesFromJson(const json& files, bool* malformed = nullptr);

// <project>/.didi/scenarios
std::filesystem::path scenarioRecordDirectory(const std::filesystem::path& project_root);

// What godot://project/scenarios answers: every stored record, newest first,
// each with whether a file it ran against has changed since.
json scenarioRecordsView(const std::filesystem::path& project_root);

}  // namespace didi::runtime
