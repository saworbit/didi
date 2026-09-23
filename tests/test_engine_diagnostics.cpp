#include "didi/common/json.hpp"
#include "didi/common/types.hpp"
#include "didi/gdextension/engine_diagnostics.hpp"

#include <functional>
#include <stdexcept>
#include <string>

#define ASSERT_TRUE(cond) if (!(cond)) throw std::runtime_error("Assertion failed: " #cond);
#define ASSERT_EQ(a, b) ASSERT_TRUE((a) == (b))

void registerTest(const std::string& name, std::function<void()> fn);

namespace {

using didi::json;
using didi::godot::attachEngineDiagnostics;

json record(const std::string& level, const std::string& message, json details = json()) {
    return {{"sequence", 1}, {"level", level}, {"source", "godot"}, {"message", message},
            {"details", details}};
}

// A success answer gets the lines beside its own fields, with where in the
// engine each came from when the engine said.
void success_answer_carries_the_lines() {
    json response = {{"status", "success"}, {"imported", json::array({"res://a.png"})}};
    attachEngineDiagnostics(response,
                            json::array({record("error", "Error importing 'res://a.png'.",
                                                {{"function", "_reimport_file"},
                                                 {"file", "editor/file_system/editor_file_system.cpp"},
                                                 {"line", 3120}})}),
                            1);
    ASSERT_EQ(response["status"], json("success"));
    ASSERT_EQ(response["engine_diagnostics"].size(), 1u);
    ASSERT_EQ(response["engine_diagnostics"][0]["level"], json("error"));
    ASSERT_EQ(response["engine_diagnostics"][0]["message"], json("Error importing 'res://a.png'."));
    ASSERT_EQ(response["engine_diagnostics"][0]["function"], json("_reimport_file"));
    ASSERT_EQ(response["engine_diagnostics"][0]["line"], json(3120));
    ASSERT_TRUE(response.contains("engine_diagnostics_note"));
    ASSERT_TRUE(!response.contains("engine_diagnostics_omitted"));
}

// A refusal carries them under error.data, where a caller already reads the
// rest of what went wrong, and the envelope is otherwise untouched.
void refusal_carries_them_under_data() {
    json response = {{"error", {{"code", 404}, {"message", "No resource at res://x.tres"}}}};
    attachEngineDiagnostics(response, json::array({record("warning", "careful")}), 1);
    ASSERT_EQ(response["error"]["code"], json(404));
    ASSERT_EQ(response["error"]["data"]["engine_diagnostics"][0]["level"], json("warning"));
    ASSERT_TRUE(!response.contains("engine_diagnostics"));
}

// Nothing printed, nothing added: an answer from a quiet call keeps the shape
// it always had, so no client sees a new empty field on every call.
void quiet_call_is_unchanged() {
    json response = {{"status", "success"}};
    const json before = response;
    attachEngineDiagnostics(response, json::array(), 0);
    ASSERT_EQ(response, before);
}

// Bounded, and the cut is disclosed rather than silent.
void many_lines_are_capped_and_counted() {
    json records = json::array();
    for (int index = 0; index < 8; ++index) records.push_back(record("error", "e" + std::to_string(index)));
    json response = json::object();
    attachEngineDiagnostics(response, records, 30);
    ASSERT_EQ(response["engine_diagnostics"].size(), 8u);
    ASSERT_EQ(response["engine_diagnostics_omitted"], json(22));
}

// A long message is cut on a character boundary, so the answer stays valid
// UTF-8 for a strict reader.
void long_message_is_cut_on_a_character() {
    std::string message(1023, 'a');
    message += "\xC3\xA9\xC3\xA9";  // two two-byte characters straddling the cap
    json response = json::object();
    attachEngineDiagnostics(response, json::array({record("error", message)}), 1);
    const auto cut = response["engine_diagnostics"][0]["message"].get<std::string>();
    ASSERT_EQ(cut.size(), 1023u);
    ASSERT_EQ(response["engine_diagnostics"][0]["message_truncated"], json(true));
    ASSERT_TRUE(!json(cut).dump().empty());
}

// editor_save_scene attaches its own, with a note about headless thumbnails,
// and that one is the better answer; it is not overwritten.
void existing_diagnostics_are_kept() {
    json response = {{"status", "saved"}, {"engine_diagnostics", json::array({"own"})}};
    attachEngineDiagnostics(response, json::array({record("error", "other")}), 1);
    ASSERT_EQ(response["engine_diagnostics"], json::array({"own"}));
}

void output_readers_are_excluded() {
    ASSERT_TRUE(didi::godot::methodReportsEngineOutputItself("runtime.getOutput"));
    ASSERT_TRUE(didi::godot::methodReportsEngineOutputItself("runtime.watchInvariants"));
    ASSERT_TRUE(!didi::godot::methodReportsEngineOutputItself("asset.reimport"));
}

struct Register {
    Register() {
        registerTest("EngineDiagnostics.SuccessCarriesLines", success_answer_carries_the_lines);
        registerTest("EngineDiagnostics.RefusalCarriesThemUnderData", refusal_carries_them_under_data);
        registerTest("EngineDiagnostics.QuietCallUnchanged", quiet_call_is_unchanged);
        registerTest("EngineDiagnostics.CappedAndCounted", many_lines_are_capped_and_counted);
        registerTest("EngineDiagnostics.CutOnACharacter", long_message_is_cut_on_a_character);
        registerTest("EngineDiagnostics.ExistingKept", existing_diagnostics_are_kept);
        registerTest("EngineDiagnostics.OutputReadersExcluded", output_readers_are_excluded);
    }
} registrar;

} // namespace
