# Test Inventory

**Generated file. Do not edit by hand.** Regenerate with `python tools/test_inventory.py`; CI runs `--check` after the build and fails when this page and the suites disagree.

Every number here is derived from the suites themselves rather than written down beside them. The native total comes from the test binary's own registry, the Python totals from parsing the test modules, and the harness total from counting its assertions. A test added without a line appearing here means the generator is wrong, which is a defect worth knowing about.

**These are the Windows figures.** The native suite is platform-conditional and the difference is not small: crash capture is Windows-only, and the IPC cases differ because a named pipe and a Unix socket are not the same transport, so the POSIX runners register roughly a dozen fewer native tests. Every platform runs the whole Python suite. Windows is the reference here because it is the only platform with the live Godot integration harness, and because it runs the largest native suite, so nothing below is an overstatement of what another platform does.

## Totals

| Measure | Count |
| --- | ---: |
| Automated tests | **1256** |
| Live-harness assertions | 1225 |

The badge in the [README](../README.md) shows the automated test total. Harness assertions are counted separately because they are assertions inside one live scenario, not independently runnable cases; adding them together would flatter the number.

## Native C++ suite (`didi_tests`)

**832 tests.** Derived from `didi_tests --list`, which prints the registry the runner iterates.

| Suite | Tests |
| --- | ---: |
| `AnimAddLibrary` | 3 |
| `Base64` | 1 |
| `Blackboard` | 13 |
| `BlackboardResources` | 8 |
| `BlackboardTasks` | 8 |
| `CaptureCache` | 3 |
| `Checkpoints` | 13 |
| `ControlRoom` | 24 |
| `CrashCapture` | 3 |
| `EditorHook` | 5 |
| `EngineDiagnostics` | 7 |
| `ErrorData` | 3 |
| `ExportPresetAdd` | 9 |
| `ExpressionSandbox` | 7 |
| `GDScript` | 24 |
| `Hierarchy` | 9 |
| `IPC` | 31 |
| `ImageDiff` | 10 |
| `ImportHealth` | 34 |
| `InvariantWatch` | 4 |
| `JsonRpc` | 4 |
| `ManagedProcess` | 6 |
| `McpServer` | 41 |
| `Phase5` | 26 |
| `Phase6` | 25 |
| `Phase7Contract` | 9 |
| `Phase7Diagnostics` | 1 |
| `Phase7Navigation` | 1 |
| `Phase7Physics` | 1 |
| `Phase7Signals` | 11 |
| `Phase7TileGrid` | 7 |
| `Phase7Viewport` | 4 |
| `ProcessRunner` | 2 |
| `ProjectSearch` | 17 |
| `Prompts` | 1 |
| `ResourceIndexer` | 12 |
| `Resources` | 2 |
| `RuntimeLaunch` | 11 |
| `RuntimeLogs` | 12 |
| `RuntimeOutput` | 5 |
| `RuntimeRouting` | 62 |
| `RuntimeSessions` | 31 |
| `RuntimeTree` | 1 |
| `SceneCallMethod` | 6 |
| `SceneExploration` | 12 |
| `Schema` | 1 |
| `Segmentation` | 5 |
| `SpeculativeVerify` | 8 |
| `TestRunner` | 2 |
| `ToolManifest` | 1 |
| `Tools` | 143 |
| `UiListControls` | 6 |
| `ViewportIsolation` | 2 |
| `autoload_diagnostics` | 9 |
| `config_file_syntax` | 28 |
| `ghost_preview` | 4 |
| `phase7` | 1 |
| `phase7b_anim` | 6 |
| `phase7b_spatial` | 8 |
| `phase7c_input` | 6 |
| `phase7c_profiler` | 10 |
| `project_settings_file` | 16 |
| `resource_create` | 5 |
| `resource_references` | 15 |
| `tool_annotations` | 10 |
| `tool_input_schema` | 6 |
| `tool_manifest` | 7 |
| `tool_modes` | 1 |
| `tool_output_schema` | 3 |

## Python contract suites (`tests/test_*.py`)

**424 tests.** Derived from `test_*` methods on every `unittest.TestCase` subclass, read with `ast`.

| File | Tests |
| --- | ---: |
| `test_check_release_archive.py` | 15 |
| `test_ci_change_classification.py` | 7 |
| `test_cli_arguments.py` | 13 |
| `test_control_room_app.py` | 14 |
| `test_control_room_protocol.py` | 17 |
| `test_didi_binary.py` | 7 |
| `test_documentation_validator.py` | 90 |
| `test_editor_console.py` | 9 |
| `test_elicitation_confirmation.py` | 6 |
| `test_engine_identity.py` | 5 |
| `test_field_trial.py` | 137 |
| `test_fuzz_target_lists.py` | 6 |
| `test_managed_recovery.py` | 2 |
| `test_managed_recovery_adversarial.py` | 3 |
| `test_managed_recovery_live.py` | 3 |
| `test_phase7_plan_ownership.py` | 10 |
| `test_phase7_schema_contract.py` | 4 |
| `test_phase7_signal_admission.py` | 3 |
| `test_refusal_codes.py` | 10 |
| `test_test_inventory.py` | 25 |
| `test_tool_output_schema_contract.py` | 5 |
| `test_vendored_versions.py` | 23 |
| `test_yolo_mode.py` | 10 |

## Live Godot harness (`tests/*.ps1`)

**1225 assertions.** Derived from `Assert-True` call sites. The harness is one long live scenario rather than a set of named cases, so this counts assertions and says so.

| File | Assertions |
| --- | ---: |
| `run_godot_integration.ps1` | 1225 |

## What is not counted here

- The end-to-end MCP conversation in `.github/workflows/ci.yml`, which drives the built binary over stdio and asserts the wire surface against the manifest that same binary emits.
- `tools/validate_documentation.py`, which checks the documentation contract rather than the code, and reports its own findings.
- Compiler and sanitizer diagnostics. ASan and UBSan run the native suite again under instrumentation; they add coverage, not cases.
- The libFuzzer targets in `fuzz/`. They generate their own inputs rather than asserting a fixed set, so counting them as tests would be counting the wrong thing: three targets is not three cases, and the number that matters is the corpus, which grows on its own.

