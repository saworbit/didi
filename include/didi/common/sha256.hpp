#pragma once

// SHA-256 (FIPS 180-4) of a byte string, as 64 lowercase hex digits. The
// digest depends on the bytes alone, never on the platform or the process, so
// a value recorded on one machine can be compared on another: checkpoints
// record one per file, and so does a scenario's proof record (Q9).

#include <string>
#include <string_view>

namespace didi {

std::string sha256Hex(std::string_view input);

}  // namespace didi
