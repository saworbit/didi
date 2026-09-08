// Controlled model of the Thread::start publication window in Godot 4.7.2.
// This is NOT a reproduction of the Godot editor crash. It tests whether the
// engine-ID guards can pass while the native handle is still unpublished.
// See docs/investigations/issue-285.md for the source mapping and limitations.
#include <iostream>
#include <semaphore>
#include <string_view>
#include <system_error>
#include <thread>

struct ThreadSlot {
    int engine_id = 0;
    std::thread native;
};

int main(int argc, char** argv) {
    const bool publish_first = argc == 2 && std::string_view(argv[1]) == "--publish-first";
    ThreadSlot loader;
    ThreadSlot worker;
    std::binary_semaphore start_loader{0};
    std::binary_semaphore join_attempted{0};
    bool guards_passed = false;
    bool joined = false;
    int native_error = 0;

    // Thread::start sets the engine ID, constructs the new std::thread, then
    // move-assigns it to its member. The callback can run before that assignment.
    loader.engine_id = 1;
    std::thread pending_loader([&] {
        if (publish_first) start_loader.acquire();
        worker.engine_id = 2;
        worker.native = std::thread([&] {
            // _finish_regen_script_doc_thread waits for loader_thread.
            guards_passed = loader.engine_id != 0 && loader.engine_id != worker.engine_id;
            if (guards_passed) {
                try {
                    loader.native.join();
                    joined = true;
                } catch (const std::system_error& error) {
                    native_error = error.code().value();
                }
            }
            join_attempted.release();
        });
    });

    if (publish_first) {
        loader.native = std::move(pending_loader);
        start_loader.release();
        join_attempted.acquire();
    } else {
        // Force the scheduler gap after native construction but before member
        // assignment. Waiting avoids a C++ data race in the probe itself.
        join_attempted.acquire();
        loader.native = std::move(pending_loader);
        loader.native.join();
    }
    worker.native.join();

    std::cout << "engine guards passed=" << guards_passed
              << ", native join succeeded=" << joined
              << ", native error=" << native_error << '\n';
    if (!guards_passed) return 2;
    // Default intentionally fails the desired successful-join contract.
    return joined ? 0 : 1;
}
