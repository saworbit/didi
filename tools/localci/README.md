# Local CI lanes

Run the parts of `ci.yml` that a Windows developer machine cannot otherwise
run, in the container image the CI matrix runs on.

```bash
bash tools/localci/run.sh              # every lane this machine is set up for
bash tools/localci/run.sh gcc          # one lane
bash tools/localci/run.sh --shell gcc  # a shell in that lane's container
bash tools/localci/run.sh --clean      # drop the per-lane build volumes
```

A bare run does `gcc`, `clang` and `asan`, and adds `macos` when `DIDI_MAC_HOST`
is set — the difference between "a Mac is configured here" and "there is no Mac
here". Set it once and the Mac is in every run:

```bash
export DIDI_MAC_HOST=shane@192.168.0.184   # in your shell profile
```

Needs Docker with Linux containers. Nothing else — no submodules, no fetched
dependencies; the build wants CMake, Python 3, threads and a C++20 compiler,
and the image supplies all four.

## Why

Five things `ci.yml` checks, and three of them had no local runner:

| | Before | Now |
| :--- | :--- | :--- |
| MSVC build and tests | local | local |
| Live Godot integration | local (Windows) | local (Windows) |
| **gcc build and tests** | push and wait | `run.sh gcc` |
| **asan + ubsan** | push and wait | `run.sh asan` |
| **The inline shell assertions in ci.yml** | push and wait | in the lanes |

That last row is the one worth naming. Several of `ci.yml`'s checks are shell
written into the workflow file — the staged-addon file list is the clearest
example — so nothing but a runner ever executed them. `lane.sh` carries the
addon check verbatim, with the Linux library name.

## The lanes

| Lane | Mirrors | What it runs |
| :--- | :--- | :--- |
| `gcc` | `ubuntu-latest (gcc)` | Release build, staged-addon check, `didi_tests`, `ctest` |
| `asan` | `Sanitizers (ubuntu, asan+ubsan)` | RelWithDebInfo + `DIDI_ENABLE_SANITIZERS`, `didi_tests` under the same ASAN/UBSAN options |
| `clang` | **nothing** — see below | Same as `gcc`, with clang and libc++ |

`gcc` and `asan` are mirrors: same OS image, same apt packages, same CMake
invocations, same test commands. A failure in them is a failure CI will
reproduce.

## macOS: a real Mac, or nothing

**A container cannot be a Mac.** Containers share the host kernel and macOS
needs Darwin. The `docker-osx` family run macOS inside QEMU inside a container,
which needs KVM, nested virtualisation, and violates the macOS EULA's
restriction to Apple-branded hardware. This repository does not go there.
GitHub Codespaces does not help either, for the same reason — Codespaces are
Linux. It rents the same Ubuntu coverage; it does not reach macOS.

So there are exactly two ways to get Darwin: CI, or a Mac you own.

### `macos` — a Mac on the network, over SSH

```bash
DIDI_MAC_HOST=you@192.168.0.7 bash tools/localci/run.sh macos
```

Real Apple Clang, real `std::filesystem`, the real `.dylib`, and a
case-insensitive filesystem. It sends your working tree (tracked files plus
untracked ones git is not ignoring — so no `build/`, no artifacts), then runs
the same steps as CI's `macos-latest (clang)` job: configure, build, the
staged-addon check with the `.dylib` name, `didi_tests`, `ctest`.

Three things it does that are worth knowing about:

- **It creates a git repository from the synced tree.** Parts of the suite shell
  out to `git rev-parse HEAD` and `git ls-files`, and sending the real history is
  not worth it — even packed this repository is 5.7 MiB, and it was 73 MiB of
  loose objects before a `git gc`. The tests assert the commit matches
  `^[0-9a-f]{7,40}$`, not that it is any particular commit, so the lane makes one
  and prints the real source commit beside it.
- **It shims `python`.** `tools/field-trial/cycle.py` spawns the interpreter by
  that name and macOS has only `python3` (#635). The symlink lives inside
  `~/didi-localci/shim/`, so nothing is installed on the Mac. Delete the shim
  when #635 lands.
- **It skips the Python suite on a pre-3.10 Mac**, and says so. A stock macOS has
  Python 3.9.6 and this tooling needs 3.10 — not only for the pinned `jsonschema`
  but for PEP 604 unions in ten-plus files (#634). That costs no coverage: those
  tests are platform-independent and run on Windows, both Linux lanes and CI.
  The C++ half is what a Mac is for, and it runs in full.

One-time setup, and the first step is the only thing in this whole directory
that cannot be automated — it is a toggle in System Settings on the Mac:

1. **Enable Remote Login** on the Mac: System Settings → General → Sharing →
   Remote Login on. Note the `user@host` it shows you.
2. **Install the toolchain** on the Mac:
   ```bash
   xcode-select --install          # clang
   brew install cmake ninja python # the rest
   ```
3. **Check the firewall is not blocking everything.** A Mac can answer ARP and
   still drop every packet, which looks from the network exactly like a machine
   that is switched off — `nmap` reports every port `filtered` rather than
   `closed`, and ICMP goes unanswered. Stealth mode alone does not do that to an
   enabled Sharing service; "Block all incoming connections" does.
   ```bash
   /usr/libexec/ApplicationFirewall/socketfilterfw --getglobalstate --getblockall --getstealthmode
   sudo /usr/libexec/ApplicationFirewall/socketfilterfw --setblockall off
   sudo lsof -iTCP:22 -sTCP:LISTEN      # sshd should appear
   ```
4. **Add a key** from the machine that will drive the lane, so it runs
   unattended. A dedicated key means the Mac can revoke exactly this access by
   deleting one line:
   ```bash
   ssh-keygen -t ed25519 -f ~/.ssh/didi_localci -N "" -C didi-localci
   ssh you@MAC "mkdir -p ~/.ssh && cat >> ~/.ssh/authorized_keys" < ~/.ssh/didi_localci.pub
   ```
   `macos.sh` picks up `~/.ssh/didi_localci` automatically; `DIDI_MAC_KEY`
   overrides it, and if neither exists it falls through to your `~/.ssh/config`.
   The key has no passphrase, which is the usual trade for an unattended LAN
   lane — it grants shell on that Mac to anyone who can read the file.
5. Optionally stop the Mac sleeping: System Settings → Energy → Prevent
   automatic sleeping when the display is off. A sleeping Mac fails the lane
   with a connection timeout.

The lane checks all of this before it does any work and names whichever step is
missing, so a bad setup costs one command, not a failed build.

### Not a self-hosted Actions runner

Registering the Mac as a self-hosted GitHub Actions runner would look tidier
and is the wrong call here: **saworbit/didi is a public repository**, and a
self-hosted runner on a public repository executes code from any fork's pull
request on the machine it runs on. GitHub documents this as a hazard rather
than a configuration. The SSH lane runs only what you ask it to, from your own
working tree, and a fork can reach neither.

### What the `clang` container lane is for

It is the fallback when the Mac is off, and it covers most of what the macOS
job catches in practice: the same compiler family and the same standard library
(libc++), so it finds missing transitive includes that MSVC supplies, two-phase
name lookup in templates, narrowing conversions, and the stricter
`std::filesystem` overload set.

It cannot stand in for Apple Clang's own version, Darwin `dyld` behaviour, the
`.dylib` name, or a case-insensitive filesystem. When the Mac is available, run
the `macos` lane and treat `clang` as a cheap pre-check.

## How the mounts work

Your working tree is bind-mounted **read-only** at `/src-ro`. Each lane
`rsync`s it into `/work`, a named Docker volume per lane, and builds there, in
a fully writable checkout — which is what a CI runner has.

- **Your working tree is never written to.** Lanes cannot leave a Linux `.so`
  next to the Windows build, and a failed lane leaves nothing behind.
- **The build is not on the bind mount**, which matters on Docker Desktop for
  Windows, where bind-mount I/O is slow enough to dominate a build.
- **`rsync --delete` keeps `/work` honest** — a file you delete on the host
  disappears there too. Only `build/` and `build-ninja/` are excluded, and rsync
  does not delete excluded paths on the receiver, so the build tree survives and
  stays incremental. `--clean` drops the volumes.
- **`.git` is copied**, which costs about 73 MiB on the first sync and almost
  nothing after.

### Why a copy rather than building in place

Building straight out of a read-only mount was the first design. It failed in
four different places, each of which looked like the last one, and every one of
them was found by running it rather than by reading the build:

1. `didi_extension` has `POST_BUILD` steps copying its library into
   `demo/addons/didi/bin/` and `tests/godot_smoke/addons/didi/bin/`
   (`CMakeLists.txt:300-303`). A read-only source fails at the **link step**.
2. 64 tests create scratch projects under `<repo>/build/test-projects/`,
   resolved from the working directory — so the build tree cannot live outside
   the source tree either.
3. `resources/subscribe` writes a board into `<repo>/.didi/blackboard/`, which
   took out one more test.
4. The Python suite runs `git -C <root> rev-parse HEAD`, so `.git` has to be
   there — it was excluded at first, on the assumption that only the CMake
   version stamp read it.

Each was patchable with one more `tmpfs` or exclusion, and the list was plainly
not finished. A writable copy of a real checkout is what CI has, and it has no
exceptions to keep up to date. **If you are tempted to make `/src-ro` writable
instead, the reason not to is that a container run would then drop Linux
artifacts into the Windows working tree.**

## Keeping it honest

The base image is pinned to `ubuntu:24.04` because `ubuntu-latest` resolved to
`ubuntu-24.04` (24.04.5) on the run that merged #626. When GitHub moves
`ubuntu-latest`, this tag has to move with it, or the lanes quietly stop being
mirrors. A recent run's "Operating System" group is where to check.

The tag carries a digest as well, for the reason the workflows pin action SHAs:
`ubuntu:24.04` is rebuilt in place every few weeks, so the tag alone makes a
lane repeatable but not reproducible. Dependabot watches that digest weekly and
offers the rebuilt image as a pull request -- see
[THIRD_PARTY.md](../../THIRD_PARTY.md) -- so the pin does not become a way of
staying unpatched. The release number is still a manual decision; only the
digest is automated.

The same goes for `lane.sh`: it duplicates steps from `ci.yml` rather than
reading them, so the two can drift. The duplication is deliberate — `act` and
similar workflow runners bring their own fidelity gaps, and the steps here are
short — but a change to those steps in `ci.yml` needs the same change here.
