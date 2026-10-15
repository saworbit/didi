#!/usr/bin/env bash
# What a lane needs from the image, checked without running one. CI builds
# the image and runs this on a pull request that changes the Dockerfile or
# what it copies, so a broken install is a red check rather than the next
# person's failed lane (#1419).
set -euo pipefail

cmake --version
ninja --version
g++ --version
clang++-20 --version
git --version
rsync --version

# libc++ 20 is in the image for floating-point std::from_chars, which 18
# lacks and main needs (#1026). Built the way the clang lane builds.
cat > /tmp/from_chars.cpp <<'CPP'
#include <charconv>
int main() {
    double value = 0;
    const char text[] = "1.5";
    const auto parsed = std::from_chars(text, text + 3, value);
    return parsed.ec == std::errc() && value == 1.5 ? 0 : 1;
}
CPP
clang++-20 -std=c++20 -stdlib=libc++ /tmp/from_chars.cpp -o /tmp/from_chars
/tmp/from_chars

# python, not only python3, and every pin in requirements-dev.txt installed at
# exactly its pinned version.
python --version
python - <<'PY'
import importlib.metadata
import pathlib
import sys

missing = []
for line in pathlib.Path("/tmp/requirements-dev.txt").read_text().splitlines():
    line = line.split("#", 1)[0].strip()
    if not line:
        continue
    name, _, pinned = line.partition("==")
    try:
        installed = importlib.metadata.version(name.strip())
    except importlib.metadata.PackageNotFoundError:
        missing.append(f"{name.strip()} is not installed")
        continue
    if pinned and installed != pinned.strip():
        missing.append(f"{name.strip()} is {installed}, pinned {pinned.strip()}")
    else:
        print(f"{name.strip()} {installed}")
if missing:
    sys.exit("requirements-dev.txt: " + "; ".join(missing))
PY

# The lane entry point the image bakes in.
test -x /usr/local/bin/lane.sh
bash -n /usr/local/bin/lane.sh
echo "Local CI image smoke check passed."
