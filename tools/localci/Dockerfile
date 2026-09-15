# The image the CI matrix actually runs on.
#
# `ubuntu-latest` resolved to `ubuntu-24.04` (24.04.5) on the run that merged
# #626, so this is a match rather than an approximation. When GitHub moves
# `ubuntu-latest` on, this tag has to move with it or the lanes stop meaning
# what they claim: check a recent run's "Operating System" group and bump it.
FROM ubuntu:24.04

ENV DEBIAN_FRONTEND=noninteractive

# `build-essential ninja-build` is exactly what ci.yml's "Install Build Tools
# (Linux)" step installs. `cmake git python3` are preinstalled on the GitHub
# runner image, so they do not appear in that step and do have to be asked for
# here -- the difference is the image, not the build.
#
# clang, libc++ and libc++abi are not in the CI Linux job at all. They are here
# for the `clang` lane, which stands in for the macOS job; see README.md for
# exactly how far that stand-in goes.
RUN apt-get update -qq \
 && apt-get install -y --no-install-recommends \
      build-essential \
      ca-certificates \
      clang \
      cmake \
      git \
      libc++-dev \
      libc++abi-dev \
      ninja-build \
      python3 \
      python3-pip \
      # `python`, not just `python3`. The GitHub runner image provides both;
      # a bare ubuntu:24.04 provides only python3, and the field-trial suite
      # refuses with "python is not on PATH. A cycle cannot run without the
      # client that hosts the agent."
      python-is-python3 \
      rsync \
 && rm -rf /var/lib/apt/lists/*

# Ubuntu 24.04 marks the system interpreter externally managed (PEP 668). A
# venv would have to be re-activated by every lane and by anything a test
# shells out to, so the pinned dependency goes into the system interpreter of a
# throwaway image instead.
COPY requirements-dev.txt /tmp/requirements-dev.txt
RUN pip3 install --no-cache-dir --break-system-packages -r /tmp/requirements-dev.txt

# The lane script is baked in rather than run from the read-only /src mount.
# Bash reads a script lazily, by byte offset, so editing lane.sh while a
# container is executing it from the bind mount makes the running shell jump
# into the middle of a different line -- which looks like a syntax error in a
# file that is perfectly valid by the time you go and look at it. run.sh
# rebuilds the image (cached) on every invocation, so an edit still takes
# effect on the next run.
COPY tools/localci/lane.sh /usr/local/bin/lane.sh
RUN chmod +x /usr/local/bin/lane.sh

# The host's working tree is bind-mounted read-only at /src-ro, and each lane
# rsyncs it into /work, a named volume, before doing anything. The build then
# happens in a fully writable checkout, which is what CI has.
#
# That indirection is not tidiness, it is the second design. Building straight
# out of a read-only mount failed in three separate places -- the POST_BUILD
# copies into demo/ and tests/godot_smoke/ (CMakeLists.txt:300-303), the 64
# tests that create scratch projects under <repo>/build/test-projects/, and
# resources/subscribe, which writes a board into <repo>/.didi/blackboard/. Each
# was patchable with another tmpfs, and the list was clearly not finished. A
# writable copy has no exceptions to keep up to date, and the host tree still
# cannot be touched.
WORKDIR /work
