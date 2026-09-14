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
BREW_PATH='export PATH="/opt/homebrew/bin:/usr/local/bin:$PATH";'
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

say "python dependency"
ssh "${SSH_OPTS[@]}" "$HOST" "$BREW_PATH"' python3 -c "import jsonschema" 2>/dev/null' \
    || ssh "${SSH_OPTS[@]}" "$HOST" "$BREW_PATH cd '$REMOTE_DIR/src' && python3 -m pip install --quiet --break-system-packages -r requirements-dev.txt"

# Everything past here mirrors ci.yml's macos-latest (clang) job. The heredoc
# is quoted so it is the Mac's shell that expands these, not this one.
say "build and test on $HOST"
ssh "${SSH_OPTS[@]}" "$HOST" "DIDI_REMOTE_DIR='$REMOTE_DIR' bash -s" <<'REMOTE'
set -euo pipefail
export PATH="/opt/homebrew/bin:/usr/local/bin:$PATH"
cd "$HOME/$DIDI_REMOTE_DIR/src"
BUILD="$HOME/$DIDI_REMOTE_DIR/build"

cmake -S . -B "$BUILD" -G Ninja -DCMAKE_BUILD_TYPE=Release
cmake --build "$BUILD" --parallel

# ci.yml "Verify the staged addon is complete and clean", macOS library name.
expected="bin/libdidi_extension.dylib didi.gdextension didi_await.gd didi_brand.gd didi_client_config.gd didi_console.gd didi_diagnostics.gd didi_log.gd didi_mark.svg didi_mark_compact.svg didi_plugin.gd didi_session.gd didi_settings.gd didi_signature.svg plugin.cfg "
actual=$(cd "$BUILD/addons/didi" && LC_ALL=C find . -type f | sed 's|^\./||' | LC_ALL=C sort | tr '\n' ' ')
if [ "$actual" != "$expected" ]; then
    echo "Staged addon is [$actual]" >&2
    echo "Expected          [$expected]" >&2
    exit 1
fi
echo "Staged addon verified"

"$BUILD/didi_tests"
ctest --test-dir "$BUILD" --output-on-failure
REMOTE

say "lane 'macos' passed"
