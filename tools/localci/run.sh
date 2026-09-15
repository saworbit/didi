#!/usr/bin/env bash
#
# Run a CI lane locally, in the container image the CI matrix runs on.
#
#   bash tools/localci/run.sh              # gcc, clang and asan, in that order
#   bash tools/localci/run.sh gcc          # one lane
#   bash tools/localci/run.sh clang asan
#   bash tools/localci/run.sh --shell gcc  # drop into the lane's container
#   bash tools/localci/run.sh --clean      # drop the build volumes and exit
#
#   DIDI_MAC_HOST=you@host bash tools/localci/run.sh macos   # a real Mac, over SSH
#
# Why this exists: three of the five things ci.yml checks have never been
# runnable on a Windows developer machine -- the non-MSVC compilers, the
# sanitizers, and the inline shell assertions that live in the workflow file.
# Pushing was the only way to run them, which makes a red check a round trip
# rather than a local failure.
#
# macOS is the fourth, and it is not a container: no container can be a Mac.
# The `macos` lane drives a real one over SSH and is opt-in; the `clang` lane
# is the fallback when that Mac is off. README.md has both stories.

set -euo pipefail

IMAGE=didi-localci
REPO_ROOT_UNIX=$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)
REPO_ROOT="$REPO_ROOT_UNIX"

# Git Bash rewrites anything that looks like a Unix path in an argument, so
# `-v /d/didi:/src` arrives as `-v /d/didi:C:/Program Files/Git/src`. Turning
# the mangling off for the docker calls is the documented escape hatch, and
# `pwd -W` gives the Windows spelling docker actually wants.
#
# REPO_ROOT is the spelling docker needs; REPO_ROOT_UNIX is the one this shell
# needs to source a sibling script. On Linux and macOS they are the same.
if [ -n "${MSYSTEM:-}" ]; then
    export MSYS_NO_PATHCONV=1
    REPO_ROOT=$(cd "$REPO_ROOT_UNIX" && pwd -W)
fi

# Machine-local settings, so the Mac does not have to be remembered as an
# environment variable on every invocation. Gitignored: it names one developer's
# host, which is not a fact about the project.
#
#   echo 'DIDI_MAC_HOST=you@192.168.0.184' > tools/localci/local.env
#
# An already-exported value wins, so a one-off override still works.
LOCAL_ENV="$REPO_ROOT_UNIX/tools/localci/local.env"
if [ -f "$LOCAL_ENV" ]; then
    while IFS='=' read -r name value; do
        case "$name" in ''|\#*) continue ;; esac
        name=${name%% *}
        [ -n "${!name:-}" ] || export "$name=${value}"
    done < "$LOCAL_ENV"
fi

SHELL_MODE=0
LANES=()
for argument in "$@"; do
    case "$argument" in
    --shell) SHELL_MODE=1 ;;
    --clean)
        for lane in gcc clang asan; do
            docker volume rm -f "didi-localci-work-$lane" >/dev/null 2>&1 || true
        done
        echo "Removed the per-lane build volumes."
        exit 0
        ;;
    -h | --help)
        sed -n '2,20p' "${BASH_SOURCE[0]}" | sed 's/^# \{0,1\}//'
        exit 0
        ;;
    gcc | clang | asan | macos) LANES+=("$argument") ;;
    *)
        echo "unknown argument: $argument (lanes: gcc, clang, asan, macos)" >&2
        exit 2
        ;;
    esac
done
# With no lane named, run everything this machine is actually set up for. The
# three container lanes always qualify; macos joins them when DIDI_MAC_HOST is
# set, because that is the difference between "a Mac is configured" and "there
# is no Mac here". Naming a lane explicitly still overrides all of this.
#
# The first version left macos out of the default entirely, which meant a bare
# run silently skipped the only lane that covers Darwin -- on a machine where
# the Mac was configured and working.
if [ ${#LANES[@]} -eq 0 ]; then
    LANES=(gcc clang asan)
    [ -n "${DIDI_MAC_HOST:-}" ] && LANES+=(macos)
fi

# The macos lane is SSH, not Docker, so the image build is skipped when that is
# all that was asked for.
needs_docker=0
for lane in "${LANES[@]}"; do
    [ "$lane" != "macos" ] && needs_docker=1
done
if [ "$needs_docker" = "1" ]; then
    echo "Building the image (cached after the first run) ..."
    docker build -q -t "$IMAGE" -f "$REPO_ROOT/tools/localci/Dockerfile" "$REPO_ROOT" >/dev/null
fi

failed=()
for lane in "${LANES[@]}"; do
    if [ "$lane" = "macos" ]; then
        printf '\n\033[1m######## lane: macos (%s) ########\033[0m\n' "${DIDI_MAC_HOST:-DIDI_MAC_HOST unset}"
        bash "$REPO_ROOT_UNIX/tools/localci/macos.sh" || failed+=("macos")
        continue
    fi
    # The host tree goes in read-only and is never the thing built; lane.sh
    # rsyncs it into /work, a named volume, and builds there. The volume is per
    # lane, so switching between them does not throw away a build tree and
    # gcc's objects never end up in clang's, and it keeps the build off the
    # Windows bind mount where I/O would dominate the run.
    #
    # Building directly out of the read-only mount was the first design and it
    # does not work: the POST_BUILD copies into demo/ and tests/godot_smoke/,
    # 64 tests that create scratch projects under <repo>/build/test-projects/,
    # and resources/subscribe writing a board into <repo>/.didi/blackboard/ all
    # need a writable checkout. Each was patchable with another tmpfs and the
    # list was obviously not finished.
    volume="didi-localci-work-$lane"
    common=(--rm
        -v "$REPO_ROOT:/src-ro:ro"
        -v "$volume:/work"
        -w /work)

    if [ "$SHELL_MODE" = "1" ]; then
        echo "Shell in the '$lane' lane. Host tree read-only at /src-ro; workspace at /work (run lane.sh to sync)."
        docker run -it "${common[@]}" "$IMAGE" bash
        exit 0
    fi

    printf '\n\033[1m######## lane: %s ########\033[0m\n' "$lane"
    if docker run "${common[@]}" "$IMAGE" /usr/local/bin/lane.sh "$lane"; then
        :
    else
        failed+=("$lane")
    fi
done

echo
if [ ${#failed[@]} -eq 0 ]; then
    echo "All lanes passed: ${LANES[*]}"
else
    echo "FAILED: ${failed[*]}" >&2
    exit 1
fi
