#pragma once

#include "didi/common/json.hpp"
#include "didi/common/types.hpp"

#include <optional>
#include <string>

namespace didi {
namespace runtime {

// One bus to append to the running engine's layout. Parsed by the server before
// anything is sent and again by the bridge, so a request is refused by name in
// the same words whichever side sees it first.
struct AudioAddBusRequest {
    std::string name;
    // Master unless the caller names another bus. The engine gives a new bus
    // an empty send, which routes exactly as Master does; this spells it.
    std::string send{"Master"};
    std::optional<double> volume_db;
    std::optional<bool> mute;
    std::optional<bool> solo;
    // Set by the server's dry-run probe, never by a client: the published
    // schema does not carry it. Everything is checked and nothing is changed.
    bool preview{false};
};

// 400 on an unknown key or on a name the engine would keep and a caller could
// not tell apart from another one.
//
// `AudioServer.set_bus_name` never refuses a name. Measured on 4.5.1, 4.6.2 and
// 4.7.2 by tools/vibe/probes/audio_bus_engine.py, it keeps the empty name,
// spaces only, a leading or trailing space, a tab and a line break, and all of
// them survive a saved layout. So these rules are Didi's: `Music ` is a second
// bus nobody can tell from `Music` in the Audio panel, and a control character
// has no reason to be in a name, which is the rule project_add_export_preset
// applies to preset names. At most 256 characters, the bound preset names have
// and the unit the schema's maxLength counts in; the engine kept 300 letters.
//
// The rules are about what a person sees, not about ASCII. Vibe session
// nineteen found a no-break space, an ideographic space, a zero width space
// and U+0085 all accepted, and a byte-order mark dropped by the engine's own
// UTF-8 reader on the way in: " Music" and "Music" behind a zero width space
// both look like "Music" in the Audio panel. So a space is anything Unicode
// calls white space, an invisible character is one drawn as nothing, and a
// control character includes the C1 range, the line and paragraph separators
// and the bidirectional controls.
//
// A name in use and a send to a bus that does not exist are the engine's
// question, not this one: they depend on the layout the editor holds now.
Result<AudioAddBusRequest> parseAudioAddBusRequest(const json& params);

// The refusal for a bus the engine named differently from the name it was
// given, after the bus has been removed again. `asked_name_taken` is whether a
// bus holds the asked-for name once this one is gone: then something else in
// the editor added it between the check and the call, and the engine answered
// with "Music 2". Otherwise the engine changed the name on its way in, which a
// retry repeats. Neither is retryable: the first is a name in use, the second a
// name that cannot be added as written.
Error busNameChangedRefusal(const std::string& asked, const std::string& stored,
                            bool asked_name_taken);

} // namespace runtime
} // namespace didi
