"""Bounded cleanup for a test-owned stdio child and all of its pipe handles."""

def stop(process):
    if process.poll() is None:
        process.kill()
    if all(pipe is None or pipe.closed for pipe in (process.stdin, process.stdout, process.stderr)):
        process.wait(timeout=10)
        return
    # kill() is asynchronous on Windows. Reap the child and close/drain every
    # pipe before a fixture directory is removed or the next test starts.
    process.communicate(timeout=10)
