# Local CI lanes

Run the parts of `ci.yml` that a Windows developer machine cannot otherwise
run, in the container image the CI matrix runs on.

```bash
bash tools/localci/run.sh              # gcc, clang and asan
bash tools/localci/run.sh gcc          # one lane
bash tools/localci/run.sh --shell gcc  # a shell in that lane's container
bash tools/localci/run.sh --clean      # drop the per-lane build volumes
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

One-time setup, and the first step is the only thing in this whole directory
that cannot be automated — it is a toggle in System Settings on the Mac:

1. **Enable Remote Login** on the Mac: System Settings → General → Sharing →
   Remote Login on. Note the `user@host` it shows you.
2. **Install the toolchain** on the Mac:
   ```bash
   xcode-select --install          # clang
   brew install cmake ninja python # the rest
   ```
3. **Add a key** from this machine, so the lane can run unattended:
   ```bash
   ssh-keygen -t ed25519 -C didi-localci     # if you have no key yet
   ssh you@192.168.0.7 "mkdir -p ~/.ssh && cat >> ~/.ssh/authorized_keys" < ~/.ssh/id_ed25519.pub
   ```
4. Optionally stop the Mac sleeping: System Settings → Energy → Prevent
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

The same goes for `lane.sh`: it duplicates steps from `ci.yml` rather than
reading them, so the two can drift. The duplication is deliberate — `act` and
similar workflow runners bring their own fidelity gaps, and the steps here are
short — but a change to those steps in `ci.yml` needs the same change here.
