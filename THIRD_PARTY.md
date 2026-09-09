# Third Party Code

Didi vendors a small number of third party sources directly into the tree. They
are copied files, not package manager entries, so nothing resolves or updates
them automatically. This page is the record, because a dependency nobody has
written down is a dependency nobody checks.

## Vendored sources

| File | Upstream | Version in tree | License |
| :--- | :--- | :--- | :--- |
| `include/didi/common/json.hpp` | [nlohmann/json](https://github.com/nlohmann/json) | 3.11.3 | MIT |
| `include/didi/common/stb_image_write.h` | [nothings/stb](https://github.com/nothings/stb) | v1.16 | Public domain or MIT, at your option |
| `include/didi/gdextension/gdextension_interface.h` | [godotengine/godot](https://github.com/godotengine/godot) | Godot 4.7 era, carries deprecation notes through 4.7 | MIT |

Each file keeps its upstream copyright header. Do not strip it.

`extension_api.json` and `gdextension_interface.h` at the repository root are
not these files. They are local dumps produced from a Godot build, they are
gitignored, and nothing compiles against them.

## Updating one

There is no command for this. Replace the file with the upstream release,
update the version in the table above, and run the full suite. The JSON header
parses every request that arrives from an MCP client, so treat a change to it
as a change to the code on the untrusted input path, not as a routine bump.

`gdextension_interface.h` is a compatibility contract, not just a header.
`addons/didi/didi.gdextension` declares `compatibility_minimum = "4.5"`, and the
live harness runs against real 4.5.1 and 4.7.2 editors. Both have to stay true.

## What Dependabot does and does not cover

Dependabot watches the GitHub Actions the workflows pin, weekly, and the Python
pin in `requirements-dev.txt`, monthly, through
[`.github/dependabot.yml`](.github/dependabot.yml).

Actions are pinned to commit SHAs rather than tags, each with a `# vX.Y.Z`
comment beside it. `tools/validate_documentation.py` rejects a workflow that
pins any other way, and Dependabot updates the SHA and its comment together, so
the pin costs nothing in maintenance.

Python version bumps used to be switched off. `requirements-dev.txt` pins
`jsonschema` exactly and CI asserts that pin, but the version was also typed
into both workflows, so any bump opened a pull request that failed until
somebody edited two more lines. Both workflows now read the version out of
`requirements-dev.txt`, so a bump either passes the schema contract suites or
it does not, and that is the whole review.

It does not watch anything in the table above, and it cannot: there is no
manifest for it to read. Those three files are reviewed by hand or not at all.
