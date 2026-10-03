#include "didi/runtime/undo_capture.hpp"

namespace didi::runtime {

namespace {
thread_local UndoStepCapture* t_innermost = nullptr;

void take(json& holder, UndoStepCapture* capture, json& into) {
    if (!holder.is_object()) return;
    const auto found = holder.find("undo_steps");
    if (found == holder.end()) return;
    if (capture && found->is_array()) {
        for (auto& step : *found) into.push_back(std::move(step));
    }
    holder.erase(found);
}
}  // namespace

UndoStepCapture::UndoStepCapture() : m_outer(t_innermost) { t_innermost = this; }

UndoStepCapture::~UndoStepCapture() {
    // A nested capture hands what it saw to the one around it, so a tool that
    // calls another tool still journals everything its call committed.
    if (m_outer) {
        for (auto& step : m_steps) m_outer->m_steps.push_back(std::move(step));
    }
    t_innermost = m_outer;
}

void collectUndoSteps(Result<json>& response) {
    UndoStepCapture* capture = t_innermost;
    json discard = json::array();
    json& into = capture ? capture->m_steps : discard;
    if (response.isOk()) {
        take(response.value(), capture, into);
    } else {
        take(response.error().data, capture, into);
    }
}

}  // namespace didi::runtime
