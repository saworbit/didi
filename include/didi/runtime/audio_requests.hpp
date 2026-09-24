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
// applies to preset names. At most 256 bytes, the bound preset names have; the
// engine kept 300 letters.
//
// A name in use and a send to a bus that does not exist are the engine's
// question, not this one: they depend on the layout the editor holds now.
Result<AudioAddBusRequest> parseAudioAddBusRequest(const json& params);

} // namespace runtime
} // namespace didi
