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

CodeQL findings in these files are dismissed rather than excluded.
`stb_image_write.h` alone accounted for five of the ten findings in CodeQL's
first pass over this repository, all integer-multiplication overflows in code
this project is told not to modify. Five alerts that can never be actioned are
how a Security tab stops being read.

Dismissal rather than exclusion is forced rather than preferred: `paths-ignore`
does not apply to a compiled language whose analysis builds the code, so there
is no configuration that removes them. Each carries a written reason pointing
here. The risk of a vendored file is managed by the process below -- knowing
what is in the tree and what updating it involves -- not by an alert nobody can
close. The fuzz targets in [`fuzz/`](fuzz/README.md) cover `json.hpp` where it
matters, by driving it through this project's own parsers on the untrusted
input path.

`extension_api.json` and `gdextension_interface.h` at the repository root are
not these files. They are local dumps produced from a Godot build, they are
gitignored, and nothing compiles against them.

## Updating one

There is no command for this. Replace the file with the upstream release,
update the version in the table above and the notice in
[`packaging/THIRD_PARTY_NOTICES.txt`](packaging/THIRD_PARTY_NOTICES.txt), which
every release archive carries, and run the full suite. The JSON header
parses every request that arrives from an MCP client, so treat a change to it
as a change to the code on the untrusted input path, not as a routine bump.

`gdextension_interface.h` is a compatibility contract, not just a header.
`addons/didi/didi.gdextension` declares `compatibility_minimum = "4.5"`, and the
live harness runs against real 4.5.1 and 4.7.2 editors. Both have to stay true.

## What Dependabot does and does not cover

Dependabot watches three things through
[`.github/dependabot.yml`](.github/dependabot.yml): the GitHub Actions the
workflows pin, weekly; the Python pin in `requirements-dev.txt`, monthly; and
the Ubuntu base image `tools/localci/Dockerfile` builds on, weekly. All three
carry a seven-day cooldown, so a brand-new release is not adopted on the day it
is published -- long enough for a compromised publish to be caught and yanked.
Cooldown does not apply to security updates, so a fix for a known vulnerability
still arrives at once.

Each ecosystem is grouped twice. Routine minor and patch bumps arrive as one
pull request rather than six, because a grouped update passes CI as a set or
fails as a set and that is the same review either way. Security updates are
grouped separately and without an `update-types` filter, so a disclosure
affecting several packages at once is still one pull request, and a fix is taken
whether upstream shipped it as a patch or as a major.

The base image is pinned by digest as well as tag, on the same reasoning as the
action SHAs. `ubuntu:24.04` is rebuilt in place every few weeks, so the tag
alone makes a lane run repeatable but not reproducible. Dependabot offers the
rebuilt image the way it offers a new action SHA, which is what stops the pin
becoming a way of staying unpatched. The release number itself is not
Dependabot's to choose -- that tag tracks what `ubuntu-latest` resolves to on
the GitHub runners and moves by hand when GitHub moves -- so semver bumps are
ignored and digest updates are not.

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
manifest for it to read. That used to be the end of this page, and the sentence
that followed admitted those three files were reviewed by hand or not at all.

[`tools/check_vendored_versions.py`](tools/check_vendored_versions.py) is that
review, automated. It asks two questions, and keeps them apart because they fail
for different reasons and deserve different consequences.

**Does this page still describe the files on disk?** Each vendored header states
its own version in its upstream banner. The tool reads that string out of the
file and compares it with the row above, so replacing a header and forgetting
the table is caught rather than inherited. This needs no network and is
deterministic, so it runs in the documentation suite on every pull request, via
`tests/test_vendored_versions.py`. A drift here is a defect in the branch and
fails the check.

**Is the file still current?** For the sources that publish a version somewhere
machine-readable, the tool fetches it and compares. `nlohmann/json` publishes
releases, so the latest release tag is the answer. `nothings/stb` tags nothing
and publishes no releases at all -- asking its releases API returns an empty
result that would read as "up to date" forever -- so the version is read out of
the upstream header's own banner instead. This needs the network, so it runs
weekly in [`supply-chain.yml`](.github/workflows/supply-chain.yml) and opens a
tracking issue rather than failing a check: upstream shipping a release is news
about the world, not a defect in whichever branch happens to be open.

`gdextension_interface.h` is deliberately not tracked, and the tool says so
rather than omitting it. It is a compatibility contract, not a version to chase.
`addons/didi/didi.gdextension` declares `compatibility_minimum = "4.5"` and the
live harness runs against real 4.5.1 and 4.7.2 editors; that is what has to stay
true. "Is there a newer Godot?" is the wrong question, and answering it weekly
would teach everyone to ignore the answer.

Run it by hand with:

```
python tools/check_vendored_versions.py            # both questions
python tools/check_vendored_versions.py --offline  # this page vs the files
```
