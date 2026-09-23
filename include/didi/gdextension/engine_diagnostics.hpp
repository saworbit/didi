#pragma once

#include "didi/common/json.hpp"
#include "didi/common/types.hpp"

#include <cstddef>
#include <string_view>

namespace didi::godot {

// What the engine printed while a call was running, put on that call's answer.
//
// Godot writes its ERROR and WARNING lines to its own console, and until this
// the only reader of that console was a person watching it. A call could
// answer success while the engine printed an error for the same work -- an
// import that failed, a script that did not parse, an undo history the editor
// called inconsistent -- and neither a caller nor any test saw the line. Vibe
// session seventeen found three defects that way, all in a console pasted by
// hand. editor_save_scene already did this for its own call (#683); this is the
// same move for every call.
//
// `records` are records from the engine output ring at warning level or above,
// in order. A success answer gets `engine_diagnostics` beside its other fields;
// a refusal gets it under `error.data`, where the rest of the refusal's facts
// are. An answer that already carries `engine_diagnostics` keeps its own, which
// is how editor_save_scene's thumbnail note survives. At most `limit` records
// are attached, and `engine_diagnostics_omitted` says how many were not.
constexpr size_t kMaxEngineDiagnostics = 8;
constexpr size_t kMaxEngineDiagnosticBytes = 1024;

void attachEngineDiagnostics(json& response, const json& records, size_t total,
                             size_t limit = kMaxEngineDiagnostics);

// Methods whose answer is the engine's output, or which account for the
// engine's errors themselves, and so must not have them attached twice.
bool methodReportsEngineOutputItself(std::string_view method);

} // namespace didi::godot
