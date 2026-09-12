#pragma once

#include "didi/common/types.hpp"

#include <string>

namespace didi {
namespace mcp {

// The floor every error envelope stands on.
//
// `error.data` is the part a caller can branch on without parsing prose:
// `data.code` says what kind of failure this is, `data.tool` says who answered,
// `data.retryable` says whether trying again could help. It used to be filled
// at each call site, so on 35 well-formed, wrong calls only 14 carried a
// `data.code`: twelve had an empty `data`, four carried `retryable` and nothing
// else, and the confirmation gate carried no `data` at all (#486, #487).
//
// This is the one place. A call site that knows more still says more, and
// anything it already set is left exactly as it set it.

// The stable string for an HTTP-shaped status. A caller branches on this rather
// than on the number, because the number is a transport convention and this is
// not.
[[nodiscard]] std::string errorCodeForStatus(int status);

// Whether the same call could succeed later. A confirmation is retryable
// because the documented next step is to get a token and call again.
[[nodiscard]] bool retryableForStatus(int status);

// Fills `code`, `tool`, `canonical_tool` and `retryable` into `error["data"]`,
// leaving every key the caller already set alone. `error` is the envelope's
// error object, the one carrying `code` and `message`. A `data` that is not an
// object is moved to `data.details` rather than dropped.
void applyErrorDataFloor(json& error, const std::string& tool,
                         const std::string& canonical_tool);

} // namespace mcp
} // namespace didi
