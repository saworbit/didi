#pragma once

#include "didi/common/types.hpp"
#include "didi/mcp/mcp_protocol.hpp"

#include <functional>
#include <mutex>
#include <optional>
#include <string>
#include <vector>

namespace didi::mcp {

// Response economy (Q5 in docs/BUILD_QUEUE.md, #776).
//
// Every tools/call answer carries its payload twice, once parsed in
// structuredContent and once serialised into content[0].text, and every live
// answer restates the session descriptor the caller already obtained when it
// attached. Over #776's seven-call authoring arc that was 68% of the wire. Both
// are the right default, because a client that reads only `content` needs the
// text and a client that never attached needs the descriptor, so neither
// changes unless the client asks. A client asks by declaring this extension in
// its capabilities, which is also where it declares the MCP Apps extension:
// once in `initialize` for a 2024-11-05 client, on each request for a modern
// one.
//
//   "extensions": {"didi/responseEconomy": {"omit": ["textCopy", "sessionDescriptor"]}}
//
// The server declares the same extension, listing what it honours, so a client
// can check before relying on it. A value the server does not know is ignored
// rather than refused: a capability is a statement of what the client can
// handle, and a client written against a later Didi should still get what this
// one can give.
inline constexpr char kResponseEconomyExtension[] = "didi/responseEconomy";
inline constexpr char kOmitTextCopy[] = "textCopy";
inline constexpr char kOmitSessionDescriptor[] = "sessionDescriptor";

struct ResponseEconomy {
    // Leave out a text item that is byte for byte the structuredContent.
    bool omit_text_copy{false};
    // Send the session descriptor only when the client does not already hold
    // it, and a reference to it otherwise.
    bool reference_session{false};

    bool any() const { return omit_text_copy || reference_session; }
    ResponseEconomy operator|(const ResponseEconomy& other) const {
        return {omit_text_copy || other.omit_text_copy,
                reference_session || other.reference_session};
    }
};

// What a client's capabilities object declared. Anything that is not the
// documented shape declares nothing.
ResponseEconomy declaredResponseEconomy(const json& capabilities);

// How often a live answer carries the whole session descriptor, chosen by the
// operator at startup with --session-descriptor (#1031).
//
// No host Didi documents can declare an extension, so the declaration above
// reaches only a client written for it. The descriptor half is safe to apply
// for every client, because the reference is valid in both halves of a result
// and runtime_get_session returns the rest. The text copy is not: only the
// client knows which half it reads, so no switch turns that one on. `every` is
// the default because API_SPECIFICATION.md promises the endpoint on every
// successful result; `once` is the operator deciding their host does not need
// it.
enum class SessionDescriptorMode { Every, Once };
std::optional<SessionDescriptorMode> parseSessionDescriptorMode(const std::string& value);
const char* sessionDescriptorModeName(SessionDescriptorMode mode);

// This server's half of the negotiation, for initialize and server/discover.
json responseEconomyDeclaration();

// The last session descriptor one conversation was sent in full.
//
// A 2024-11-05 client is one conversation per process, so the process can know
// what it has already told it. The descriptor is compared whole rather than by
// session_id, so any change at all sends it again.
class SessionDescriptorLedger {
public:
    // True when this is the descriptor last sent in full. Otherwise it is
    // recorded as sent, because the caller is about to send it.
    bool alreadySent(const json& descriptor);

private:
    std::mutex m_mutex;
    std::optional<json> m_lastSent;
};

// Whether the client already holds this session descriptor. Answering false
// may record it as sent, since the answer is then going to carry it.
using DescriptorHeld = std::function<bool(const json& descriptor)>;

// Replaces a live payload's top-level session descriptor with a reference when
// the client already holds it, and says whether it did. A payload that is not
// a live answer, or whose session is not a descriptor, is left alone; asking
// may still record the descriptor as sent. Shared by tools/call and
// resources/read, so both use one ledger (#1033).
bool referenceHeldSession(json& payload, const DescriptorHeld& client_holds);

// Narrows a successful result to the sections a caller selected with `fields`
// (Q5). A declared section the answer carries and the caller did not select is
// removed, and named in omitted_fields in the order the sections are declared.
// Keys that are not sections are always kept, a text item that was the
// payload is rewritten to match, and a failure is returned as it came.
CallToolResult selectSections(CallToolResult result, const std::vector<std::string>& sections,
                              const json& selected);

// Applies what a client declared to one encoded tools/call result, the output
// of CallToolResult::toJson(). A result the client did not ask to change comes
// back untouched, and so does every failure: an error keeps its text and its
// session provenance whatever was declared, because it is the answer a caller
// most needs whole.
json economizeToolResult(json encoded, const ResponseEconomy& economy,
                         const DescriptorHeld& client_holds);

}  // namespace didi::mcp
