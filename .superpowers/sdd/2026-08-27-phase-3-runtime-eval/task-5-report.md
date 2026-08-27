# Task 5 report: read-only bounded `eval_gdscript`

## Status

Implemented authenticated live expression evaluation for editor and game
sessions. The tool accepts expressions only, validates them with a token scanner,
prebinds the narrow accepted form `node.get(<string literal>)` only after proving
the property is native and ClassDB-defined, executes Godot `Expression` on the
main thread with `const_calls_only=true`, confines context nodes to the active
session scope, and returns bounded typed JSON with live-execution provenance.

The implementation does not compile or run scripts. It does not log expression
source, and every constructed Godot `Expression` is owned by an RAII guard that
destroys it on every exit path.

## RED evidence

### Missing implementation

The policy tables were added to `tests/test_expression_sandbox.cpp` before the
production header and source. Focused build command:

```powershell
cmake --build build --config Release --target didi_tests
```

Expected RED output:

```text
tests/test_expression_sandbox.cpp(1,10): fatal error C1083:
Cannot open include file: 'didi/gdextension/expression_sandbox.hpp'
```

The Windows environment supplied both `Path` and `PATH`, which causes MSBuild to
reject the environment. Subsequent build evidence used the same CMake command in
a child process with one normalized `Path` entry; no compiler or test behavior
was changed.

### Non-finite typed conversion regression

Final self-review added a live request for `Vector2(INF, 0)` before changing the
converter. Godot 4.5.1 then failed as expected:

```text
Game expression rejection 340 returned fake success.
```

Vector2, Vector3, and Color conversion now rejects every non-finite component
with a structured 415 error instead of allowing the JSON encoder to substitute
`null`.

### Script-callback security correction

Security review identified that Godot's const-call classification does not make
Object reads or implicit conversions inert: `node.get(...)`, `node.property`,
and `node[...]` can reach a script property getter or `_get`, while `str(node)`
or `% node` can reach `_to_string`. Callback-rejection tests were added before
each correction. The initial callback cases, and then the member/index bypasses,
each produced the expected native RED:

```text
[  FAILED  ] ExpressionSandbox.DocumentedVocabulary
Results: 51 passed, 1 failed, 52 total.
```

The final policy forbids Object stringification and formatting, non-call member
access, and dynamic indexing; rejects arbitrary/chained/container `.get`; and
accepts only exact direct `node.get(<unescaped ASCII property-name literal>)`.
Before Expression parsing, those reads are rewritten to reserved inputs
populated through `ClassDB.class_get_property`, after
`ClassDB.class_get_property_getter` proves the property is native. Only
scalar/vector value types can be prebound. Editor and game fixtures supply
malicious property getters, `_get`, and `_to_string` callbacks; rejected direct,
member, index, string, format, and chained-read requests leave a native counter
property unchanged.

### Bounded literal result generator

The live oversized-result test required a side-effect-free expression after
Object stringification was removed. A test for `'x'.repeat(3)` was added first
and produced the expected native RED:

```text
[  FAILED  ] ExpressionSandbox.ReadOnlyContainersAndMath
Results: 51 passed, 1 failed, 52 total.
```

The final scanner permits `repeat` only when the receiver is a string literal,
the sole argument is an unsigned integer literal, and the statically estimated
output is at most 512 KiB. It is not a general callable surface. The independent
256 KiB result-conversion budget rejects the 300,000-byte integration result.

## ABI verification

The official Godot 4.5.1 and 4.7.2 binaries were asked to dump their extension
API metadata. Both versions reported the same method hashes used by the raw ABI
bridge:

```text
Expression.parse              3069722906
Expression.execute            3712471238
Expression.has_execute_failed   36873697
Expression.get_error_text      201670096
ClassDB.class_get_property_getter 3770832642
ClassDB.class_get_property        2498641674
```

`Expression.parse` also advertises compatibility hash `3658149758` in both
versions. There was no ABI blocker.

## Mutation evidence

### Policy allowlist mutation

`set` was temporarily added to the callable allowlist and removed from the
forbidden-identifier set. After rebuilding, the native suite failed exactly the
independent documented-vocabulary test:

```text
[  FAILED  ] ExpressionSandbox.DocumentedVocabulary
Assertion failed: didi::godot::ExpressionPolicy::validate(source).isErr()
Results: 51 passed, 1 failed, 52 total.
```

This proves the native test's rejected literals are independent of the
production allowlist.

### `const_calls_only` mutation

The controlled permissive-`set` precondition was retained to isolate the engine
defense. With `const_calls_only=true`, the Godot 4.5.1 integration remained
green because Godot rejected `node.set(...)`. Changing only the execute argument
to `const_calls_only=false`, rebuilding, and rerunning produced:

```text
Game expression rejection 327 returned fake success.
```

Both mutations were restored. The final source has `set` forbidden and passes a
true `GDExtensionBool` as the fourth `Expression.execute` argument.

## GREEN evidence

Release build, using the normalized child environment described above:

```powershell
cmake --build build --config Release
```

Output:

```text
didi_core.vcxproj -> build\Release\didi_core.lib
didi.vcxproj -> build\Release\didi.exe
didi_extension.vcxproj -> build\Release\didi_extension.dll
didi_tests.vcxproj -> build\Release\didi_tests.exe
BUILD_EXIT=0
```

Native suite:

```powershell
.\build\Release\didi_tests.exe
```

Output:

```text
Results: 52 passed, 0 failed, 52 total.
```

Godot 4.5.1:

```powershell
.\tests\run_godot_integration.ps1 `
  -GodotExecutable 'C:\Godot\Godot_v4.5.1-stable_win64_console.exe' `
  -Configuration Release
```

Godot 4.7.2:

```powershell
.\tests\run_godot_integration.ps1 `
  -GodotExecutable 'C:\Godot\Godot_v4.7.2-stable_win64_console.exe' `
  -Configuration Release
```

Both produced:

```text
Godot integration passed: Phase 1/2 editor workflows plus concurrent Phase 3 game tree and execution control.
```

The concurrent live fixture covers native scalar-property prebinding, child
counts, arrays, dictionaries, Vector2, editor/game default contexts, game frame
metadata, Node summaries, provenance, harmless strings containing dangerous
words, and secret source absence from the extension log ring. It rejects
mutation, dynamic dispatch, filesystem/process access, identifier obfuscation,
NUL and oversized source, timeout bounds, missing/escaped contexts, parse and
execution errors, deep and oversized results, SceneTree/non-Node Objects,
non-finite typed components, script property getters, script `_get`, chained
Object reads, and implicit `_to_string` paths. Pre/post counter reads prove the
malicious callbacks caused no side effects in both editor and game sessions.
Native tests additionally exercise malformed UTF-8 directly.

Final independent security re-review reported no unresolved Critical or
Important finding. Its sole minor observation was that a Godot String was copied
to `std::string` before enforcing the 256 KiB limit. The final converter now asks
Godot for the UTF-8 byte length and rejects an oversized String or dictionary key
before allocating the copy. The full native and two-engine matrix above was
rerun after that correction.

PowerShell syntax validation also passed:

```text
PowerShell parser: OK
```

## Files changed

- `CMakeLists.txt`
- `include/didi/gdextension/expression_sandbox.hpp`
- `src/gdextension/expression_sandbox.cpp`
- `src/gdextension/editor_hook.cpp`
- `src/gdextension/godot_bridge.cpp`
- `src/tools/runtime_tools.cpp`
- `tests/test_expression_sandbox.cpp`
- `tests/run_godot_integration.ps1`
- `tests/godot_smoke/main.tscn`
- `tests/godot_smoke/runtime_probe.gd`
- `tests/godot_smoke/malicious_probe.gd`

## Self-review and concerns

- The scanner reads UTF-8 bytes into identifiers, numbers, strings,
  punctuation, and operators while preserving byte spans for safe source
  rewriting. It rejects controls and non-ASCII tokens outside strings,
  validates escapes, ignores all quoted contents, rejects assignment and
  statement punctuation, blocks the high-risk identifier set and reserved
  sandbox names, and requires every syntactic call target to satisfy the
  read-only policy. Raw regular-expression matching is not used.
- Object stringification/coercion operators and helper calls, property-member
  syntax, and dynamic indexed reads are forbidden. Exact direct native property
  reads are prebound through ClassDB and never execute Object `get`;
  script/dynamic properties, Object/container values, escaped property names,
  and chained/arbitrary `.get` are rejected.
- Host-side validation fails before IPC forwarding; the live bridge repeats the
  validation before touching the engine. Godot execution is reached only from
  `EditorHook::processQueue`, so all Expression and SceneTree/Object calls remain
  on the registered main-loop thread.
- Editor contexts remain under the current edited-scene root. Game contexts
  remain under `/root`. Canonical-path checks reject relative segments, aliases,
  NodePath subnames, duplicate/trailing separators, backslashes, and paths over
  1,024 bytes. Returned editor Nodes are rechecked against the edited-scene
  subtree before summary conversion.
- Result conversion is allow-by-type: null, bool, integer, finite float,
  String/StringName/NodePath, bounded Array/Dictionary with string keys,
  finite Vector2/Vector3/Color, and typed Node summaries. Non-Node Objects and
  all other Variant types fail with structured errors; objects are never
  stringified as a conversion fallback.
- Conversion is bounded to depth 16 and a conservative 256 KiB serialized-size
  budget. Deadline checks run at every recursive conversion entry and again
  after string and result conversion. String byte length is checked through the
  engine before allocating a UTF-8 copy. The final response is independently
  size-checked. Source is capped at 2,048 bytes and timeout is restricted to
  1..5,000 ms.
- Timeout enforcement is cooperative at the required validation, context,
  parse, and execute boundaries. Godot's `Expression.execute` is synchronous and
  cannot be preempted through this ABI; safety therefore also depends on the
  bounded expression grammar and `const_calls_only=true`.
- The first 4.5.1 run crashed before evaluation because concurrent editor/game
  instances collided on Godot's shared `user://logs` file. The fixture now gives
  each process a unique build-local `--log-file`; both engine versions pass.
- No unresolved correctness or security concern remains within the Task 5
  contract. Broader adversarial/stress coverage remains assigned to Task 6.

## Fix round 1: conservative read-only boundary

### Status

The post-implementation security review found two Critical, three Important,
and one Minor issue in the first Task 5 commit. This fix round addresses every
finding by shrinking the executable vocabulary and making output handling and
conversion bounds apply before potentially large host allocations.

`in` is now forbidden outright. Object-returning traversal, metadata access,
live container enumeration, unrestricted chaining, and method calls on
unproven receivers are rejected before IPC forwarding and again in the engine.
The remaining direct Node methods are exact native scalar queries with literal
arguments: `get_child_count`, `get_path`, `get_class`, `is_class`,
`is_in_group`, `has_method`, and `has_meta`. `node.get(<literal>)` remains a
host-rewritten ClassDB-proven native property read; it never executes
`Object.get`. String/Array/Dictionary methods require a source-local literal
receiver, and global constructors/math accept source-local numeric arguments
only.

### RED evidence

Tests were added before the policy and logging corrections. The native suite
then reported:

```text
[  FAILED  ] McpServer.OutputLoggingRedactsBodies
[  FAILED  ] ExpressionSandbox.DocumentedVocabulary
Results: 51 passed, 2 failed, 53 total.
```

The logger test drives the real standalone `runStdio` path, places a secret
only in the serialized response value, and captures the configured Logger sink
and stderr. This distinguishes required JSON-RPC stdout from diagnostic leaks.

The first Godot 4.5.1 run against the unchanged extension reached the new live
malicious Object containment case and failed as intended:

```text
Game expression rejection 350 returned fake success.
```

Both editor and game fixtures now include scripted `_get`, property getters,
and `_to_string` callbacks, a detached Node held in metadata, a 300,000-byte
metadata String, and 1,024 live children. Pre/post native scalar property reads
prove that containment, traversal, metadata, live enumeration, member/index,
and implicit-conversion attempts are rejected without script side effects.
Explicit `/root` and `..` traversal expressions include chained scalar escape
attempts through `get_child_count()` and `get_path()`.

### Security corrections

- `in` is a forbidden identifier, so Godot Object containment cannot dispatch
  script `_get`.
- Receiver classification is structural over scanner tokens. Unknown method
  receivers, arbitrary chains, Object-returning traversal, `get_meta`,
  `get_children`, signal/group/property enumeration, and live `find`/`count`
  paths are rejected. A self-review regression additionally rejects global
  helpers or constructors fed by `node`, `tree`, or nested live calls.
- Returned Nodes are always summarized relative to the active scope root for
  editor and game sessions. Empty, absolute, `..`-escaping, non-canonical, and
  oversized paths fail instead of escaping as metadata.
- Successful responses no longer echo expression source. Standalone JSON-RPC
  response and notification logs contain only kind and serialized byte count;
  full result bodies are not copied to the Logger sink or stderr.
- Dictionary size is queried and capped at 4,096 entries before `keys()`.
  String/StringName/NodePath UTF-8 length is queried in Godot before allocating
  a C++ copy. Native property strings, class names, root names, context paths,
  dictionary keys, and Node summary paths/classes all receive explicit byte
  bounds based on the remaining result budget.
- Recursive conversion checks the cooperative deadline at entry and after
  string, key-array, typed JSON, and Node-summary work. The final response is
  dumped for size, the deadline is checked after that dump, and `elapsed_ms` is
  measured only afterward.
- `Expression.execute` still receives `const_calls_only=true`; Expression RAII
  ownership and main-thread execution are unchanged.

The timeout remains deliberately documented as cooperative: Godot's raw
`Expression.execute` call is synchronous and cannot be preempted through this
ABI. The executable vocabulary is therefore restricted to bounded literals,
prebounded scalar/vector/color inputs, and exact native scalar Node queries.

### Fix-round mutation evidence

Three fix-specific mutations were applied one at a time, rebuilt, observed to
fail, and restored:

1. Removing `in` from the forbidden set:

   ```text
   [  FAILED  ] ExpressionSandbox.DocumentedVocabulary
   Results: 52 passed, 1 failed, 53 total.
   ```

2. Re-admitting `get_node`, `get_node_or_null`, and `has_node` as direct Node
   methods:

   ```text
   [  FAILED  ] ExpressionSandbox.DocumentedVocabulary
   Results: 52 passed, 1 failed, 53 total.
   ```

3. Restoring full serialized response logging at `MCP_OUT`:

   ```text
   [  FAILED  ] McpServer.OutputLoggingRedactsBodies
   Results: 52 passed, 1 failed, 53 total.
   ```

The earlier `set`-allowlist and `const_calls_only=false` mutations remain valid
and were not affected by this narrower policy.

### Final GREEN evidence

Release build with the inherited Windows environment normalized to one `Path`
entry:

```powershell
cmake --build build --config Release
```

```text
didi_core.vcxproj -> build\Release\didi_core.lib
didi.vcxproj -> build\Release\didi.exe
didi_extension.vcxproj -> build\Release\didi_extension.dll
didi_tests.vcxproj -> build\Release\didi_tests.exe
```

Native suite:

```powershell
.\build\Release\didi_tests.exe
```

```text
Results: 53 passed, 0 failed, 53 total.
```

Godot 4.5.1 and 4.7.2 integrations, rerun after the final scalar traversal
regressions were added:

```powershell
.\tests\run_godot_integration.ps1 -GodotExecutable 'C:\Godot\Godot_v4.5.1-stable_win64_console.exe' -Configuration Release
.\tests\run_godot_integration.ps1 -GodotExecutable 'C:\Godot\Godot_v4.7.2-stable_win64_console.exe' -Configuration Release
```

Both reported:

```text
Godot integration passed: Phase 1/2 editor workflows plus concurrent Phase 3 game tree and execution control.
```

PowerShell parser validation and `git diff --check` also passed. The sole
`diff --check` diagnostic is Git's informational LF-to-CRLF working-copy
warning for the existing PowerShell file policy, not a whitespace error.

### Files changed in fix round 1

- `src/gdextension/expression_sandbox.cpp`
- `src/mcp/mcp_server.cpp`
- `tests/test_expression_sandbox.cpp`
- `tests/test_jsonrpc.cpp`
- `tests/run_godot_integration.ps1`
- `tests/godot_smoke/runtime_probe.gd`
- `tests/godot_smoke/malicious_probe.gd`

### Self-review and concerns

- The native vocabulary tables are independent literals rather than being
  generated from production sets. Live assertions distinguish host-policy
  rejection from an engine-side failure, proving dangerous cases never reach
  `Expression.parse` or execute.
- Source-local receiver checks conservatively reject unknown/nested receivers.
  Literal `repeat` remains syntax-restricted and statically capped; it is not a
  general callable.
- ClassDB property prebinding remains limited to native ClassDB-defined
  scalar/vector/color/string-like types. Script/dynamic properties, Objects,
  containers, Callable/Signal, escaped names, and unbounded strings fail before
  Expression execution.
- Safe `get_child_count` and `get_path` cases run successfully against the
  deliberately large in-scope fixtures, while traversal and enumeration are
  rejected before execution.
- No raw ABI or plan-contract blocker was encountered. The unavoidable caveat
  is cooperative rather than preemptive timeout enforcement for the synchronous
  Godot Expression ABI; the conservative grammar is part of that safety bound.

Final independent security re-review reported no unresolved Critical or
Important finding. It confirmed that the prior Minor oversized-engine-string
finding is resolved by the length-before-copy checks. Its only non-blocking
caveats are the documented cooperative timeout model and the necessary trust in
Godot core/ClassDB-native property getters; a separately loaded hostile native
GDExtension already has process authority and is outside this expression
sandbox's isolation boundary.
