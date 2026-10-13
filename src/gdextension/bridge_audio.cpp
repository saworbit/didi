#include "bridge_internal.hpp"
#include "didi/gdextension/godot_bridge.hpp"
#include "didi/runtime/audio_requests.hpp"
#include <tuple>
#include <utility>

namespace didi::godot {
using namespace bridge_internal;

// The audio reads answer an editor or a game, and a game has no
// EditorInterface, so they run before the lookup below. Behind it every
// game request failed on "Can't retrieve singleton 'EditorInterface'
// outside of editor" and audio_list_buses fell back to reading the file
// (vibe session nineteen).
json GodotBridge::bridgeAudioListBuses(const std::string& /*method*/, const json& /*params*/, const std::string& /*session_kind*/) {
    // Every method hash below is identical on Godot 4.5.1, 4.6.2 and 4.7.2,
    // checked by dumping extension_api.json from each, so this needs no
    // per-version branch. AudioServer is a core singleton and is present
    // whether or not an editor scene is open.
    auto server = singleton("AudioServer");
    if (server.isErr()) return errorJson(server.error());

    auto count_value = callObject(server.value(), "AudioServer", "get_bus_count", 3905245786LL);
    if (count_value.isErr()) return errorJson(count_value.error());
    auto count = scalarFromVariant<int64_t>(count_value.value(), GDEXTENSION_VARIANT_TYPE_INT);
    if (count.isErr()) return errorJson(count.error());

    json buses = json::array();
    for (int64_t index = 0; index < count.value(); ++index) {
        auto bus_index = makeScalar(GDEXTENSION_VARIANT_TYPE_INT, index);
        if (bus_index.isErr()) return errorJson(bus_index.error());

        auto name_value = callObject(server.value(), "AudioServer", "get_bus_name", 844755477LL,
                                     {&bus_index.value()});
        if (name_value.isErr()) return errorJson(name_value.error());
        auto name = stringFromVariant(name_value.value(), GDEXTENSION_VARIANT_TYPE_STRING);
        if (name.isErr()) return errorJson(name.error());

        auto volume_value = callObject(server.value(), "AudioServer", "get_bus_volume_db",
                                       2339986948LL, {&bus_index.value()});
        if (volume_value.isErr()) return errorJson(volume_value.error());
        auto volume = scalarFromVariant<double>(volume_value.value(), GDEXTENSION_VARIANT_TYPE_FLOAT);
        if (volume.isErr()) return errorJson(volume.error());

        const auto boolOf = [&](const char* method_name, int64_t hash) -> Result<bool> {
            auto value = callObject(server.value(), "AudioServer", method_name, hash,
                                    {&bus_index.value()});
            if (value.isErr()) return value.error();
            return scalarFromVariant<bool>(value.value(), GDEXTENSION_VARIANT_TYPE_BOOL);
        };
        auto mute = boolOf("is_bus_mute", 1116898809LL);
        if (mute.isErr()) return errorJson(mute.error());
        auto solo = boolOf("is_bus_solo", 1116898809LL);
        if (solo.isErr()) return errorJson(solo.error());
        auto bypass = boolOf("is_bus_bypassing_effects", 1116898809LL);
        if (bypass.isErr()) return errorJson(bypass.error());

        auto send_value = callObject(server.value(), "AudioServer", "get_bus_send", 659327637LL,
                                     {&bus_index.value()});
        if (send_value.isErr()) return errorJson(send_value.error());
        auto send = stringFromVariant(send_value.value(), GDEXTENSION_VARIANT_TYPE_STRING_NAME);
        if (send.isErr()) {
            send = stringFromVariant(send_value.value(), GDEXTENSION_VARIANT_TYPE_STRING);
            if (send.isErr()) return errorJson(send.error());
        }

        auto effect_count_value = callObject(server.value(), "AudioServer",
                                             "get_bus_effect_count", 3744713108LL,
                                             {&bus_index.value()});
        if (effect_count_value.isErr()) {
            return errorJson(effect_count_value.error());
        }
        auto effect_count =
            scalarFromVariant<int64_t>(effect_count_value.value(), GDEXTENSION_VARIANT_TYPE_INT);
        if (effect_count.isErr()) return errorJson(effect_count.error());

        // The effect chain is the part the offline layout file cannot
        // report, so it is the reason to attach an editor at all.
        json effects = json::array();
        for (int64_t slot = 0; slot < effect_count.value(); ++slot) {
            auto slot_index = makeScalar(GDEXTENSION_VARIANT_TYPE_INT, slot);
            if (slot_index.isErr()) return errorJson(slot_index.error());
            auto effect = callObject(server.value(), "AudioServer", "get_bus_effect", 726064442LL,
                                     {&bus_index.value(), &slot_index.value()});
            if (effect.isErr()) return errorJson(effect.error());
            auto class_value = callVariant(effect.value(), "get_class");
            std::string class_name = "AudioEffect";
            if (class_value.isOk()) {
                auto text = stringFromVariant(class_value.value(), GDEXTENSION_VARIANT_TYPE_STRING);
                if (text.isOk()) class_name = text.value();
            }
            effects.push_back({{"slot", slot}, {"class", class_name}});
        }

        buses.push_back({{"index", index},
                         {"name", name.value()},
                         {"volume_db", volume.value()},
                         {"mute", mute.value()},
                         {"solo", solo.value()},
                         {"bypass_effects", bypass.value()},
                         {"send", send.value()},
                         {"effects", std::move(effects)}});
    }

    return liveResult({{"status", "success"},
                       {"bus_count", count.value()},
                       {"buses", std::move(buses)}});
}


json GodotBridge::bridgeAudioConfigureBus(const std::string& /*method*/, const json& params, const std::string& session_kind) {
    auto server = singleton("AudioServer");
    if (server.isErr()) return errorJson(server.error());

    auto count_value = callObject(server.value(), "AudioServer", "get_bus_count", 3905245786LL);
    if (count_value.isErr()) return errorJson(count_value.error());
    auto count = scalarFromVariant<int64_t>(count_value.value(), GDEXTENSION_VARIANT_TYPE_INT);
    if (count.isErr()) return errorJson(count.error());

    // A bus can be named or numbered. Names are what a person uses and what
    // the layout file records; indices are what AudioServer takes. Resolving
    // a name through the engine rather than through the layout file means a
    // bus added at runtime is still addressable.
    int64_t index = -1;
    const json bus_field = params.contains("bus") ? params["bus"] : json();
    if (bus_field.is_string()) {
        const auto wanted = bus_field.get<std::string>();
        auto name_variant = makeString(wanted);
        if (name_variant.isErr()) return errorJson(name_variant.error());
        auto found = callObject(server.value(), "AudioServer", "get_bus_index", 2458036349LL,
                                {&name_variant.value()});
        if (found.isErr()) return errorJson(found.error());
        auto resolved = scalarFromVariant<int64_t>(found.value(), GDEXTENSION_VARIANT_TYPE_INT);
        if (resolved.isErr()) return errorJson(resolved.error());
        index = resolved.value();
        if (index < 0) return errorJson(404, "No audio bus is named " + wanted);
    } else if (bus_field.is_number_integer()) {
        index = bus_field.get<int64_t>();
    } else {
        return errorJson(400, "bus must be a bus name or a bus index");
    }
    if (index < 0 || index >= count.value()) {
        return errorJson(404, "Audio bus index " + std::to_string(index) +
                                  " is out of range; this project has " +
                                  std::to_string(count.value()) + " buses");
    }

    auto bus_index = makeScalar(GDEXTENSION_VARIANT_TYPE_INT, index);
    if (bus_index.isErr()) return errorJson(bus_index.error());

    const auto readState = [&]() -> Result<json> {
        auto name_value = callObject(server.value(), "AudioServer", "get_bus_name", 844755477LL,
                                     {&bus_index.value()});
        if (name_value.isErr()) return name_value.error();
        auto name = stringFromVariant(name_value.value(), GDEXTENSION_VARIANT_TYPE_STRING);
        if (name.isErr()) return name.error();
        auto volume_value = callObject(server.value(), "AudioServer", "get_bus_volume_db",
                                       2339986948LL, {&bus_index.value()});
        if (volume_value.isErr()) return volume_value.error();
        auto volume = scalarFromVariant<double>(volume_value.value(), GDEXTENSION_VARIANT_TYPE_FLOAT);
        if (volume.isErr()) return volume.error();
        auto mute_value = callObject(server.value(), "AudioServer", "is_bus_mute", 1116898809LL,
                                     {&bus_index.value()});
        if (mute_value.isErr()) return mute_value.error();
        auto mute = scalarFromVariant<bool>(mute_value.value(), GDEXTENSION_VARIANT_TYPE_BOOL);
        if (mute.isErr()) return mute.error();
        auto solo_value = callObject(server.value(), "AudioServer", "is_bus_solo", 1116898809LL,
                                     {&bus_index.value()});
        if (solo_value.isErr()) return solo_value.error();
        auto solo = scalarFromVariant<bool>(solo_value.value(), GDEXTENSION_VARIANT_TYPE_BOOL);
        if (solo.isErr()) return solo.error();
        return json{{"index", index},
                    {"name", name.value()},
                    {"volume_db", volume.value()},
                    {"mute", mute.value()},
                    {"solo", solo.value()}};
    };

    // Read before anything is written. These values are the only way back,
    // because bus state is not part of the edited scene and the editor undo
    // stack does not carry it.
    auto before = readState();
    if (before.isErr()) return errorJson(before.error());

    json applied = json::array();
    if (params.contains("volume_db")) {
        const auto& value = params["volume_db"];
        if (!value.is_number()) return errorJson(400, "volume_db must be a number");
        const double db = value.get<double>();
        // The engine bus editor spans -80 to 24 decibels. Outside that a
        // caller is either confusing decibels with a linear gain or has
        // slipped a digit, and clamping silently would hide both.
        if (!(db >= -80.0 && db <= 24.0)) {
            return errorJson(400, "volume_db must be between -80 and 24 decibels");
        }
        auto db_variant = makeScalar(GDEXTENSION_VARIANT_TYPE_FLOAT, db);
        if (db_variant.isErr()) return errorJson(db_variant.error());
        auto set = callObject(server.value(), "AudioServer", "set_bus_volume_db", 1602489585LL,
                              {&bus_index.value(), &db_variant.value()});
        if (set.isErr()) return errorJson(set.error());
        applied.push_back("volume_db");
    }

    const auto applyFlag = [&](const char* field, const char* method_name,
                               int64_t hash) -> Result<bool> {
        if (!params.contains(field)) return false;
        const auto& value = params[field];
        if (!value.is_boolean()) {
            return Error::invalidArgument(std::string(field) + " must be a boolean");
        }
        auto flag = makeScalar(GDEXTENSION_VARIANT_TYPE_BOOL, value.get<bool>());
        if (flag.isErr()) return flag.error();
        auto set = callObject(server.value(), "AudioServer", method_name, hash,
                              {&bus_index.value(), &flag.value()});
        if (set.isErr()) return set.error();
        return true;
    };
    auto muted = applyFlag("mute", "set_bus_mute", 300928843LL);
    if (muted.isErr()) return errorJson(muted.error());
    if (muted.value()) applied.push_back("mute");
    auto soloed = applyFlag("solo", "set_bus_solo", 300928843LL);
    if (soloed.isErr()) return errorJson(soloed.error());
    if (soloed.value()) applied.push_back("solo");

    if (applied.empty()) {
        return errorJson(400, "Give at least one of volume_db, mute or solo to change");
    }

    auto after = readState();
    if (after.isErr()) return errorJson(after.error());

    // Where the change ends up, which is not where this handler leaves it.
    //
    // undo_redo_registered: false beside a revert_with block is the shape of
    // "this lives in memory, here is how to put it back", and in an attached
    // editor that is not the whole story. The editor's own bus-layout
    // autosave notices the AudioServer change and writes
    // res://default_bus_layout.tres a moment later with no call from here,
    // so a tracked project file appears in the working tree carrying
    // whatever value was tried last. A caller sweeping bus volumes to find
    // a mix reasonably believed nothing had been committed (#622).
    //
    // Confirmed on 4.5.1: the file is absent from the integration fixture
    // and present in the copy after a run whose only bus calls are this
    // tool's.
    const bool editor_session = session_kind == "editor";
    // The file the editor writes, which is the one it opened the project
    // with. A literal res://default_bus_layout.tres named a file that is
    // never written in a project that moved its layout (#935), and the
    // setting itself names the wrong one once it has moved underneath a
    // running editor.
    const auto project_layout_path =
        busLayoutPathFrom(projectSettingString("audio/buses/default_bus_layout"));
    const auto layout_path = editor_session
        ? startupBusLayoutPath().value_or(project_layout_path)
        : project_layout_path;
    json result = {
        {"status", "success"},
        {"bus", index},
        {"applied", std::move(applied)},
        {"before", before.value()},
        {"after", after.value()},
        {"undo_redo_registered", false},
        {"persisted_by_editor", editor_session},
        {"layout_path", layout_path},
        {"limitation",
         editor_session
             ? "This sets the running engine's audio bus state. This tool writes no "
               "file, but the editor's own bus-layout autosave picks the change up and "
               "writes " + layout_path + " shortly afterwards, so it reaches "
               "the project on disk anyway. revert_with restores the previous values." +
                   busLayoutMovedSentence(layout_path, project_layout_path)
             : "This sets the running game's audio bus state and nothing writes it "
               "down. It is gone when the process exits, and the project's bus layout "
               "on disk is unchanged. revert_with restores the previous values."},
        {"revert_with", before.value()}};
    if (project_layout_path != layout_path) result["project_layout_path"] = project_layout_path;
    return liveResult(std::move(result));
}


json GodotBridge::bridgeAudioAddBus(const std::string& /*method*/, const json& params, const std::string& /*session_kind*/, GDExtensionObjectPtr /*editor*/) {
    // One bus appended to the layout the editor holds, named, routed, and
    // left for the editor's own bus-layout autosave to write down. Every
    // fact below was measured on 4.5.1, 4.6.2 and 4.7.2 by
    // tools/vibe/probes/audio_bus_engine.py, and every hash here is the
    // same on all three (#771).
    auto parsed = runtime::parseAudioAddBusRequest(params);
    if (parsed.isErr()) {
        return errorJson(parsed.error().code, parsed.error().message, parsed.error().data);
    }
    const auto& request = parsed.value();
    for (const auto& bind : {std::make_tuple("AudioServer", "get_bus_count", 3905245786LL),
                             std::make_tuple("AudioServer", "get_bus_name", 844755477LL),
                             std::make_tuple("AudioServer", "get_bus_send", 659327637LL),
                             std::make_tuple("AudioServer", "add_bus", 1025054187LL),
                             std::make_tuple("AudioServer", "remove_bus", 1286410249LL),
                             std::make_tuple("AudioServer", "set_bus_name", 501894301LL),
                             std::make_tuple("AudioServer", "set_bus_send", 3780747571LL),
                             std::make_tuple("Object", "emit_signal", 4047867050LL)}) {
        if (requireMethodBind(std::get<0>(bind), std::get<1>(bind), std::get<2>(bind)).isErr()) {
            return bridgeError(501, "required_bind_unavailable");
        }
    }
    auto server = singleton("AudioServer");
    if (server.isErr()) return errorJson(server.error());

    const auto indexVariant = [](int64_t index) {
        return makeScalar(GDEXTENSION_VARIANT_TYPE_INT, index);
    };
    const auto busCount = [&]() -> Result<int64_t> {
        auto value = callObject(server.value(), "AudioServer", "get_bus_count", 3905245786LL);
        if (value.isErr()) return value.error();
        return scalarFromVariant<int64_t>(value.value(), GDEXTENSION_VARIANT_TYPE_INT);
    };
    const auto nameOf = [&](int64_t index) -> Result<std::string> {
        auto at = indexVariant(index);
        if (at.isErr()) return at.error();
        auto value = callObject(server.value(), "AudioServer", "get_bus_name", 844755477LL,
                                {&at.value()});
        if (value.isErr()) return value.error();
        return stringFromVariant(value.value(), GDEXTENSION_VARIANT_TYPE_STRING);
    };
    const auto sendOf = [&](int64_t index) -> Result<std::string> {
        auto at = indexVariant(index);
        if (at.isErr()) return at.error();
        auto value = callObject(server.value(), "AudioServer", "get_bus_send", 659327637LL,
                                {&at.value()});
        if (value.isErr()) return value.error();
        auto send = stringFromVariant(value.value(), GDEXTENSION_VARIANT_TYPE_STRING_NAME);
        if (send.isErr()) send = stringFromVariant(value.value(), GDEXTENSION_VARIANT_TYPE_STRING);
        return send;
    };
    // ASCII only. It is there to catch `music` beside `Music`, which the
    // engine keeps as two buses and `get_bus_index` tells apart by case.
    const auto folded = [](std::string text) {
        for (auto& character : text) {
            if (character >= 'A' && character <= 'Z') character = static_cast<char>(character - 'A' + 'a');
        }
        return text;
    };

    auto count = busCount();
    if (count.isErr()) return errorJson(count.error());
    std::vector<std::string> names;
    std::vector<std::string> sends;
    for (int64_t index = 0; index < count.value(); ++index) {
        auto name = nameOf(index);
        if (name.isErr()) return errorJson(name.error());
        auto send = sendOf(index);
        if (send.isErr()) return errorJson(send.error());
        names.push_back(name.value());
        sends.push_back(send.value());
    }

    // The engine never refuses a name. One already in use comes back as
    // `Music 2`, and `Master` on another bus as `Master 2`, with nothing
    // printed, so the name the caller asked for would belong to the other
    // bus. The check is against the engine, not the layout file, because
    // the editor holds the layout in memory and writes it when it likes.
    for (const auto& existing : names) {
        if (existing == request.name) {
            return errorJson(409, "A bus named \"" + request.name + "\" already exists. Godot "
                                  "would name the new one \"" + request.name + " 2\" rather "
                                  "than refuse, so this is refused instead. "
                                  "audio_configure_bus changes the bus that is there.",
                             {{"code", "bus_name_in_use"}, {"name", request.name},
                              {"buses", names}, {"retryable", false}});
        }
    }
    for (const auto& existing : names) {
        if (folded(existing) == folded(request.name)) {
            return errorJson(409, "\"" + request.name + "\" differs from the existing bus \"" +
                                  existing + "\" only in letter case. Godot would keep both, "
                                  "and nobody can tell them apart in the Audio panel.",
                             {{"code", "bus_name_differs_only_in_case"}, {"name", request.name},
                              {"existing", existing}, {"buses", names}, {"retryable", false}});
        }
    }

    // A send the engine cannot use still reads back as set, and the sound
    // goes to Master with no message: a later bus, a bus that does not
    // exist, the bus itself and the empty send all did, measured with a
    // tone on every bus's peak meter. The new bus goes on the end, so every
    // bus already there comes before it and is a target that works.
    if (request.send == request.name) {
        return errorJson(400, "A bus cannot send to itself. Godot would store the send and "
                              "route the bus to Master.",
                         {{"code", "invalid_arguments"}, {"parameter", "send"},
                          {"retry_with", {{"send", "Master"}}}, {"retryable", false}});
    }
    bool send_found = false;
    std::optional<std::string> send_meant;
    for (const auto& existing : names) {
        if (existing == request.send) send_found = true;
        else if (!send_meant && folded(existing) == folded(request.send)) send_meant = existing;
    }
    if (!send_found) {
        json data = {{"code", "send_bus_not_found"}, {"parameter", "send"},
                     {"send", request.send}, {"buses", names}, {"retryable", false}};
        std::string message = "No bus is named \"" + request.send + "\", so there is nothing "
                              "to send to. Godot would store the send and route the bus to "
                              "Master.";
        if (send_meant) {
            data["did_you_mean"] = *send_meant;
            data["retry_with"] = {{"send", *send_meant}};
            message += " The bus \"" + *send_meant + "\" differs only in letter case.";
        }
        return errorJson(404, message, std::move(data));
    }

    // A send is kept by name and does not follow a rename, so a project
    // can already hold one that names no bus. Adding a bus with that name
    // routes those buses through it, and that changes a mix the caller did
    // not name.
    json adopted = json::array();
    for (size_t index = 0; index < names.size(); ++index) {
        if (sends[index] == request.name) adopted.push_back(names[index]);
    }

    if (request.preview) {
        return liveResult({{"status", "success"},
                           {"preview", true},
                           {"index", count.value()},
                           {"name", request.name},
                           {"send", request.send},
                           {"buses", names},
                           {"adopted_sends", std::move(adopted)}});
    }

    auto at_end = indexVariant(-1);
    if (at_end.isErr()) return errorJson(at_end.error());
    auto added = callObject(server.value(), "AudioServer", "add_bus", 1025054187LL,
                            {&at_end.value()});
    if (added.isErr()) return errorJson(added.error());
    auto grown = busCount();
    if (grown.isErr()) return errorJson(grown.error());
    if (grown.value() != count.value() + 1) {
        return errorJson(500, "AudioServer.add_bus did not add a bus: the layout has " +
                                  std::to_string(grown.value()) + " buses, expected " +
                                  std::to_string(count.value() + 1) + ".");
    }
    const int64_t index = count.value();
    auto bus_index = indexVariant(index);
    if (bus_index.isErr()) return errorJson(bus_index.error());
    const auto removeAdded = [&]() {
        (void)callObject(server.value(), "AudioServer", "remove_bus", 1286410249LL,
                         {&bus_index.value()});
    };

    auto name_variant = makeString(request.name);
    if (name_variant.isErr()) {
        removeAdded();
        return errorJson(name_variant.error());
    }
    auto named = callObject(server.value(), "AudioServer", "set_bus_name", 501894301LL,
                            {&bus_index.value(), &name_variant.value()});
    if (named.isErr()) {
        removeAdded();
        return errorJson(named.error());
    }
    // Read back rather than trusted. Something else in the editor can add a
    // bus between the check above and this call, and the engine would
    // answer that with `Music 2` rather than an error. The engine can also
    // change a name on its way in: its UTF-8 reader drops a leading
    // byte-order mark, and this said "another bus took the name meanwhile",
    // retryable, for a name no bus held, which every retry repeated.
    // Which of the two it was is read off the layout once the bus is gone.
    auto kept = nameOf(index);
    if (kept.isErr() || kept.value() != request.name) {
        removeAdded();
        bool asked_name_taken = false;
        if (auto remaining = busCount(); remaining.isOk()) {
            for (int64_t other = 0; other < remaining.value(); ++other) {
                auto other_name = nameOf(other);
                if (other_name.isOk() && other_name.value() == request.name) asked_name_taken = true;
            }
        }
        const auto refusal = runtime::busNameChangedRefusal(
            request.name, kept.isOk() ? kept.value() : std::string("?"), asked_name_taken);
        return errorJson(refusal.code, refusal.message, refusal.data);
    }

    auto send_variant = makeStringName(request.send);
    if (send_variant.isErr()) {
        removeAdded();
        return errorJson(send_variant.error());
    }
    auto routed = callObject(server.value(), "AudioServer", "set_bus_send", 3780747571LL,
                             {&bus_index.value(), &send_variant.value()});
    if (routed.isErr()) {
        removeAdded();
        return errorJson(routed.error());
    }
    if (request.volume_db) {
        auto db = makeScalar(GDEXTENSION_VARIANT_TYPE_FLOAT, *request.volume_db);
        if (db.isOk()) {
            (void)callObject(server.value(), "AudioServer", "set_bus_volume_db", 1602489585LL,
                             {&bus_index.value(), &db.value()});
        }
    }
    for (const auto& [value, method_name] :
         {std::make_pair(request.mute, "set_bus_mute"), std::make_pair(request.solo, "set_bus_solo")}) {
        if (!value) continue;
        auto flag = makeScalar(GDEXTENSION_VARIANT_TYPE_BOOL, *value);
        if (flag.isOk()) {
            (void)callObject(server.value(), "AudioServer", method_name, 300928843LL,
                             {&bus_index.value(), &flag.value()});
        }
    }

    // On 4.5.1 and 4.6.2 the editor's Audio panel rebuilds when a bus is
    // added and does not follow a rename, so the new strip says New Bus.
    // A person who clicks into that name field and away renames the bus
    // back to New Bus through the panel, and every player set to the name
    // given here goes to Master. This signal makes the panel rebuild with
    // the right name on all three lines, and it changed no name, send,
    // volume or effect when measured.
    bool panel_refreshed = false;
    auto signal_name = makeStringName("bus_layout_changed");
    if (signal_name.isOk()) {
        std::vector<const VariantValue*> signal_arguments{&signal_name.value()};
        auto emitted = callObject(server.value(), "Object", "emit_signal", 4047867050LL,
                                  signal_arguments);
        panel_refreshed = emitted.isOk();
    }

    const auto readState = [&]() -> Result<json> {
        auto name = nameOf(index);
        if (name.isErr()) return name.error();
        auto send = sendOf(index);
        if (send.isErr()) return send.error();
        auto volume = callObject(server.value(), "AudioServer", "get_bus_volume_db",
                                 2339986948LL, {&bus_index.value()});
        if (volume.isErr()) return volume.error();
        auto db = scalarFromVariant<double>(volume.value(), GDEXTENSION_VARIANT_TYPE_FLOAT);
        if (db.isErr()) return db.error();
        const auto flagOf = [&](const char* method_name) -> Result<bool> {
            auto value = callObject(server.value(), "AudioServer", method_name, 1116898809LL,
                                    {&bus_index.value()});
            if (value.isErr()) return value.error();
            return scalarFromVariant<bool>(value.value(), GDEXTENSION_VARIANT_TYPE_BOOL);
        };
        auto mute = flagOf("is_bus_mute");
        if (mute.isErr()) return mute.error();
        auto solo = flagOf("is_bus_solo");
        if (solo.isErr()) return solo.error();
        return json{{"index", index}, {"name", name.value()}, {"send", send.value()},
                    {"volume_db", db.value()}, {"mute", mute.value()}, {"solo", solo.value()}};
    };
    auto after = readState();
    if (after.isErr()) return errorJson(after.error());

    // The file the editor writes is the one it opened the project with,
    // which is the one the project names unless the setting moved while
    // the editor was running.
    const auto project_layout_path =
        busLayoutPathFrom(projectSettingString("audio/buses/default_bus_layout"));
    const auto layout_path = startupBusLayoutPath().value_or(project_layout_path);
    json result = {
        {"status", "success"},
        {"bus", index},
        {"name", request.name},
        {"send", request.send},
        {"after", after.value()},
        {"bus_count", grown.value()},
        {"adopted_sends", std::move(adopted)},
        {"layout_path", layout_path},
        {"persisted_by_editor", true},
        {"panel_refreshed", panel_refreshed},
        {"undo_redo_registered", false},
        {"limitation",
         "This adds the bus to the editor's running AudioServer and writes no file. The "
         "editor's own bus-layout autosave writes " + layout_path + " a moment later. There "
         "is no undo entry: delete the bus in the editor's Audio panel to take it back." +
             busLayoutMovedSentence(layout_path, project_layout_path)},
        {"next_step",
         "Set each AudioStreamPlayer's bus to \"" + request.name + "\" with "
         "scene_set_property, then editor_save_scene. A player set to a bus before the bus "
         "exists reads Master and is saved as Master."}};
    if (project_layout_path != layout_path) result["project_layout_path"] = project_layout_path;
    return liveResult(std::move(result));
}


} // namespace didi::godot
