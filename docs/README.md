# Didi Documentation Index

Every page under `docs/`, grouped by what you came here to do. The
[repository README](../README.md) carries the short list for a first read. This
one is complete, so a page cannot sit in the tree with nothing pointing at it.

Where a document carries a status banner, the status below repeats it. A design
record is kept for the reasoning, not because it describes today's behaviour.
[Current Capability Matrix](CAPABILITIES.md) is always the current answer.

## Start here

| Page | What it is for |
| --- | --- |
| [Quickstart Guide](QUICKSTART.md) | Getting Didi running with Godot and one assistant in about five minutes. |
| [Integration Guide](INTEGRATION_GUIDE.md) | Installing the addon into an existing project and wiring each supported assistant to it. |
| [LLM Operating Instructions](LLM_INSTRUCTIONS.md) | The system prompt and decision tree to hand an agent. |

## What Didi does today

| Page | What it is for |
| --- | --- |
| [Current Capability Matrix](CAPABILITIES.md) | The authoritative list of what is live, offline, unavailable and unimplemented. |
| [Tool Reference](TOOL_REFERENCE.md) | Behaviour, arguments and limits for every canonical tool and legacy name. |
| [Managed Recovery](MANAGED_RECOVERY.md) | Opt-in owned-editor startup, saved-file checkpoints, reconciliation, and recovery limits. |
| [Resources and Prompts](RESOURCES_AND_PROMPTS.md) | The `godot://` resources and the prompt templates. |

## How it works

| Page | What it is for |
| --- | --- |
| [Architecture](ARCHITECTURE.md) | The C++20 design, the dual execution topology, threading, and the IPC transport. |
| [API and Protocol Specification](API_SPECIFICATION.md) | JSON-RPC 2.0 over the wire and the binary frame format. |
| [Administrator and Operations Guide](ADMIN_GUIDE.md) | DACL hardening, headless CI, observability and troubleshooting. |
| [Developer and Extension Guide](DEVELOPER_GUIDE.md) | Building from source, the test suites, and adding a tool. |
| [Brand Identity](brand/BRAND.md) | The mark, wordmark, lockups and palette, and the assets they generate from. |

## The surface, and where it is going

| Page | Status |
| --- | --- |
| [Roadmap](ROADMAP.md) | Completed phases and the technical build order. |
| [Surface Amendments](SURFACE_AMENDMENTS.md) | The record of every accepted change to the canonical tool surface. An amendment is accepted here before a name is registered. |
| [Phase 7 API Feasibility Gate](PHASE_7_API_FEASIBILITY.md) | Reproducible feasibility results on Godot 4.5.1 and 4.7.2, and the three contracts the engine API blocks. |
| [Phase 7 Canonical Completion Plan](PHASE_7_IMPLEMENTATION_PLAN.md) | The approved plan, stopped at its feasibility gate. |
| [Phase 7 Partial Delivery Plan](PHASE_7_PARTIAL_IMPLEMENTATION_PLAN.md) | The plan for the names that did ship, with the live signal bridge evidence behind them. |
| [Post-Phase-6 Roadmap Design](FUTURE_PHASES_DESIGN.md) | The design for what follows Phase 6. |
| [Future Phases Documentation Plan](FUTURE_PHASES_IMPLEMENTATION_PLAN.md) | How that design lands in the documentation. |

## Design records

Reasoning kept in place. Read the status line at the top of each before treating
any of it as current behaviour.

| Page | Status |
| --- | --- |
| [Control Room Design](CONTROL_ROOM_DESIGN.md) | Implemented. The MCP Apps dashboard the host renders in the conversation, and why it waited for its condition. |
| [Autonomous Pipeline Design](AUTONOMOUS_PIPELINE_DESIGN.md) | Design approved. The loop that finds defects, fixes them and proves the fixes. |
| [Field Trial Design](FIELD_TRIAL_DESIGN.md) | Design approved. Method and apparatus for handing Didi to an agent that has never seen it. |
| [Field Trial Implementation Plan](FIELD_TRIAL_IMPLEMENTATION_PLAN.md) | The executable plan for that trial. The seed, briefing and scoring script live in `tools/field-trial/`. |
| [Field Trial Results](FIELD_TRIAL_RESULTS.md) | One section per run, numbers kept so two runs can be compared. |
| [Gogo Design](GOGO_DESIGN.md) | Design only. The parallel Godot bench farm. Nothing of it is implemented. |
| [Engine Failure Handling Design](ENGINE_FAILURE_HANDLING_DESIGN.md) | Implemented; superseded in part by managed recovery's opt-in owned-editor restart. Historical reasoning for ordinary attachment remains. |
| [Human Interaction Design](HUMAN_INTERACTION_DESIGN.md) | Implemented. Records what the editor console became, where it went past what this document recommended, and why the MCP Apps step waited for its condition. |
| [Realignment Program Plan](REALIGNMENT_IMPLEMENTATION_PLAN.md) | Partly implemented. Its checkboxes were never maintained, so read them as the original proposal rather than as progress. |
| [Lab Orchestrator Design](LAB_ORCHESTRATOR_DESIGN.md) | Superseded. A redirect to [Gogo Design](GOGO_DESIGN.md). |

## Completed historical records

Finished work, kept for the reasoning. None of it describes behaviour you have
to check before using Didi.

| Page | What it recorded |
| --- | --- |
| [GitHub Safety Batch Design](GITHUB_SAFETY_BATCH_DESIGN.md) | The design for a batch of safety reports from the post-Phase-4 backlog. |
| [GitHub Safety Batch Implementation Plan](GITHUB_SAFETY_BATCH_IMPLEMENTATION_PLAN.md) | The plan that delivered it. |
| [macOS CI Tap Warning Design](CI_MACOS_TAP_WARNING_DESIGN.md) | Why the macOS CI job emitted a Homebrew tap-trust warning, and what to do about it. |
| [macOS CI Tap Warning Implementation Plan](CI_MACOS_TAP_WARNING_IMPLEMENTATION_PLAN.md) | The plan that removed it. The enforced contract now lives in `tools/validate_documentation.py` and the CI workflow. |
