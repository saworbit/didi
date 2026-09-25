"""Bounded cleanup for a test-owned stdio child and all of its pipe handles."""

def stop(process):
    if process.poll() is None:
        process.kill()
    # kill() is asynchronous on Windows. Reap the child and close/drain every
    # pipe before a fixture directory is removed or the next test starts.
    process.communicate(timeout=10)
