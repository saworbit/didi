#!/usr/bin/env bash
#
# One CI lane, run inside the container. Not meant to be called directly --
# `run.sh` mounts the source and invokes this.
#
# Each lane mirrors a job in ci.yml step for step, including the inline shell
# assertions, because those assertions are the part that has never been
# runnable locally: they live in the workflow file and nothing else executes
# them.

set -euo pipefail

LANE="${1:?lane required: gcc, clang or asan}"
WORK=/work
# `build` inside the checkout, exactly where ci.yml's `cmake -B build -S .`
# puts it. The tests resolve their scratch projects as
# `<cwd>/build/test-projects/`, so this is not a free choice.
BUILD="$WORK/build"

say() { printf '\n\033[1m== %s\033[0m\n' "$*"; }

# Copy the working tree in, rather than building out of the read-only mount.
#
# --delete so a file deleted on the host disappears here too. The two build
# directories are excluded, and rsync does not delete excluded paths on the
# receiver, so $BUILD survives and stays incremental.
#
# `.git` is copied, which was not the first guess. It was excluded as large and
# only read by the version stamp -- and the Python suite then failed with
# `git -C /work rev-parse HEAD` returning 128. A checkout is what CI has and
# what parts of this repository assume; the first sync pays for it once and
# rsync keeps the rest cheap.
say "syncing the working tree into $WORK"
rsync -a --delete \
    --exclude=/build \
    --exclude=/build-ninja \
    /src-ro/ "$WORK/"
cd "$WORK"

# Configure once per build volume, not once per run.
#
# CMakeLists.txt stamps the build id with `string(TIMESTAMP ...)`, so every
# configure rewrites generated/include/didi/common/version.hpp with a new
# value, and every translation unit that includes it rebuilds. That turns a
# "nothing changed" run into several minutes of recompiling, which is the
# difference between a tool you run before pushing and one you don't.
#
# CI configures fresh every time and should; it starts from an empty runner.
# Here the build volume persists, so reuse it. DIDI_LOCALCI_RECONFIGURE=1
# forces the fresh path when a CMake input actually changed.
if [ -f "$BUILD/CMakeCache.txt" ] && [ "${DIDI_LOCALCI_RECONFIGURE:-0}" != "1" ]; then
    say "reusing the existing configuration in $BUILD"
    echo "  (DIDI_LOCALCI_RECONFIGURE=1 to configure from scratch)"
    CONFIGURED=1
else
    CONFIGURED=0
fi

if [ "$CONFIGURED" = "0" ]; then
case "$LANE" in
gcc)
    # ci.yml: fast-build-and-test, ubuntu-latest (gcc)
    say "configure (gcc, Release)"
    cmake -S "$WORK" -B "$BUILD" -G Ninja -DCMAKE_BUILD_TYPE=Release
    ;;
clang)
    # No CI job matches this one. It is the closest honest stand-in for
    # macos-latest (clang): the same compiler family and the same standard
    # library implementation (libc++), which is what catches the great
    # majority of MSVC-to-Clang breaks. It is not macOS. See README.md.
    say "configure (clang + libc++, Release)"
    CC=clang CXX=clang++ cmake -S "$WORK" -B "$BUILD" -G Ninja \
        -DCMAKE_BUILD_TYPE=Release \
        -DCMAKE_CXX_FLAGS="-stdlib=libc++" \
        -DCMAKE_EXE_LINKER_FLAGS="-stdlib=libc++"
    ;;
asan)
    # ci.yml: sanitizers, "Sanitizers (ubuntu, asan+ubsan)"
    say "configure (gcc, RelWithDebInfo, sanitizers on)"
    cmake -S "$WORK" -B "$BUILD" -G Ninja \
        -DCMAKE_BUILD_TYPE=RelWithDebInfo \
        -DDIDI_ENABLE_SANITIZERS=ON
    ;;
*)
    echo "unknown lane: $LANE (expected gcc, clang or asan)" >&2
    exit 2
    ;;
esac
fi

say "build"
cmake --build "$BUILD" --parallel

if [ "$LANE" = "asan" ]; then
    # The sanitizer job runs the native tests and stops there. Same options as
    # ci.yml so a leak it reports here is the leak it would report there.
    say "native tests under asan+ubsan"
    ASAN_OPTIONS=detect_leaks=1:abort_on_error=0 \
    UBSAN_OPTIONS=print_stacktrace=1:halt_on_error=1 \
        "$BUILD/didi_tests"
    say "lane '$LANE' passed"
    exit 0
fi

# Lifted verbatim from ci.yml's "Verify the staged addon is complete and
# clean", with the Linux library name. This is one of the inline assertions
# that only ever ran on a runner.
say "staged addon is complete and clean"
expected="bin/libdidi_extension.so didi.gdextension didi.gdextension.uid didi_await.gd didi_await.gd.uid didi_brand.gd didi_brand.gd.uid didi_client_config.gd didi_client_config.gd.uid didi_console.gd didi_console.gd.uid didi_diagnostics.gd didi_diagnostics.gd.uid didi_import_watch.gd didi_import_watch.gd.uid didi_log.gd didi_log.gd.uid didi_mark.svg didi_mark_compact.svg didi_plugin.gd didi_plugin.gd.uid didi_session.gd didi_session.gd.uid didi_settings.gd didi_settings.gd.uid didi_signature.svg plugin.cfg "
actual=$(cd "$BUILD/addons/didi" && LC_ALL=C find . -type f | sed 's|^\./||' | LC_ALL=C sort | tr '\n' ' ')
if [ "$actual" != "$expected" ]; then
    echo "Staged addon is [$actual]" >&2
    echo "Expected          [$expected]" >&2
    exit 1
fi
echo "Staged addon verified"

say "native tests"
"$BUILD/didi_tests"

# ctest is the composition the release gates on, over unittest discovery rather
# than module-by-module. ci.yml has a comment about why that distinction cost a
# release; running it locally is the point of this lane.
say "ctest (the suites the way the release runs them)"
ctest --test-dir "$BUILD" --output-on-failure

say "lane '$LANE' passed"
