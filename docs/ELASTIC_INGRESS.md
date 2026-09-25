# Optional elastic ingress

The first increment adds an explicit `safe-v1` normalization profile for three
read-only tools. Build with `-DDIDI_ELASTIC_INGRESS=ON` to enable it; default OFF.
It adds no tool names, Godot calls, reflection, or evaluator.

Read the [decision record](ELASTIC_INGRESS_DECISION.md) for motivation, alternatives,
client limitations, adoption gates, and the independent Windows timeout fix.
Observed results are in [local verification](ELASTIC_INGRESS_VALIDATION.md).

## Isolated validation

```powershell
cmake -S . -B out/elastic-enabled -DDIDI_ELASTIC_INGRESS=ON -DDIDI_BUILD_TESTS=ON
$env:_CL_ = '/MP2' # MSVC only: limit compiler concurrency
cmake --build out/elastic-enabled --config Debug --parallel 1
$env:DIDI_TEST_BINARY = (Resolve-Path out/elastic-enabled/Debug/didi.exe).Path
$env:DIDI_TEST_ELASTIC_INGRESS = '1'
ctest --test-dir out/elastic-enabled -C Debug --output-on-failure
```

Repeat with a separate `out/elastic-disabled` directory, the option OFF, and
`DIDI_TEST_ELASTIC_INGRESS=0`, updating the executable path. Single-config
builds put executables directly in the build directory. Existing extension
staging stays beneath this worktree. Do not install into an active project.

## Wire contract

Enabled `tools/list` entries advertise `_meta.didi.argumentNormalization` with
`profile: safe-v1` and `requestMetaKey: didi/argumentNormalization`.
The published input schema remains the canonical representation. A client must
understand this profile and be able to set request metadata; schema-only clients
continue to use canonical arguments.
After normal MCP negotiation, a caller explicitly selects the profile:

```json
{"jsonrpc":"2.0","id":42,"method":"tools/call","params":{"name":"ui_hit_test","arguments":{"point":["12.5",-3],"max_results":"8"},"_meta":{"didi/argumentNormalization":"safe-v1"}}}
```

| Tool | Permitted conversion |
| --- | --- |
| `scene_get_hierarchy` | Signed decimal strings for `max_depth`, `max_nodes` |
| `ui_list_controls` | Signed decimal strings for `max_results` |
| `ui_hit_test` | Signed decimal strings for `max_results`; exact `[x,y]` to `{x,y}`; finite numeric strings for x/y |

Integer strings accept an optional leading minus followed by decimal digits;
a leading plus is rejected. Leading zeroes are accepted.
Integer fields reject whitespace, partial parses, fractions, exponent strings,
booleans, and signed 64-bit overflow. Schema ranges still apply. Coordinate
strings use locale-independent binary64 parsing, including decimal exponents;
a leading plus is rejected (an exponent sign such as `1e+2` is accepted).
Nonfinite values and overflow are refused. Parsed string magnitudes must be
strictly below 2^53 so integer spellings cannot round onto that boundary.
JSON integer coordinates outside
[-2^53, 2^53] are refused before conversion. Point objects contain only x/y.
Other fields retain their schema rules. Paths, booleans, units, colors,
transforms, aliases, and mutation arguments are not coerced.

## Bounds and diagnostics

Before copying arguments, one cumulative budget checks depth <=8 (root 0),
4096 values including containers, and 65536 conservative serialized bytes.
Strings and keys are charged at worst-case JSON escaping size. The transport's
existing protections still govern parsing before this perimeter is reached.
No limit or conversion applies to calls that do not select the profile.

A temporary object is normalized and strictly validated before dispatch. The
registry performs its normal validation again. No partial/default success is
returned and no engine operation occurs during normalization.

Failures retain the `isError` tool result with the existing 400 envelope,
`data.code: invalid_arguments`, `retryable: false`, `tool`, `canonical_tool`,
`field`, and stable `reason`. Conversion failures also provide `expected`.
Diagnostics use fixed policy strings, never raw input, and stay below 2 KiB.
Schema failures omit input-derived prose; consult the published input schema.

Reasons include `feature_disabled`, `unsupported_profile`, `unsupported_tool`,
`expected_integer`, `expected_finite_number`, `inexact_integer`,
`invalid_vector_shape`, `input_budget_exceeded`, `expected_object`, and
`schema_validation_failed`. Unknown tools retain their existing protocol error.
Disabled builds reject explicit profile requests and do not advertise support.

The implementation is pure JSON in `src/mcp/argument_normalization.cpp`.
Godot integration here uses the GDExtension C interface; future work must not
assume godot-cpp types. Reflection, generic mutations, fuzzy target selection,
and script evaluation require separate designs. In-process source filtering
is not a general GDScript sandbox. Cross-platform and performance evidence
remain release gates; there is no claim of zero runtime risk.

## Real-editor acceptance

With the enabled build and a local Godot installation, run the retained session
regression in a disposable managed project:

```powershell
$env:DIDI_TEST_BINARY = (Resolve-Path out/elastic-validation/Debug/didi.exe).Path
$env:DIDI_RECOVERY_GODOT = 'C:\Godot\Godot_v4.6.2-stable_win64_console.exe'
python -m unittest discover -s tests -p test_managed_recovery_live.py -k elastic_ingress
```

The test opens a two-control scene, compares canonical and normalized live
results for all three tools, and recovers after 22 malformed requests in the
same session. It verifies the expected button hit, no injected input, unchanged
scene bytes, and unchanged source fixture. Without the required environment
variables it skips; a disabled build also skips the profile-specific scenario.
