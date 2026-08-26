#pragma once

#include "didi/common/types.hpp"
#include "didi/common/json.hpp"

namespace didi {
namespace godot {

class VisualTestLab {
public:
    static VisualTestLab& instance();

    json createLab(const json& params);

private:
    VisualTestLab() = default;
};

} // namespace godot
} // namespace didi
