#pragma once

#include <cstdint>
#include <optional>
#include <string>

// Godot's connection flags, and the set signal_connect will write.
//
// The rule used to be `flags == 2`, in five places, on the premise that "no
// other value survives a scene save predictably". The engine disagrees.
// Connecting with each combination, packing, saving, loading with
// CACHE_MODE_IGNORE and reading get_signal_connection_list back, on 4.5.1,
// 4.6.2 and 4.7.2:
//
//   asked 2  persist                    saved with no flags= line
//   asked 3  persist|deferred           saved flags=3
//   asked 6  persist|one_shot           saved flags=6
//   asked 7  persist|deferred|one_shot  saved flags=7
//
// All four round-trip exactly, and 3, 6 and 7 are what the editor's own Connect
// dialog writes when Deferred or One Shot is ticked. Refusing them meant a
// connection a user authored could be read through the surface and removed
// through the surface and not put back (#852).
//
// Without CONNECT_PERSIST nothing is saved at all: asked 1, 4, 8, 16 and 32 the
// engine writes no `[connection]` line. So the persist bit stays required. A
// connection this tool made that vanished on the next scene load would be work
// reported as done that did not last.
//
// What the read-back value carries depends on how the scene was instantiated,
// which is the fact that makes a comparison wrong where a mask is right:
//
//   GEN_EDIT_STATE_MAIN      the scene the editor has open   2, 3, 6, 7
//   GEN_EDIT_STATE_INSTANCE  a sub-scene instanced inside it 34, 35, 38, 39
//
// Bit 32 is CONNECT_INHERITED, the engine's own note that the connection came
// from an instanced scene rather than from a live connect(). It is provenance,
// not a setting: the engine adds it on instantiate and strips it on save, and
// asking for 34 produces a file byte-identical to asking for 2. A caller
// reconciling connections reads it back and writes it straight back, so it is
// accepted and masked off rather than refused.
namespace didi::connection_flags {

inline constexpr int64_t kDeferred = 1;
inline constexpr int64_t kPersist = 2;
inline constexpr int64_t kOneShot = 4;
inline constexpr int64_t kReferenceCounted = 8;
inline constexpr int64_t kAppendSourceObject = 16;
inline constexpr int64_t kInherited = 32;

// The part of a flags value a caller chose, with the engine's provenance bit
// removed. This is what goes to connect() and what a response reports.
inline constexpr int64_t authored(int64_t flags) { return flags & ~kInherited; }

// Whether the connection is stored in the saved .tscn. The one question
// `origin: scene` is keyed on, and the reason a mask is not a comparison
// (#768).
inline constexpr bool isPersistent(int64_t flags) { return (flags & kPersist) != 0; }

// The flags signal_connect will write: CONNECT_PERSIST, optionally with
// CONNECT_DEFERRED and CONNECT_ONE_SHOT.
inline constexpr int64_t kConnectableMask = kPersist | kDeferred | kOneShot;

inline constexpr bool isConnectable(int64_t flags) {
    const auto asked = authored(flags);
    return isPersistent(asked) && (asked & ~kConnectableMask) == 0;
}

// The sentence for a value signal_connect will not write.
//
// The refusal used to be the bare identifier `invalid_signal_connect_request`,
// with `flags` named nowhere, so a caller who passed 3 could not tell whether
// the problem was the flag, the method, the node or the signal (#406, #424).
inline std::optional<std::string> refuseConnectFlags(int64_t flags) {
    if (isConnectable(flags)) return std::nullopt;
    const auto asked = authored(flags);
    std::string reason = "Argument 'flags' is " + std::to_string(flags) + ", which ";
    if (!isPersistent(asked)) {
        reason +=
            "does not include 2, CONNECT_PERSIST. A connection without it is not stored in the "
            "scene file, so it would vanish on the next load and this tool would have reported "
            "work that did not last.";
    } else if ((asked & kAppendSourceObject) != 0) {
        reason +=
            "includes 16, CONNECT_APPEND_SOURCE_OBJECT. That appends the emitter to the "
            "arguments, so the method needs one more than the signal declares, and this tool "
            "checks the method against the signal's own arity before it connects. Measured on "
            "4.7.2: with that flag a zero-argument signal never reaches a zero-argument method.";
    } else if ((asked & kReferenceCounted) != 0) {
        reason +=
            "includes 8, CONNECT_REFERENCE_COUNTED. That flag only counts a callable connected "
            "more than once, and signal_connect answers 409 for a callable that is already "
            "connected, so the count can never rise above one here.";
    } else {
        reason += "sets a bit Godot does not define as a connection flag.";
    }
    reason +=
        " Accepted: 2 (CONNECT_PERSIST), 3 (with CONNECT_DEFERRED), 6 (with CONNECT_ONE_SHOT), "
        "7 (with both), and each of those plus 32 (CONNECT_INHERITED) as signal_list_connections "
        "reports it for a connection inside an instanced scene.";
    return reason;
}

} // namespace didi::connection_flags
