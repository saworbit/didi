## Description
<!-- Provide a brief description of the changes introduced by this PR. -->

## Related Issues
<!-- Link related issues, e.g. Fixes #123 -->

## Type of Change
- [ ] Bug fix (non-breaking change which fixes an issue)
- [ ] New feature (non-breaking change which adds new MCP tools or capabilities)
- [ ] Breaking change (fix or feature that would cause existing functionality to not work as expected)
- [ ] Documentation update

## Checklist
- [ ] My code adheres to the project's coding style guidelines (C++20, 4 spaces).
- [ ] I have added automated tests in `tests/` covering new functionality.
- [ ] All native unit tests pass (`./build/Release/didi_tests.exe` or `ctest`).
- [ ] Live bridge changes pass `tests/run_godot_integration.ps1` on Godot 4.5.1.
- [ ] Runtime-session changes cover editor and game descriptors, authentication, attach rollback, pause/step/stop, cursor polling, cleanup, and token redaction.
- [ ] Expression changes cover the strict read-only grammar, receiver allowlist, output/depth bounds, and cooperative-timeout wording.
- [ ] The exact MCP smoke starts with an explicit Godot project, returns 78 canonical tools / 88 registrations, preserves the Phase 4/5/6 contracts, and keeps reserved runtime debugger tools marked unimplemented.
- [ ] Tool counts, capability metadata, reference docs, roadmap, and changelog are current.
- [ ] I have updated relevant documentation in `docs/` and `README.md`.
