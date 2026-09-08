# Issue 285: documentation-worker join investigation

The engine crash is **not fixed by this branch**. The recorded 4.7.2 stack
identifies a documentation-worker join failure. A controlled C++ probe shows
one possible mechanism; it is not an editor MRP. The implemented changes keep
retry decisions specific and preserve the evidence needed for further work.

## Research and evidence

- [Reparenting report](https://godotforums.org/d/30349-fatal-error-when-reparenting-node):
  a fatal bounds check after node removal produces `0xC000001D`. Different
  engine generation and callback context; not a matching cause.
- [Godot 112943](https://github.com/godotengine/godot/issues/112943): the same
  status accompanies a C# handle check. The non-Mono harness does not exercise
  that path.
- [Historical CPU issue](https://godotengine.org/article/dev-snapshot-godot-3-1-beta-7/):
  unsupported instructions are another cause, not established for this crash.
- [4.5.1 error macros](https://github.com/godotengine/godot/blob/4.5.1-stable/core/error/error_macros.h)
  use `__builtin_trap` outside MSVC. [GCC documents](https://gcc.gnu.org/onlinedocs/gcc-11.3.0/gcc/Other-Builtins.html)
  that this may intentionally execute an illegal instruction.
- The [captured stack and disassembly in issue 285](https://github.com/saworbit/didi/issues/285#issuecomment-5566192382)
  are stronger evidence: official 4.7.2, image size `0x0ae59000`, four engine
  frame RVAs `066744b6`, `017a6266`, `03928abd`, `04cf4b1a`. The first is a
  shared `ud2` trap reached through the documentation worker's native join.
  This investigation did not independently re-symbolize that minidump.

## What the source adds

[Thread::wait_to_finish](https://github.com/godotengine/godot/blob/4.7.2-stable/core/os/thread.cpp#L83)
checks both the engine's unassigned ID and its self-join condition before
calling `std::thread::join`. An engine-ID check is not a check that the native
handle has been published or that another thread has not consumed it.

[Thread::start](https://github.com/godotengine/godot/blob/4.7.2-stable/core/os/thread.cpp#L71)
sets the engine ID and then assigns a newly constructed `std::thread` to its
member. The new callback can run before that assignment completes.

[EditorHelp](https://github.com/godotengine/godot/blob/4.7.2-stable/editor/doc/editor_help.cpp#L3169)
starts a loader; the loader starts a worker; the worker joins the loader.
A schedule in which the loader and worker run before the initiating thread
publishes the loader's native handle can pass both engine-ID guards and then
try to join an empty native thread object.

There is also an ownership question: regeneration on the main thread waits
for the worker and then the loader, while the loader can create a worker
which also joins the loader. These observations warrant an engine-level test;
they do not establish which schedule occurred in the original crash. A single
dump cannot exclude another join that returned before the snapshot. The shared
trap address does not prove self-join. Absence of Didi frames does not prove
that main-thread activity could not trigger an engine race.

## Controlled publication probe

`tools/issue285/thread_publication_probe.cpp` isolates the first schedule. It
uses semaphores to force the gap after native thread construction but before
member assignment, without introducing a data race in the probe. The default
mode catches the native join error; the comparison mode publishes the handle
before releasing the loader callback.

From a Visual Studio developer command prompt at the repository root:

```bat
cl /nologo /std:c++20 /EHsc /O2 tools\issue285\thread_publication_probe.cpp /Fe:build\thread_publication_probe.exe /Fo:build\thread_publication_probe.obj
build\thread_publication_probe.exe
build\thread_publication_probe.exe --publish-first
```

Create `build` first if necessary. A C++20 GCC build can use
`g++ -std=c++20 -pthread tools/issue285/thread_publication_probe.cpp -o probe`.
Only the MSVC commands were tested here.

Observed with MSVC 14.44:

```text
engine guards passed=1, native join succeeded=0, native error=22
engine guards passed=1, native join succeeded=1, native error=0
```

Default exits 1 (the desired successful-join contract fails); `--publish-first`
exits 0. This tests standard-library handoff semantics, not the actual engine
binary, its timing probability, or a production patch. The next engine test
should pause after native thread construction and before handle assignment,
then exercise the documentation loader/worker handoff. A fix needs to give
joining a clear owner and synchronize publication without deadlocking resource
loading. Do not change Didi scene ownership based only on this model.

## Changes on this branch

1. Retry only the recorded 4.7.2 image/stack fingerprint. A different fatal
   check in Godot is no longer treated as issue 285. Unknown versions,
   including 4.5.1 without verified offsets, fail normally. This is an observed
   stack signature, not cryptographic binary identification or proof of cause.
2. Preserve both the report and binary minidump under attempt-specific names
   before the next harness attempt clears `godot_crash_*` files.
3. Upload retry evidence even when a later attempt succeeds. Previously the
   failure-only upload condition discarded evidence of tolerated crashes.
4. Describe the matched stack in the retry warning without asserting that
   Didi could not have triggered it.

Validation: classifier baseline 9 cases passed; added cases exposed 7 false
positives before the change; all 18 final cases passed. Artifact tests showed
the absent minidump before the fix and verified byte-for-byte survival after
retry cleanup, separate attempt paths, and missing-dump handling afterward.
PowerShell syntax and diff whitespace checks passed. No full live integration
run or custom Godot build is claimed by these checks.
