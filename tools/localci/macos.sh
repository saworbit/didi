#!/usr/bin/env bash
#
# The macOS lane. Not a container -- a real Mac on the network, driven over SSH.
#
#   DIDI_MAC_HOST=shane@192.168.0.7 bash tools/localci/run.sh macos
#
# This is the only lane that gets actual Darwin coverage: Apple Clang, the real
# `std::filesystem`, the `.dylib` name, and a case-insensitive filesystem. The
# `clang` container lane stands in for some of that and cannot stand in for any
# of it properly. See README.md.
#
# Deliberately *not* a self-hosted GitHub Actions runner. saworbit/didi is a
# public repository, and a self-hosted runner on a public repository will
# execute code from any fork's pull request on the machine it runs on. An SSH
# lane runs only what you ask it to run, from your own working tree.

set -euo pipefail

HOST="${DIDI_MAC_HOST:-}"
REMOTE_DIR="${DIDI_MAC_DIR:-didi-localci}"
SSH_OPTS=(-o BatchMode=yes -o ConnectTimeout=8)

# A dedicated key rather than whatever the agent happens to hold, so the lane
# is unattended and the Mac can revoke exactly this access by deleting one line
# from authorized_keys. DIDI_MAC_KEY overrides; ~/.ssh/config is used if
# neither is set, which is the right answer for anyone who already manages
# their hosts there.
KEY="${DIDI_MAC_KEY:-$HOME/.ssh/didi_localci}"
if [ -f "$KEY" ]; then
    SSH_OPTS+=(-i "$KEY" -o IdentitiesOnly=yes)
fi

say() { printf '\n\033[1m== %s\033[0m\n' "$*"; }
die() { printf '\033[31m%s\033[0m\n' "$*" >&2; exit 1; }

if [ -z "$HOST" ]; then
    die "Set DIDI_MAC_HOST first, as user@host:

    DIDI_MAC_HOST=you@192.168.0.7 bash tools/localci/run.sh macos

If SSH is not set up yet, tools/localci/README.md has the one-time steps.
They have to be done on the Mac -- Remote Login is a System Settings toggle
and nothing here can turn it on for you."
fi

say "reaching $HOST"
if ! ssh "${SSH_OPTS[@]}" "$HOST" true 2>/dev/null; then
    die "Cannot open a non-interactive SSH session to $HOST.

Most likely one of:
  * Remote Login is off      -> on the Mac: System Settings > General >
                                Sharing > Remote Login
  * no key is installed      -> ssh-copy-id, or see README.md
  * the Mac is asleep        -> System Settings > Energy, or wake it

Check by hand with:  ssh $HOST"
fi

kernel=$(ssh "${SSH_OPTS[@]}" "$HOST" uname -s)
[ "$kernel" = "Darwin" ] || die "$HOST reports '$kernel', not Darwin. This lane is for a Mac."
printf 'macOS %s on %s\n' \
    "$(ssh "${SSH_OPTS[@]}" "$HOST" sw_vers -productVersion)" \
    "$(ssh "${SSH_OPTS[@]}" "$HOST" uname -m)"

say "toolchain"
# Homebrew is not on a non-interactive shell's PATH by default, because
# .zprofile is only read by a login shell. Every remote command below sources
# it, and this is where a missing one is named rather than failing later as
# "cmake: command not found" from inside a build.
BREW_PATH='export PATH="$HOME/bin:/opt/homebrew/bin:/usr/local/bin:$PATH";'
missing=$(ssh "${SSH_OPTS[@]}" "$HOST" "$BREW_PATH"' for t in cmake ninja python3 clang++; do command -v $t >/dev/null 2>&1 || echo $t; done')
if [ -n "$missing" ]; then
    die "Missing on $HOST: $(echo $missing)

  clang++            xcode-select --install
  cmake ninja        brew install cmake ninja
  python3            brew install python"
fi
ssh "${SSH_OPTS[@]}" "$HOST" "$BREW_PATH"' printf "  %s\n" "$(cmake --version | head -1)" "$(ninja --version)" "$(clang++ --version | head -1)"'

say "sending the working tree"
# Tracked files plus untracked ones git is not ignoring: that is exactly the
# source a build needs, and it leaves out build/, build-ninja/ and every
# generated artifact without having to list them. No rsync on Git Bash, so it
# is tar over the pipe.
ssh "${SSH_OPTS[@]}" "$HOST" "rm -rf '$REMOTE_DIR/src' && mkdir -p '$REMOTE_DIR/src'"
git ls-files -z --cached --others --exclude-standard \
    | tar --null -T - -cf - \
    | ssh "${SSH_OPTS[@]}" "$HOST" "tar -xf - -C '$REMOTE_DIR/src'"
echo "  $(git ls-files --cached --others --exclude-standard | wc -l) files -> $HOST:$REMOTE_DIR/src"

# Parts of the suite shell out to git -- the field-trial tests run
# `git -C <root> rev-parse HEAD` and the docs validator runs `git ls-files` --
# so an extracted tree with no repository fails nine tests with exit 128.
#
# The real history is not worth sending. This repository carries 10,218 loose
# objects (73 MiB); packing them into a bundle to send the 4.3 MiB version took
# six minutes of pure I/O on the Windows filesystem, per run.
#
# What those tests need is a repository, not this repository: the field-trial
# assertion is `assertRegex(commit, r"^[0-9a-f]{7,40}$")`. So the lane makes one
# from the tree it just sent, and prints the real commit beside the synthetic
# one so nobody reads a trial record as pointing at upstream history.
say "git repository for the tests that need one"
source_commit=$(git rev-parse HEAD 2>/dev/null || echo "unknown")
ssh "${SSH_OPTS[@]}" "$HOST" "cd '$REMOTE_DIR/src' && \
    git init -q && git add -A && \
    git -c user.email=localci@invalid -c user.name='didi localci' \
        commit -qm 'working tree synced by tools/localci, from $source_commit'"
echo "  synthetic repo; the tree came from $source_commit"

say "python dependency"
# Two pip eras, and a Mac can be either. --break-system-packages arrived in pip
# 23 for PEP 668 environments; macOS ships a system python3 whose pip is older
# than that and rejects the flag outright with "no such option". --user works
# there and fails on a PEP 668 interpreter, so try that first and fall back.
#
# Either can still fail for a reason neither flag fixes: requirements-dev.txt
# pins jsonschema 4.26.0, which dropped Python 3.9, and macOS 27 ships 3.9.6 as
# its system python3. Without Homebrew there is no newer interpreter, and
# installing one needs sudo. That is a gap in the Python suite only -- the
# reason to run a Mac at all is Apple Clang, the real std::filesystem and the
# .dylib, none of which care. So the lane degrades loudly instead of refusing.
# The gate is the interpreter version, not whether a package imports.
#
# jsonschema was only the first wall: requirements-dev.txt pins 4.26.0, which
# dropped 3.9. Installing 4.25.1 gets past it and straight into the second,
# which is that the Python tree uses PEP 604 unions (`int | None`) in more than
# ten files without `from __future__ import annotations`, so 3.9 raises
# TypeError at class-body evaluation. 3.10 is a hard floor for this tooling
# whatever pip is persuaded to install.
#
# The C++ half is the entire reason to run a Mac -- Apple Clang, the real
# std::filesystem, the .dylib. The Python suite is platform-independent and
# already runs on Windows, both Linux lanes and CI, so skipping it here costs
# no coverage that exists anywhere else.
PYTHON_TESTS=0
version=$(ssh "${SSH_OPTS[@]}" "$HOST" "$BREW_PATH"' python3 -V 2>&1')
if ssh "${SSH_OPTS[@]}" "$HOST" "$BREW_PATH"' python3 -c "import sys; raise SystemExit(0 if sys.version_info >= (3, 10) else 1)"'; then
    if ssh "${SSH_OPTS[@]}" "$HOST" "$BREW_PATH"' python3 -c "import jsonschema" 2>/dev/null' \
        || ssh "${SSH_OPTS[@]}" "$HOST" "$BREW_PATH cd '$REMOTE_DIR/src' && { python3 -m pip install --quiet --user -r requirements-dev.txt || python3 -m pip install --quiet --break-system-packages -r requirements-dev.txt; }" 2>/dev/null; then
        PYTHON_TESTS=1
        echo "  $version, jsonschema available -- running the full suite"
    else
        printf '\033[33m  %s is new enough, but requirements-dev.txt would not install.\033[0m\n' "$version"
    fi
else
    printf '\033[33m  SKIPPING the Python suite: %s, and this tooling needs 3.10+.\n' "$version"
    printf '  Not just the pinned jsonschema -- PEP 604 unions in 10+ files raise\n'
    printf '  TypeError on 3.9. See #634. The C++ half below is what the Mac is\n'
    printf '  for, and the Python suite runs on every other lane and in CI.\033[0m\n'
fi

# Everything past here mirrors ci.yml's macos-latest (clang) job. The heredoc
# is quoted so it is the Mac's shell that expands these, not this one.
say "build and test on $HOST"
ssh "${SSH_OPTS[@]}" "$HOST" "DIDI_REMOTE_DIR='$REMOTE_DIR' DIDI_PYTHON_TESTS='$PYTHON_TESTS' bash -s" <<'REMOTE'
set -euo pipefail
export PATH="$HOME/bin:/opt/homebrew/bin:/usr/local/bin:$PATH"
cd "$HOME/$DIDI_REMOTE_DIR/src"
BUILD="$HOME/$DIDI_REMOTE_DIR/build"

# tools/field-trial/cycle.py spawns the interpreter as `python`, and macOS has
# no such command -- only python3 (#635). A shim inside this lane's own
# workspace fixes the run without installing anything on the Mac: `rm -rf
# ~/didi-localci` still removes every trace of the lane. Remove this once #635
# lands, and the lane will then be testing the fixed behaviour rather than
# hiding it.
SHIM="$HOME/$DIDI_REMOTE_DIR/shim"
if ! command -v python >/dev/null 2>&1; then
    mkdir -p "$SHIM"
    ln -sf "$(command -v python3)" "$SHIM/python"
    export PATH="$SHIM:$PATH"
fi

cmake -S . -B "$BUILD" -G Ninja -DCMAKE_BUILD_TYPE=Release
cmake --build "$BUILD" --parallel

# ci.yml "Verify the staged addon is complete and clean", macOS library name.
expected="bin/libdidi_extension.dylib didi.gdextension didi.gdextension.uid didi_await.gd didi_await.gd.uid didi_brand.gd didi_brand.gd.uid didi_client_config.gd didi_client_config.gd.uid didi_console.gd didi_console.gd.uid didi_diagnostics.gd didi_diagnostics.gd.uid didi_import_watch.gd didi_import_watch.gd.uid didi_log.gd didi_log.gd.uid didi_mark.svg didi_mark_compact.svg didi_plugin.gd didi_plugin.gd.uid didi_session.gd didi_session.gd.uid didi_settings.gd didi_settings.gd.uid didi_signature.svg plugin.cfg "
actual=$(cd "$BUILD/addons/didi" && LC_ALL=C find . -type f | sed 's|^\./||' | LC_ALL=C sort | tr '\n' ' ')
if [ "$actual" != "$expected" ]; then
    echo "Staged addon is [$actual]" >&2
    echo "Expected          [$expected]" >&2
    exit 1
fi
echo "Staged addon verified"

"$BUILD/didi_tests"

if [ "${DIDI_PYTHON_TESTS:-1}" = "1" ]; then
    ctest --test-dir "$BUILD" --output-on-failure
else
    # The native half is the whole reason this lane exists; excluding the
    # Python suite by name keeps ctest's composition otherwise intact and makes
    # the exclusion visible in its own output rather than silent.
    ctest --test-dir "$BUILD" --output-on-failure -E didi_python_tests
    echo
    echo "NOTE: didi_python_tests was excluded -- see the dependency warning above."
fi
REMOTE

say "lane 'macos' passed"
