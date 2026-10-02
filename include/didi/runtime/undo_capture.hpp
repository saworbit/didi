#pragma once

// Where the change journal's undo references cross from the bridge to the
// server (Q15 in docs/BUILD_QUEUE.md).
//
// The editor hook reads the editor's histories around every command and adds
// what the command committed to its answer as `undo_steps`, inside error.data
// when the command failed. Every live request goes through
// RuntimeRouteLease::sendRequest, which hands those steps to the capture open
// on the calling thread and takes them off the answer, so no tool's answer
// changes shape and no caller has to know the field exists. A capture is
// opened around one tool call; a request made with none open, such as a
// resource read, still has the field removed.

#include "didi/common/json.hpp"
#include "didi/common/types.hpp"

namespace didi::runtime {

class UndoStepCapture {
public:
    UndoStepCapture();
    ~UndoStepCapture();
    UndoStepCapture(const UndoStepCapture&) = delete;
    UndoStepCapture& operator=(const UndoStepCapture&) = delete;

    // Every step collected while this capture was the innermost one on its
    // thread, in the order the answers arrived.
    const json& steps() const { return m_steps; }

private:
    friend void collectUndoSteps(Result<json>& response);
    json m_steps = json::array();
    UndoStepCapture* m_outer = nullptr;
};

// Moves `undo_steps` from a bridge answer, or from its error's data, into the
// innermost capture on this thread, and removes it either way.
void collectUndoSteps(Result<json>& response);

}  // namespace didi::runtime
