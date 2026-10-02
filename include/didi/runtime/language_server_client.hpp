#pragma once

// A client for the GDScript language server a Godot editor runs (Q11 in
// docs/BUILD_QUEUE.md).
//
// script_check_syntax compiled a file in a separate `godot --headless
// --check-only` process, which has no SceneTree and so no autoloads, and every
// script that named one came back with a false error until a heuristic demoted
// it (#383). The editor the caller is working in already knows the project's
// autoloads and class names, and serves its analyzer over the Language Server
// Protocol. This asks it.
//
// Consuming Godot's server is not building a language server, so the Phase 8
// exclusion stands. Measured on 4.5.1, 4.6.2 and 4.7.2 (2026-10-02):
//
// - Messages are JSON-RPC with Content-Length framing over TCP.
// - `initialize` with a rootUri that is not the editor's project draws
//   `gdscript_client/changeWorkspace` naming the project the editor has open.
//   That is how a server owned by another editor is told apart: two editors
//   contend for one port and the second one's server never starts.
// - The first `initialize` an editor sees parses every script in the project
//   on its main thread and prints an ERROR for each one that does not parse.
//   It happens once per editor.
// - `didOpen` answers with `textDocument/publishDiagnostics` for that file,
//   from the text sent rather than the file on disk. Severity 1 is an error
//   and 2 a warning; `code` differs by line and is not read.
// - There is no `shutdown`; the server answers -32601.
// - Godot never forgets the last client to speak, and an editor that later
//   wants that client (the Connect dialog adding a callback) prints an ERROR
//   when it has gone. So the connection is kept open between checks rather
//   than opened and dropped each time.

#include "didi/common/json.hpp"
#include "didi/offline/gdscript_diagnostics.hpp"

#include <chrono>
#include <cstddef>
#include <optional>
#include <string>
#include <vector>

namespace didi::runtime {

// Where an editor's language server listens, as the editor's bridge reports it.
struct LanguageServerEndpoint {
    std::string host;
    int port{0};
    // "editor_settings", or "command_line" when the editor was started with
    // --lsp-port, which overrides the setting without changing it.
    std::string port_source;
};

struct LanguageServerLimits {
    std::chrono::milliseconds connect{1000};
    // The first initialize an editor sees parses the whole project.
    std::chrono::milliseconds initialize{20000};
    std::chrono::milliseconds diagnostics{10000};
    std::size_t max_message_bytes{16u * 1024u * 1024u};
    std::size_t max_diagnostics{500};
};

struct LanguageServerCheck {
    // The diagnostics below came from the server. When false, `failure` says
    // why and nothing else is meaningful.
    bool answered{false};
    std::vector<offline::ScriptDiagnostic> diagnostics;
    // More diagnostics arrived than max_diagnostics.
    bool truncated{false};
    double seconds{0.0};
    std::string failure;
    // The project the server answered for, when it was not this one.
    std::string other_project;
};

// Checks one script with the language server at `endpoint`.
//
// `connection_key` names the editor session, so a connection kept for one
// editor is never reused for another. `project_root` is the editor's project
// directory and `res_path` the script's res:// path; `source_text` is what the
// server is asked to analyze, which callers read from disk.
LanguageServerCheck checkScriptWithLanguageServer(const LanguageServerEndpoint& endpoint,
                                                  const std::string& connection_key,
                                                  const std::string& project_root,
                                                  const std::string& res_path,
                                                  const std::string& source_text,
                                                  const LanguageServerLimits& limits = {});

// Drops the connection kept between checks, if there is one.
void closeLanguageServerConnection();

namespace language_server {

// One message with its Content-Length header.
std::string frame(const json& message);

// Splits a byte stream into the messages framed in it.
class FrameReader {
public:
    explicit FrameReader(std::size_t max_message_bytes) : m_max(max_message_bytes) {}
    void push(const char* data, std::size_t size) { m_buffer.append(data, size); }
    // The next whole message, or nothing until more bytes arrive. Once a
    // stream is malformed it stays failed, because nothing after a bad frame
    // can be located.
    std::optional<json> next();
    bool failed() const { return !m_error.empty(); }
    const std::string& error() const { return m_error; }

private:
    std::size_t m_max;
    std::string m_buffer;
    std::string m_error;
};

// file:///C:/a%20b/c.gd for C:/a b/c.gd, and file:///home/a/c.gd for /home/a/c.gd.
std::string fileUri(const std::string& absolute_path);

// A path or file URI in a form two spellings of the same file compare equal
// in: decoded, forward slashes, no trailing slash, and case-folded on Windows.
std::string comparablePath(const std::string& path_or_uri);

// One LSP Diagnostic as a script diagnostic, or nothing when it is not one.
std::optional<offline::ScriptDiagnostic> diagnosticFromLsp(const json& diagnostic);

// The loopback address to connect to for the host an editor listens on, or
// nothing when it is not this machine's loopback or wildcard address. Didi does
// not open a connection off this machine to read a project's scripts.
std::optional<std::string> loopbackAddressFor(const std::string& host);

} // namespace language_server
} // namespace didi::runtime
