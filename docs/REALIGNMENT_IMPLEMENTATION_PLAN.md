# Didi Realignment Program Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Bring Didi to the current MCP specification as a dual-era server, close the capability gaps that break the agent verification loop, and consolidate documentation and roadmap so the project has one honest source of truth.

**Architecture:** Didi stays a C++20 standalone binary plus GDExtension with authenticated local IPC — that architecture is validated, not challenged, by the current spec. The protocol layer gains a version-dispatching front end that serves both the legacy `initialize` handshake and the modern stateless per-request `_meta` model from the same process. Didi's proprietary metadata moves from an ad-hoc `_meta.didi` block to a declared, namespaced MCP extension that can be switched off.

**Tech Stack:** C++20, nlohmann-style `json` (`include/didi/common/json.hpp`), CMake, the in-repo test harness in `tests/test_main.cpp`, Godot 4.5+ GDExtension, PowerShell/CTest CI.

---

## Global Constraints

- Target spec revision: **`2026-07-28`** (current). Legacy revision retained: **`2024-11-05`** (what Didi ships today).
- Didi MUST remain a **dual-era server**: a request carrying modern per-request `_meta` is served statelessly; an `initialize` request selects legacy semantics. Per spec: *"A dual-era server MAY serve both eras concurrently on the same endpoint or process."*
- No existing client may break. Every change lands behind era dispatch, not as a replacement.
- Zero new external runtime dependencies. No Node.js, no Python at runtime.
- Extension identifiers MUST follow `_meta` key naming rules with a mandatory prefix. Didi's is **`io.saworbit.didi/capabilities`**.
- All extension behavior is **on by default** and disabled wholesale by `--strict-mcp`.
- Capability honesty rule from Phase 1 is unchanged and absolute: never report `success`, `implemented: true`, or `is_live_frame: true` for a path that did not execute.
- Every new tool name requires a Surface Amendment record (Workstream C, Task C1) before implementation.
- Platforms: Windows, Linux, macOS. Godot 4.5+.

---

## The finding that reframes this whole program

Didi is not out of compliance because of the decisions you made. Didi is compliant with `2024-11-05`, and the specification has since been **redesigned**, most drastically in `2026-07-28`:

| What changed | Where Didi stands |
| :--- | :--- |
| `initialize` / `notifications/initialized` handshake **removed**; MCP is now stateless | We implement only the removed handshake (`src/mcp/mcp_server.cpp:140`) |
| Every request carries `_meta.io.modelcontextprotocol/protocolVersion`, `clientInfo`, `clientCapabilities` | Not parsed |
| `server/discover` — servers **MUST** implement | Absent |
| All results carry `resultType: "complete"` | Absent |
| `ttlMs` + `cacheScope` required on all `*/list` and `resources/read` results | Absent |
| `ping`, `logging/setLevel`, `resources/subscribe` **removed** | We implement `ping`; Phase 11 planned the other two — that plan is now obsolete |
| Roots, Sampling, Logging **deprecated** | Not implemented — accidentally correct |
| Server-initiated requests replaced by Multi Round-Trip Requests (`InputRequiredResult`) | Not implemented |
| `outputSchema` + `structuredContent` on tools | Not implemented — every Didi tool returns JSON as a text blob |
| `extensions` field on capabilities for optional non-core features | This is the sanctioned home for `_meta.didi` |

**Three things follow from this, and they matter more than the list above.**

**1. Your "executable, not GDScript" decision was right, and the spec has now caught up to it.** The `2026-07-28` "Stateful Tools" section reads: *"MCP has no protocol-level session, so a server cannot rely on implicit per-connection state... servers that need to maintain state across calls should do so by returning an explicit handle from a creation tool and accepting that handle as an argument on subsequent calls."* That is precisely Didi's runtime session IDs and confirmation tokens. It also says handles should be opaque, bounded in lifetime, authorization-checked on every call, and return a clear error when expired — all four of which Didi already does. You built the pattern the spec later adopted. A stateless protocol in front of a stateful native backend is now the blessed architecture, and a pure-GDScript in-editor server would have made it harder, not easier.

**2. Almost nothing Didi does is genuinely "outside the boundary."** You asked to call out the hacks. Having read the spec against the code, the honest answer is that `_meta` is the *designated* extension point and `2026-07-28` added a formal `extensions` capability map for exactly this. Didi's capability metadata is not a hack — it is the extension mechanism used correctly, with the wrong key name. Rename `_meta.didi` to `io.saworbit.didi/capabilities`, declare it in `capabilities.extensions`, and it stops being an asterisk and becomes a feature. The genuinely non-MCP part of Didi is the Godot IPC channel, which is a backend implementation detail the spec has no opinion about, exactly like a database driver.

**3. The gaps in the competitive analysis and the gaps in spec compliance have the same root cause,** which is the one you already identified: the surface was declared before it was validated. Workstream C fixes the mechanism, not just the symptoms.

---

## Two decisions to confirm before execution

**Decision 1 — Dual-era, not a jump.** Recommended: implement `2026-07-28` alongside `2024-11-05` and advertise both from `server/discover`. Rationale: the spec explicitly sanctions dual-era servers, and the "Legacy client / Modern server" row of the compatibility matrix is `Fails` — a clean jump would break every client on the market today until they migrate. Confirm before Task A1.

**Decision 2 — The 78-name freeze gets a controlled amendment process, not an exception.** Recommended: keep the rule that names cannot be added casually, but replace "never" with a Surface Amendment record requiring a workflow justification, an execution mode, and a test before a name is registered. Six of the eight competitive gaps have no canonical name today, and under the current rule they can never acquire one. Confirm before Task C1.

---

## Foundational rules audit

Before executing, all 21 standing rules were audited against the current
specification and the code that enforces them. Nine were kept, eight amended,
three retired, one flagged for recheck. The discipline is overwhelmingly sound;
one rule had inverted.

**Only one rule was ever load-bearing for quality: "never claim success on a
stub."** Everything else is scaffolding that grew around it. The hard-coded tool
counts in `tools/validate_documentation.py` were scaffolding that turned hostile
— built to protect the no-stub rule, they came to enforce prose agreement
*instead of* truth, which is that rule's exact opposite. Workstream 0 fixes it.

| Rule | Verdict | Note |
| :--- | :--- | :--- |
| Counts 78/10/88/60/18 as doc regexes | **RETIRED** | Workstream 0. Blocked Phase 7 |
| `FUTURE_PHASE_RANGE = range(7,13)` | AMEND | Freezes roadmap shape in CI |
| 17 fixed `REQUIRED_DOCUMENTS` paths | AMEND | Blocks doc consolidation (D1) |
| Version consistency across 8 files | KEEP | Checks code, not just prose |
| Forbidden agent-artifact paths | KEEP | Working as intended |
| Link and anchor validation | KEEP | Rare and valuable |
| **"No success stubs"** | **KEEP** | The crown jewel. Absolute |
| "Don't increase the canonical count" | **RETIRED** | Workstream 0. Split from the rule above |
| "No second plugin architecture or network transport" | AMEND | State the reason: local-only is a security boundary |
| "No custom GDScript language server" | KEEP + clarify | Consuming Godot's LSP as a *client* is not this |
| "Replace static ClassDB map with `extension_api.json`" | KEEP | Already pre-authorizes B5 |
| Phase gates: docs + native + Godot + CI | KEEP | Why Didi is trustworthy |
| Phases organized by engine subsystem | AMEND | Reorganize by agent workflow |
| MCP `2024-11-05` | RETIRE | → dual-era, Workstream A |
| `eval_gdscript` expression-only | KEEP | Now also the predicate language for B2 |
| dry-run + confirmation tokens | KEEP + extend | Spec blesses the pattern; pair with elicitation |
| One-client `423` lock on all operations | AMEND | Reads could be concurrent, mutations exclusive |
| Explicit `--project`, fail closed | KEEP | Genuinely differentiating |
| Zero external runtime dependencies | KEEP | The moat |
| 10 legacy names retained indefinitely | AMEND | Adopt MCP's 12-month deprecation lifecycle |
| `scene_close` requires `discard_unsaved` | **RECHECK** | See below |

**`scene_close` recheck.** The docs pin the constraint to *"Godot 4.5 does not
expose active-scene dirty state through GDExtension"*, but CI now tests 4.5.1,
4.6.2, and 4.7.2 and the refusal is unconditional in code. Either the limitation
still holds in 4.7 and the docs are misleadingly version-pinned, or it was fixed
upstream and the friction is now unnecessary. This needs a Godot integration test
that asserts the limitation per version, not a sentence that asserts it.

---

## Workstream sequencing

Each workstream produces working, testable software on its own. **A and C must be planned in full detail before execution; B, D, and E get their own plans when their turn comes** — this document scopes them and sets their exit gates.

```
A. Protocol modernization ──┐
                            ├──> C. Surface governance ──> B. Capability gaps ──> D. Docs ──> E. Distribution
                            │         (fixes the mechanism)   (fixes the symptoms)
   (fully detailed below) ──┘
```

- **A** first because every other workstream writes tool definitions, and `outputSchema` / `annotations` / `resultType` change the shape of every one of them. Doing A last means rewriting everything twice.
- **C** before **B** because B adds tool names and C is what makes adding names legitimate.
- **D** continuously, gated last, because docs describe what exists.

---

# Workstream 0: Governance Unlock — `COMPLETE`

**Delivered 2026-08-30.** Every other workstream writes tool definitions or
consolidates documents, and both were blocked by counts hard-coded in CI. This
had to land first.

- `kLegacyToolNames` in `include/didi/mcp/mcp_protocol.hpp` is the single
  declaration of which registrations are legacy. Before this, the
  canonical/legacy split existed only in prose and was unverifiable.
- `ToolRegistry::buildManifest()` and `didi --dump-tool-manifest` emit sorted,
  byte-stable JSON with counts and names.
- `tools/validate_documentation.py --tool-manifest` derives every published count
  from that artifact. A missing, malformed, or internally inconsistent manifest
  is a hard failure with an actionable message.
- CI generates the manifest from the binary it just built and validates against it.
- The fused rule is split: no success stubs stays absolute; the surface grows
  through [Surface Amendments](SURFACE_AMENDMENTS.md).

**Result:** the binary independently reports 78 canonical, 10 legacy, 88 total,
60 implemented, 18 unimplemented — matching what the documents already claimed.
The numbers were right; nothing could prove it. Implementing a reserved tool now
updates every published count automatically instead of breaking CI.

**Verified:** `didi_tests` 178 passed / 0 failed; `pytest
tests/test_documentation_validator.py` 38 passed; doc validation exit 0; and
negative cases — count drift, inconsistent manifest, missing manifest — all fail
with the offending document and expected value named.

**Known issue found while verifying:** `Tools.CaptureViewportWithIpc` is flaky.
It failed once with `runtime_logs.isOk()` on the first run after a fresh build,
then passed on five consecutive runs. Tracked separately; not addressed here.

**Still open from the audit:** `FUTURE_PHASE_RANGE` and `REQUIRED_DOCUMENTS`
remain coupled to the current roadmap and doc layout. Both block Workstream D and
should be decoupled as part of it, not before.

---

# Workstream A: Protocol Modernization

**Deliverable:** Didi answers `server/discover`, serves modern stateless requests and legacy handshake requests from one process, emits `resultType`, `ttlMs`/`cacheScope`, `outputSchema`, `structuredContent`, and tool `annotations`, and declares its capability metadata as a namespaced extension with a `--strict-mcp` off switch.

## File Structure

| File | Responsibility |
| :--- | :--- |
| `include/didi/mcp/mcp_protocol.hpp` (modify) | Version constants, supported-version list, extension identifier, error codes |
| `include/didi/mcp/protocol_era.hpp` (create) | `ProtocolEra` enum, `RequestContext` struct, `_meta` parsing declarations |
| `src/mcp/protocol_era.cpp` (create) | `_meta` parsing, era detection, version validation |
| `include/didi/mcp/mcp_server.hpp` (modify) | Era-aware `handleRequest` signature, `--strict-mcp` flag storage |
| `src/mcp/mcp_server.cpp` (modify) | Era dispatch, `server/discover`, result decoration |
| `include/didi/mcp/tool_registry.hpp` (modify) | `outputSchema`, `annotations`, `title` fields on tool definitions |
| `src/mcp/tool_registry.cpp` (modify) | Emit new fields; deterministic ordering |
| `src/standalone/main.cpp` (modify) | `--strict-mcp` argument parsing |
| `tests/test_protocol_era.cpp` (create) | Era detection, version negotiation, `_meta` parsing |
| `tests/test_discover.cpp` (create) | `server/discover` contract |
| `tests/test_strict_mcp.cpp` (create) | Extension on/off behavior |
| `CMakeLists.txt` (modify) | Register the three new test files |

---

### Task A1: Protocol version constants and era detection

**Files:**
- Modify: `include/didi/mcp/mcp_protocol.hpp:12`
- Create: `include/didi/mcp/protocol_era.hpp`
- Create: `src/mcp/protocol_era.cpp`
- Create: `tests/test_protocol_era.cpp`
- Modify: `CMakeLists.txt:118` (add test file after `tests/test_phase6.cpp`)

**Interfaces:**
- Produces: `didi::mcp::ProtocolEra` (`Legacy`, `Modern`, `Unsupported`), `didi::mcp::RequestContext{ era, protocol_version, client_name, client_capabilities }`, `didi::mcp::detectEra(const json& request) -> RequestContext`, `didi::mcp::kSupportedVersions` (ordered, newest first), `didi::mcp::kModernVersion`, `didi::mcp::kLegacyVersion`, `didi::mcp::kExtensionId`.

- [ ] **Step 1: Write the failing test**

Create `tests/test_protocol_era.cpp`:

```cpp
#include "didi/mcp/protocol_era.hpp"
#include "didi/common/json.hpp"
#include <stdexcept>
#include <string>

#define ASSERT_TRUE(cond) if (!(cond)) throw std::runtime_error("Assertion failed: " #cond);
#define ASSERT_EQ(a, b) ASSERT_TRUE((a) == (b))

void registerTest(const std::string& name, std::function<void()> fn);

static void test_era_modern_request_detected() {
    auto req = didi::json::parse(R"({
        "jsonrpc":"2.0","id":1,"method":"tools/list",
        "params":{"_meta":{
            "io.modelcontextprotocol/protocolVersion":"2026-07-28",
            "io.modelcontextprotocol/clientInfo":{"name":"TestClient","version":"1.0"}
        }}
    })");
    auto ctx = didi::mcp::detectEra(req);
    ASSERT_EQ(ctx.era, didi::mcp::ProtocolEra::Modern);
    ASSERT_EQ(ctx.protocol_version, std::string("2026-07-28"));
    ASSERT_EQ(ctx.client_name, std::string("TestClient"));
}

static void test_era_initialize_is_legacy() {
    auto req = didi::json::parse(R"({"jsonrpc":"2.0","id":1,"method":"initialize","params":{}})");
    auto ctx = didi::mcp::detectEra(req);
    ASSERT_EQ(ctx.era, didi::mcp::ProtocolEra::Legacy);
}

static void test_era_no_meta_no_initialize_is_legacy() {
    // A bare tools/list with no _meta is a legacy client mid-session.
    auto req = didi::json::parse(R"({"jsonrpc":"2.0","id":2,"method":"tools/list","params":{}})");
    auto ctx = didi::mcp::detectEra(req);
    ASSERT_EQ(ctx.era, didi::mcp::ProtocolEra::Legacy);
}

static void test_era_unknown_version_is_unsupported() {
    auto req = didi::json::parse(R"({
        "jsonrpc":"2.0","id":1,"method":"tools/list",
        "params":{"_meta":{"io.modelcontextprotocol/protocolVersion":"1900-01-01"}}
    })");
    auto ctx = didi::mcp::detectEra(req);
    ASSERT_EQ(ctx.era, didi::mcp::ProtocolEra::Unsupported);
    ASSERT_EQ(ctx.protocol_version, std::string("1900-01-01"));
}

static void test_supported_versions_newest_first() {
    ASSERT_TRUE(didi::mcp::kSupportedVersions.size() >= 2);
    ASSERT_EQ(std::string(didi::mcp::kSupportedVersions[0]), std::string("2026-07-28"));
}

struct RegisterProtocolEraTests {
    RegisterProtocolEraTests() {
        registerTest("protocol_era.modern_detected", test_era_modern_request_detected);
        registerTest("protocol_era.initialize_is_legacy", test_era_initialize_is_legacy);
        registerTest("protocol_era.bare_request_is_legacy", test_era_no_meta_no_initialize_is_legacy);
        registerTest("protocol_era.unknown_version_unsupported", test_era_unknown_version_is_unsupported);
        registerTest("protocol_era.versions_newest_first", test_supported_versions_newest_first);
    }
} g_register_protocol_era_tests;
```

- [ ] **Step 2: Run the test to verify it fails**

```bash
cmake --build build --target didi_tests && ./build/didi_tests
```

Expected: compile failure — `didi/mcp/protocol_era.hpp: No such file or directory`.

- [ ] **Step 3: Write the header**

Create `include/didi/mcp/protocol_era.hpp`:

```cpp
#pragma once
#include "didi/common/json.hpp"
#include <array>
#include <string>

namespace didi::mcp {

inline constexpr const char* kModernVersion = "2026-07-28";
inline constexpr const char* kLegacyVersion = "2024-11-05";
inline constexpr const char* kExtensionId   = "io.saworbit.didi/capabilities";

// Newest first. Clients pick from this list on UnsupportedProtocolVersionError.
inline constexpr std::array<const char*, 2> kSupportedVersions{
    kModernVersion, kLegacyVersion
};

// Spec 2026-07-28 error-code policy: -32020..-32099 reserved for the spec.
inline constexpr int kUnsupportedProtocolVersion = -32022;

enum class ProtocolEra { Legacy, Modern, Unsupported };

struct RequestContext {
    ProtocolEra era = ProtocolEra::Legacy;
    std::string protocol_version;
    std::string client_name;
    json client_capabilities = json::object();
};

// Detects era from the request body alone. A request carrying
// _meta["io.modelcontextprotocol/protocolVersion"] is modern; anything else
// (including `initialize` and bare in-session calls) is legacy.
RequestContext detectEra(const json& request);

bool isSupportedVersion(const std::string& version);

} // namespace didi::mcp
```

- [ ] **Step 4: Write the implementation**

Create `src/mcp/protocol_era.cpp`:

```cpp
#include "didi/mcp/protocol_era.hpp"
#include <algorithm>

namespace didi::mcp {

bool isSupportedVersion(const std::string& version) {
    return std::any_of(kSupportedVersions.begin(), kSupportedVersions.end(),
                       [&](const char* v) { return version == v; });
}

RequestContext detectEra(const json& request) {
    RequestContext ctx;

    if (!request.contains("params") || !request["params"].is_object()) {
        return ctx; // Legacy by default.
    }
    const auto& params = request["params"];
    if (!params.contains("_meta") || !params["_meta"].is_object()) {
        return ctx;
    }
    const auto& meta = params["_meta"];

    const char* kVersionKey = "io.modelcontextprotocol/protocolVersion";
    if (!meta.contains(kVersionKey) || !meta[kVersionKey].is_string()) {
        return ctx;
    }

    ctx.protocol_version = meta[kVersionKey].get<std::string>();
    ctx.era = isSupportedVersion(ctx.protocol_version)
                  ? ProtocolEra::Modern
                  : ProtocolEra::Unsupported;

    const char* kInfoKey = "io.modelcontextprotocol/clientInfo";
    if (meta.contains(kInfoKey) && meta[kInfoKey].is_object()
        && meta[kInfoKey].contains("name") && meta[kInfoKey]["name"].is_string()) {
        ctx.client_name = meta[kInfoKey]["name"].get<std::string>();
    }

    const char* kCapsKey = "io.modelcontextprotocol/clientCapabilities";
    if (meta.contains(kCapsKey) && meta[kCapsKey].is_object()) {
        ctx.client_capabilities = meta[kCapsKey];
    }

    return ctx;
}

} // namespace didi::mcp
```

Note: `kLegacyVersion` is a supported version string but arrives via `initialize`, not `_meta`. A modern client naming `2024-11-05` in `_meta` is served modern-shaped results at that version — this is intentional and matches the spec's per-request model.

- [ ] **Step 5: Register the new source and test in CMake**

In `CMakeLists.txt`, add `src/mcp/protocol_era.cpp` to the core library sources and `tests/test_protocol_era.cpp` to the `didi_tests` source list after `tests/test_phase6.cpp`.

- [ ] **Step 6: Run the test to verify it passes**

```bash
cmake --build build --target didi_tests && ./build/didi_tests
```

Expected: all five `protocol_era.*` tests PASS, and every pre-existing test still passes.

- [ ] **Step 7: Commit**

```bash
git add include/didi/mcp/protocol_era.hpp src/mcp/protocol_era.cpp tests/test_protocol_era.cpp CMakeLists.txt
git commit -m "feat(mcp): add protocol era detection for dual-era support"
```

---

### Task A2: `server/discover` and `UnsupportedProtocolVersionError`

**Files:**
- Modify: `src/mcp/mcp_server.cpp:139` (insert before the `initialize` branch)
- Create: `tests/test_discover.cpp`
- Modify: `CMakeLists.txt`

**Interfaces:**
- Consumes: `detectEra`, `kSupportedVersions`, `kUnsupportedProtocolVersion`, `kExtensionId` from Task A1.
- Produces: `server/discover` responding with `DiscoverResult`; `McpServer::makeUnsupportedVersionError(const json& id, const std::string& requested) -> JsonRpcResponse`.

- [ ] **Step 1: Write the failing test**

Create `tests/test_discover.cpp`:

```cpp
#include "didi/mcp/mcp_server.hpp"
#include "didi/mcp/jsonrpc.hpp"
#include "didi/mcp/protocol_era.hpp"
#include <stdexcept>
#include <string>

#define ASSERT_TRUE(cond) if (!(cond)) throw std::runtime_error("Assertion failed: " #cond);
#define ASSERT_EQ(a, b) ASSERT_TRUE((a) == (b))

void registerTest(const std::string& name, std::function<void()> fn);

static didi::json callServer(didi::mcp::McpServer& server, const std::string& raw) {
    auto req = didi::mcp::JsonRpcRequest::parse(raw);
    ASSERT_TRUE(req.has_value());
    return server.handleRequest(*req).toJson();
}

static void test_discover_returns_supported_versions() {
    didi::mcp::McpServer server;
    auto resp = callServer(server, R"({
        "jsonrpc":"2.0","id":"d1","method":"server/discover",
        "params":{"_meta":{"io.modelcontextprotocol/protocolVersion":"2026-07-28"}}
    })");
    ASSERT_TRUE(resp.contains("result"));
    const auto& r = resp["result"];
    ASSERT_EQ(r["resultType"].get<std::string>(), std::string("complete"));
    ASSERT_TRUE(r["supportedVersions"].is_array());
    ASSERT_EQ(r["supportedVersions"][0].get<std::string>(), std::string("2026-07-28"));
    ASSERT_TRUE(r.contains("capabilities"));
    ASSERT_TRUE(r["_meta"].contains("io.modelcontextprotocol/serverInfo"));
    ASSERT_TRUE(r.contains("instructions"));
}

static void test_discover_works_without_prior_initialize() {
    // server/discover MUST answer before any handshake.
    didi::mcp::McpServer server;
    auto resp = callServer(server, R"({"jsonrpc":"2.0","id":1,"method":"server/discover","params":{}})");
    ASSERT_TRUE(resp.contains("result"));
    ASSERT_TRUE(!resp.contains("error"));
}

static void test_discover_declares_didi_extension() {
    didi::mcp::McpServer server;
    auto resp = callServer(server, R"({"jsonrpc":"2.0","id":1,"method":"server/discover","params":{}})");
    const auto& exts = resp["result"]["capabilities"]["extensions"];
    ASSERT_TRUE(exts.contains("io.saworbit.didi/capabilities"));
}

static void test_unsupported_version_returns_32022_with_supported_list() {
    didi::mcp::McpServer server;
    auto resp = callServer(server, R"({
        "jsonrpc":"2.0","id":9,"method":"tools/list",
        "params":{"_meta":{"io.modelcontextprotocol/protocolVersion":"1900-01-01"}}
    })");
    ASSERT_TRUE(resp.contains("error"));
    ASSERT_EQ(resp["error"]["code"].get<int>(), -32022);
    ASSERT_EQ(resp["error"]["data"]["requested"].get<std::string>(), std::string("1900-01-01"));
    ASSERT_TRUE(resp["error"]["data"]["supported"].is_array());
}

struct RegisterDiscoverTests {
    RegisterDiscoverTests() {
        registerTest("discover.supported_versions", test_discover_returns_supported_versions);
        registerTest("discover.no_handshake_required", test_discover_works_without_prior_initialize);
        registerTest("discover.declares_extension", test_discover_declares_didi_extension);
        registerTest("discover.unsupported_version_error", test_unsupported_version_returns_32022_with_supported_list);
    }
} g_register_discover_tests;
```

- [ ] **Step 2: Run to verify it fails**

```bash
cmake --build build --target didi_tests && ./build/didi_tests
```

Expected: `discover.*` tests FAIL — `server/discover` returns method-not-found.

- [ ] **Step 3: Implement discover and the version guard**

In `src/mcp/mcp_server.cpp`, at the top of `handleRequest` before the `initialize` branch:

```cpp
const RequestContext ctx = detectEra(nlohmann_request_body(req));

if (ctx.era == ProtocolEra::Unsupported) {
    return makeUnsupportedVersionError(req.id, ctx.protocol_version);
}

if (req.method == "server/discover") {
    json capabilities = {
        {"tools",     {{"listChanged", true}}},
        {"resources", {{"listChanged", true}}},
        {"prompts",   {{"listChanged", false}}}
    };
    if (!m_strictMcp) {
        capabilities["extensions"] = { {kExtensionId, json::object()} };
    }

    json supported = json::array();
    for (const char* v : kSupportedVersions) supported.push_back(v);

    json result = {
        {"resultType", "complete"},
        {"supportedVersions", supported},
        {"capabilities", capabilities},
        {"instructions",
         "Didi is a native MCP server for Godot 4.5+. Always read the capability "
         "metadata on each tool before calling it: a tool may be registered but "
         "unimplemented, or implemented but unavailable because no Godot editor is "
         "attached. Never infer availability from a tool name."},
        {"ttlMs", 3600000},
        {"cacheScope", "private"},
        {"_meta", {{"io.modelcontextprotocol/serverInfo",
                    {{"name", kServerName}, {"version", kServerVersion}}}}}
    };
    return JsonRpcResponse::makeSuccess(req.id, result);
}
```

Add to `McpServer`:

```cpp
JsonRpcResponse McpServer::makeUnsupportedVersionError(const json& id,
                                                      const std::string& requested) {
    json supported = json::array();
    for (const char* v : kSupportedVersions) supported.push_back(v);
    json data = {{"supported", supported}, {"requested", requested}};
    return JsonRpcResponse::makeErrorWithData(
        id, static_cast<JsonRpcErrorCode>(kUnsupportedProtocolVersion),
        "Unsupported protocol version", data);
}
```

If `JsonRpcResponse` has no `makeErrorWithData`, add it in `include/didi/mcp/jsonrpc.hpp` mirroring `makeError` with an extra `data` member written into the `error` object.

Note `listChanged` flips to `true` for tools and resources. That is the honesty fix identified in the competitive review — Didi's `currentMode` / `liveAvailable` genuinely change when the editor attaches. Emitting the notification is Task A5.

- [ ] **Step 4: Run to verify it passes**

```bash
cmake --build build --target didi_tests && ./build/didi_tests
```

Expected: all four `discover.*` tests PASS.

- [ ] **Step 5: Commit**

```bash
git add src/mcp/mcp_server.cpp include/didi/mcp/mcp_server.hpp include/didi/mcp/jsonrpc.hpp tests/test_discover.cpp CMakeLists.txt
git commit -m "feat(mcp): implement server/discover and UnsupportedProtocolVersionError"
```

---

### Task A3: Modern result decoration — `resultType`, `ttlMs`, `cacheScope`

**Files:**
- Modify: `src/mcp/mcp_server.cpp` (`tools/list`, `resources/list`, `resources/read`, `prompts/list`, `tools/call`, `prompts/get` handlers)
- Modify: `tests/test_discover.cpp` (add cases)

**Interfaces:**
- Consumes: `RequestContext` from A1.
- Produces: `McpServer::decorateResult(json& result, const RequestContext& ctx, bool cacheable, int ttl_ms)`.

- [ ] **Step 1: Write the failing test**

Append to `tests/test_discover.cpp`:

```cpp
static void test_modern_list_carries_resulttype_and_cache_fields() {
    didi::mcp::McpServer server;
    auto resp = callServer(server, R"({
        "jsonrpc":"2.0","id":1,"method":"tools/list",
        "params":{"_meta":{"io.modelcontextprotocol/protocolVersion":"2026-07-28"}}
    })");
    const auto& r = resp["result"];
    ASSERT_EQ(r["resultType"].get<std::string>(), std::string("complete"));
    ASSERT_TRUE(r.contains("ttlMs"));
    ASSERT_EQ(r["cacheScope"].get<std::string>(), std::string("private"));
}

static void test_legacy_list_omits_modern_fields() {
    didi::mcp::McpServer server;
    callServer(server, R"({"jsonrpc":"2.0","id":0,"method":"initialize","params":{}})");
    auto resp = callServer(server, R"({"jsonrpc":"2.0","id":1,"method":"tools/list","params":{}})");
    const auto& r = resp["result"];
    ASSERT_TRUE(!r.contains("resultType"));
    ASSERT_TRUE(!r.contains("ttlMs"));
}
```

Register both in `RegisterDiscoverTests`.

- [ ] **Step 2: Run to verify it fails**

Expected: `resultType` absent on the modern list result.

- [ ] **Step 3: Implement the decorator**

```cpp
void McpServer::decorateResult(json& result, const RequestContext& ctx,
                               bool cacheable, int ttl_ms) {
    if (ctx.era != ProtocolEra::Modern) return;  // Legacy shape is unchanged.
    result["resultType"] = "complete";
    if (cacheable) {
        result["ttlMs"] = ttl_ms;
        // Didi results are project- and session-scoped. Never "public".
        result["cacheScope"] = "private";
    }
    result["_meta"]["io.modelcontextprotocol/serverInfo"] =
        {{"name", kServerName}, {"version", kServerVersion}};
}
```

Call it before every `makeSuccess` return: `cacheable = true, ttl_ms = 5000` for `tools/list` and `resources/list` (short, because capability metadata flips on editor attach); `cacheable = true, ttl_ms = 0` for `resources/read`; `cacheable = false` for `tools/call` and `prompts/get`.

`cacheScope` is `"private"` everywhere with no exception — Didi results describe one user's local project.

- [ ] **Step 4: Run to verify it passes**

```bash
cmake --build build --target didi_tests && ./build/didi_tests
```

- [ ] **Step 5: Commit**

```bash
git add src/mcp/mcp_server.cpp include/didi/mcp/mcp_server.hpp tests/test_discover.cpp
git commit -m "feat(mcp): decorate modern results with resultType and cache hints"
```

---

### Task A4: Namespaced extension metadata and `--strict-mcp`

**Files:**
- Modify: `src/mcp/tool_registry.cpp:74` region (the `_meta.didi` writer)
- Modify: `src/standalone/main.cpp`
- Create: `tests/test_strict_mcp.cpp`
- Modify: `CMakeLists.txt`

**Interfaces:**
- Consumes: `kExtensionId` from A1.
- Produces: `McpServer::setStrictMcp(bool)`; tool definitions carrying `_meta["io.saworbit.didi/capabilities"]` instead of `_meta["didi"]`.

- [ ] **Step 1: Write the failing test**

Create `tests/test_strict_mcp.cpp`:

```cpp
#include "didi/mcp/mcp_server.hpp"
#include "didi/mcp/jsonrpc.hpp"
#include <stdexcept>
#include <string>

#define ASSERT_TRUE(cond) if (!(cond)) throw std::runtime_error("Assertion failed: " #cond);
#define ASSERT_EQ(a, b) ASSERT_TRUE((a) == (b))

void registerTest(const std::string& name, std::function<void()> fn);

static didi::json listTools(didi::mcp::McpServer& server) {
    auto req = didi::mcp::JsonRpcRequest::parse(
        R"({"jsonrpc":"2.0","id":1,"method":"tools/list",
            "params":{"_meta":{"io.modelcontextprotocol/protocolVersion":"2026-07-28"}}})");
    return server.handleRequest(*req).toJson()["result"]["tools"];
}

static void test_extension_metadata_present_by_default() {
    didi::mcp::McpServer server;
    auto tools = listTools(server);
    ASSERT_TRUE(tools.size() > 0);
    ASSERT_TRUE(tools[0]["_meta"].contains("io.saworbit.didi/capabilities"));
    ASSERT_TRUE(tools[0]["_meta"]["io.saworbit.didi/capabilities"].contains("currentMode"));
}

static void test_legacy_didi_key_no_longer_emitted() {
    didi::mcp::McpServer server;
    auto tools = listTools(server);
    ASSERT_TRUE(!tools[0]["_meta"].contains("didi"));
}

static void test_strict_mcp_strips_extension_metadata() {
    didi::mcp::McpServer server;
    server.setStrictMcp(true);
    auto tools = listTools(server);
    ASSERT_TRUE(tools.size() > 0);
    for (const auto& t : tools) {
        ASSERT_TRUE(!t.contains("_meta") || !t["_meta"].contains("io.saworbit.didi/capabilities"));
    }
}

static void test_strict_mcp_hides_unimplemented_tools() {
    // Without the extension a client cannot see `implemented:false`,
    // so strict mode must not advertise names it will refuse.
    didi::mcp::McpServer server;
    server.setStrictMcp(true);
    auto tools = listTools(server);
    for (const auto& t : tools) {
        ASSERT_TRUE(t["name"].get<std::string>() != std::string("signal_connect"));
    }
}

static void test_tools_list_is_deterministically_ordered() {
    didi::mcp::McpServer a, b;
    ASSERT_EQ(listTools(a).dump(), listTools(b).dump());
}

struct RegisterStrictMcpTests {
    RegisterStrictMcpTests() {
        registerTest("strict.extension_present_by_default", test_extension_metadata_present_by_default);
        registerTest("strict.legacy_key_removed", test_legacy_didi_key_no_longer_emitted);
        registerTest("strict.strips_extension", test_strict_mcp_strips_extension_metadata);
        registerTest("strict.hides_unimplemented", test_strict_mcp_hides_unimplemented_tools);
        registerTest("strict.deterministic_order", test_tools_list_is_deterministically_ordered);
    }
} g_register_strict_mcp_tests;
```

- [ ] **Step 2: Run to verify it fails**

Expected: failures on the extension key name and on `setStrictMcp` not existing.

- [ ] **Step 3: Implement**

In `src/mcp/tool_registry.cpp`, change the metadata writer from `definition["_meta"]["didi"][...]` to `definition["_meta"][kExtensionId][...]` for all six fields (`executionModes`, `implemented`, `currentMode`, `liveAvailable`, `editorConnected`, `sessionKind`).

In `McpServer`, add `bool m_strictMcp = false;` and `void setStrictMcp(bool v) { m_strictMcp = v; }`. In the `tools/list` handler, when `m_strictMcp` is true: erase `_meta[kExtensionId]` from each definition, skip any tool whose `implemented` is false, and skip the 10 legacy names. Sort the emitted array by `name` before returning, unconditionally — the spec asks for deterministic order in both eras.

In `src/standalone/main.cpp`, parse `--strict-mcp` and call `server.setStrictMcp(true)`.

**The rule this encodes:** the extension is what makes the surface honest. If a client turns the extension off, Didi must not advertise anything it cannot deliver, because there is no longer a channel to say so. Strict mode is therefore *smaller*, never merely quieter.

- [ ] **Step 4: Run to verify it passes**

```bash
cmake --build build --target didi_tests && ./build/didi_tests
```

- [ ] **Step 5: Commit**

```bash
git add src/mcp/tool_registry.cpp src/mcp/mcp_server.cpp include/didi/mcp/mcp_server.hpp src/standalone/main.cpp tests/test_strict_mcp.cpp CMakeLists.txt
git commit -m "feat(mcp): namespace capability metadata as an extension, add --strict-mcp"
```

---

### Task A5: Tool annotations, `outputSchema`, and `structuredContent`

**Files:**
- Modify: `include/didi/mcp/tool_registry.hpp`, `src/mcp/tool_registry.cpp`
- Modify: `src/mcp/mcp_server.cpp` (`tools/call` result assembly)
- Modify: `tests/test_tools.cpp`

**Interfaces:**
- Produces: `ToolDefinition::title`, `ToolDefinition::output_schema`, `ToolDefinition::annotations` with fields `readOnlyHint`, `destructiveHint`, `idempotentHint`, `openWorldHint`.

- [ ] **Step 1: Write the failing test**

Append to `tests/test_tools.cpp`:

```cpp
static void test_read_tools_annotated_readonly() {
    didi::mcp::McpServer server;
    auto tools = listToolsByName(server); // existing helper in this file
    const auto& t = tools.at("scene_get_hierarchy");
    ASSERT_EQ(t["annotations"]["readOnlyHint"].get<bool>(), true);
    ASSERT_EQ(t["annotations"]["openWorldHint"].get<bool>(), false);
}

static void test_destructive_tools_annotated_destructive() {
    didi::mcp::McpServer server;
    auto tools = listToolsByName(server);
    const auto& t = tools.at("scene_remove_node");
    ASSERT_EQ(t["annotations"]["readOnlyHint"].get<bool>(), false);
    ASSERT_EQ(t["annotations"]["destructiveHint"].get<bool>(), true);
}

static void test_tool_call_returns_structured_content() {
    didi::mcp::McpServer server;
    auto resp = callTool(server, "runtime_list_sessions", didi::json::object()); // existing helper
    const auto& r = resp["result"];
    ASSERT_TRUE(r.contains("structuredContent"));
    // Backwards compat: the same JSON must also appear as a text block.
    ASSERT_EQ(r["content"][0]["type"].get<std::string>(), std::string("text"));
    ASSERT_EQ(didi::json::parse(r["content"][0]["text"].get<std::string>()), r["structuredContent"]);
}
```

- [ ] **Step 2: Run to verify it fails**

Expected: `annotations` and `structuredContent` absent.

- [ ] **Step 3: Implement**

Add the three fields to `ToolDefinition` and emit them from `tools/list` (omit `annotations` entirely when unset rather than emitting an empty object).

Annotate every registered tool by this rule, which is mechanical and admits no judgment calls:

| Condition | `readOnlyHint` | `destructiveHint` | `idempotentHint` |
| :--- | :--- | :--- | :--- |
| Tool has no `dry_run` parameter | `true` | `false` | `true` |
| Mutation that only creates or sets | `false` | `false` | `true` |
| Mutation that removes, overwrites, or reloads | `false` | `true` | `false` |

`openWorldHint` is `false` for every Didi tool without exception — Didi's world is one local project and never reaches the network.

For `tools/call`, populate `structuredContent` with the JSON object handlers already build, and keep serialising the identical object into a `text` content block. Do not add `outputSchema` in this task; that is Task A6, which is per-tool work and should not block annotations.

**Why this matters beyond compliance:** `readOnlyHint` is the spec-native way to tell a client which tools are safe to auto-approve. satelliteoflove hand-rolls this by splitting every domain into `_read` and `_edit` tool pairs. Annotations get Didi the same benefit across all 60 implemented tools without splitting a single one.

- [ ] **Step 4: Run to verify it passes**

```bash
cmake --build build --target didi_tests && ./build/didi_tests
```

- [ ] **Step 5: Commit**

```bash
git add include/didi/mcp/tool_registry.hpp src/mcp/tool_registry.cpp src/mcp/mcp_server.cpp tests/test_tools.cpp
git commit -m "feat(mcp): add tool annotations and structuredContent results"
```

---

### Task A6: `outputSchema` for the twelve highest-traffic tools

**Files:** `src/mcp/tool_registry.cpp`, `tests/test_tools.cpp`

Add `outputSchema` (JSON Schema 2020-12) to: `scene_get_hierarchy`, `scene_get_property`, `viewport_capture_frame`, `viewport_diff_capture`, `script_check_syntax`, `project_search_text`, `project_search_symbols`, `project_list_resources`, `runtime_list_sessions`, `runtime_get_session`, `runtime_read_logs`, `runtime_get_tree`.

- [ ] **Step 1:** Write one test per tool asserting `outputSchema` exists, is an object, and that a real call's `structuredContent` validates against it.
- [ ] **Step 2:** Run — expect FAIL.
- [ ] **Step 3:** Add each schema, deriving fields from the handler's actual output in `src/tools/`. Every schema MUST include `execution_mode` as a required string, since every Didi result carries it.
- [ ] **Step 4:** Run — expect PASS.
- [ ] **Step 5:** Commit as `feat(mcp): add outputSchema to core read tools`.

---

### Task A7: `notifications/tools/list_changed` on editor attach and detach

**Files:** `src/mcp/mcp_server.cpp`, `src/runtime/session_client.cpp`, `tests/test_runtime_routing.cpp`

This closes the honesty gap the competitive review found: `liveAvailable` changes when Godot attaches, and today no client is told.

- [ ] **Step 1:** Write a test that attaches a fake session and asserts a `notifications/tools/list_changed` notification is emitted exactly once.
- [ ] **Step 2:** Run — expect FAIL.
- [ ] **Step 3:** Emit the notification on route selection change and on route loss. Coalesce: at most one notification per 500 ms, and never during shutdown. Legacy-era clients that never declared interest still receive it — that is spec-legal for `2024-11-05` and harmless.
- [ ] **Step 4:** Run — expect PASS.
- [ ] **Step 5:** Commit as `feat(mcp): notify clients when live capability changes`.

---

### Workstream A exit gate

- `server/discover` answers before any handshake and lists both supported versions.
- A modern request and a legacy `initialize` session both work against the same binary in the same CI run.
- An unsupported version returns `-32022` with a `supported` array.
- Every tool carries `annotations`; the twelve core tools carry `outputSchema`; every `tools/call` returns `structuredContent`.
- `--strict-mcp` emits a surface with zero `io.saworbit.didi/*` keys, zero unimplemented names, and zero legacy names.
- `tools/list` is byte-identical across two fresh processes.
- Windows, Linux, and macOS CI green.

---

# Workstream C: Surface Governance

**Deliverable:** a written amendment process that makes it possible to add a tool name, plus the first four amendments.

- **C1 — Surface Amendment record.** Create `docs/SURFACE_AMENDMENTS.md`. Each entry: proposed name, the agent workflow that fails without it, execution modes, the safety class from the A5 annotation table, the test that will prove it, and the reviewer. Amend `ROADMAP.md:241` so "do NOT add more domain stubs" reads "do not add names without an accepted Surface Amendment" — the rule was written to stop stub proliferation, and an amendment record stops that just as firmly while unblocking real work.
- **C2 — Workflow validation ritual.** Add `docs/WORKFLOW_VALIDATION.md` defining one mandatory pre-phase exercise: run a single real task end to end (the standing example: *make the player double-jump and prove it works*) and log every point where the agent had to guess, poll, or go blind. Every logged blind spot becomes a candidate amendment. This is the mechanism fix — it finds missing capabilities without requiring the name to exist first.
- **C3 — File the first four amendments:** `runtime_read_output`, `runtime_step` (`until` parameter — a change, not a new name), `ui_list_controls`, `godot_api_reference`.
- **C4 — Retire the obsolete Phase 11 items.** `logging/setLevel` and `resources/subscribe` were removed from MCP; Roots and Sampling are deprecated. Delete them from `FUTURE_PHASES_DESIGN.md:169-195` and replace with `subscriptions/listen` and the MRTR pattern.

**Exit gate:** no capability can be added without a record; no roadmap item references a removed or deprecated MCP feature.

---

# Workstream B: Capability Gaps

Needs its own detailed plan. Scope and order, from the competitive analysis:

- **B1 — `runtime_read_output`.** Engine stdout/stderr from an attached session, with the cursor semantics `runtime_read_logs` already has, plus `category` and source file/line. Largest functional gap; four of six competitors have it.
- **B2 — `runtime_step` gains `until`.** Predicate restricted to the existing `eval_gdscript` read-only subset, with `max_frames` and a cooperative timeout. Same capability as satelliteoflove's `godot_game_time`, on a predicate language that cannot mutate the game.
- **B3 — `runtime_inject_input` (Phase 7C).** Ship as a bounded batch of `key | mouse_button | mouse_motion | action | wait | step | click_control`, not single events. `click_control` resolves through `ui_hit_test`.
- **B4 — `ui_list_controls`.** Bounded list of visible Controls with rect, type, and text. Makes B3's `click_control` addressable by name.
- **B5 — `godot_api_reference`.** Version-exact class, method, property, and signal lookup read from the in-repo `extension_api.json`. Retires the "small built-in offline class map" caveat on `script_reflect_class`.
- **B6 — LSP diagnostics backend.** Client to Godot's shipped language server (ports 6007, 6005, 6008), replacing the per-file `--headless --check-only` spawn and adding a workspace scan. Consuming Godot's LSP is not building one — the `FUTURE_PHASES_DESIGN.md:105` exclusion stands.
- **B7 — Opt-in game state contract + batched `scene_get_properties`.** Must report provenance and populate `omitted_fields` on truncation.
- **B8 — Debugger locals and `Performance` monitors.** The half of Phase 7C with no dependency on which script editor the user has open.

**Exit gate:** an agent can launch a game, watch real engine output, step until a stated condition holds, inject input, read state as JSON, and confirm the outcome — without a human relaying anything.

---

# Workstream D: Documentation and Roadmap Consolidation

Needs its own plan. Scope:

- **D1 — Restructure to four tiers.** *Start here* (README, QUICKSTART), *Use* (CAPABILITIES, TOOL_REFERENCE, LLM_INSTRUCTIONS), *Operate* (ADMIN_GUIDE, SECURITY, INTEGRATION_GUIDE), *Build* (ARCHITECTURE, DEVELOPER_GUIDE, API_SPECIFICATION). Move the six historical design and plan documents into `docs/history/` — they are records, and keeping them in the top-level list makes the current docs look twice as large as they are.
- **D2 — One roadmap.** `ROADMAP.md` and `FUTURE_PHASES_DESIGN.md` and `FUTURE_PHASES_IMPLEMENTATION_PLAN.md` currently overlap. Merge into `ROADMAP.md` as the single forward-looking document; `CAPABILITIES.md` stays the single present-tense document. Nothing else states status.
- **D3 — A compliance page.** New `docs/MCP_COMPLIANCE.md` stating: the supported revisions, the dual-era behavior, the one declared extension `io.saworbit.didi/capabilities` and precisely what it adds, the `--strict-mcp` switch, and an explicit list of core features Didi does not implement and why (Roots, Sampling, Logging — all deprecated upstream; Streamable HTTP — Didi is deliberately local-only, which is a security property, not a shortfall). This is the page you asked for: it says what aligns, what extends, and how to turn the extensions off.
- **D4 — README rewrite.** Lead with capability honesty and authenticated local IPC. Replace the "Legacy Script/CLI Wrappers / Multi-Hop Network Bridges" columns with named projects. Move export-exclusion guidance from `ADMIN_GUIDE.md:34` into the quickstart where a first-time user will see it.
- **D5 — Extend `tools/validate_documentation.py`** to fail CI when a documented tool name is absent from the registry, when `CAPABILITIES.md` and `tools/list` disagree on any execution mode, or when a doc references a removed MCP method.

**Exit gate:** every fact appears in exactly one place; CI fails when docs and the binary disagree.

---

# Workstream E: Distribution

Needs its own plan. Signed reproducible binaries for the three platforms; a one-line client config snippet per major client; an addon install and uninstall path; publish to the MCP server registry. Phase 12 material, but it gates every other workstream's reach — 60 working tools currently lose to 21 on installation friction alone.

**Exit gate:** a new user gets from zero to a working `tools/list` in under five minutes without a compiler.

---

## Self-Review

**Spec coverage.** Every removed, added, or deprecated feature in the `2026-07-28` changelog is addressed: handshake removal, per-request `_meta`, `server/discover`, `resultType`, `CacheableResult`, `extensions`, deterministic ordering, error-code renumbering, annotations, `structuredContent`, `outputSchema`, `listChanged` — all in Workstream A. Removed `ping`/`logging/setLevel`/`resources/subscribe` and deprecated Roots/Sampling/Logging — handled in C4 and documented in D3. MRTR and `subscriptions/listen` are scoped in C4 and D3 but deliberately **not** implemented in this program: both are only reachable once a real client exercises them, and neither blocks any Workstream B capability. That is a stated deferral, not a gap.

**Placeholder scan.** Tasks A1–A5 and A7 carry runnable test code, real file paths, and exact commands. A6 is a repeat of an established pattern across twelve tools and is deliberately compressed. Workstreams B, D, and E are **scoped, not planned** — each needs its own plan document before execution, as the writing-plans scope check requires. Do not execute them from this document.

**Type consistency.** `RequestContext`, `ProtocolEra`, `detectEra`, `kSupportedVersions`, `kExtensionId`, `kUnsupportedProtocolVersion` are defined in A1 and used unchanged in A2–A5. `decorateResult` is defined in A3. `setStrictMcp` is defined in A4 and used in A4's tests only. `makeErrorWithData` is flagged in A2 as possibly needing to be added.

**Known risk.** The compatibility floor is unverified: no current client is known to speak `2026-07-28` yet. Dual-era support makes this safe either way — that is the entire reason for Decision 1.
