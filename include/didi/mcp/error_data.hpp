#pragma once

#include "didi/common/types.hpp"

#include <map>
#include <string>
#include <vector>

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

// What fixes a refusal (Q6). A refusal names the argument or the call that
// fixes it, in one of these fields of `error.data`:
//
//   retry_with      the arguments to send again, as values
//   field           the argument at fault, by name; argument, missing and
//                   did_you_mean say the same thing and count too
//   next_call       {"tool", "arguments", "reason"}: a call to make first, one
//                   that changes the state the refusal was about
//   restart_with    a server flag the refusal needs, which no call can change
//   retry_after_ms  a transient state: the same call, later
//   no_remedy       why nothing the caller sends can fix it, so it is not
//                   retried; parameter names an argument as field does
//
// A call site that knows the remedy says it, and the floor leaves it alone.
// Where a site said none, the floor fills the remedy its code carries from
// the table in refusal_remedies.cpp. A 5xx fault other than 503 and 504 is
// the server's to fix, not the caller's, and carries none.
[[nodiscard]] bool hasRemedy(const json& data);

// The remedy fields for a refusal whose site gave none, from its code and the
// tool it came from. Empty when the code is one nothing the caller does can
// fix, or one the table does not know.
[[nodiscard]] json remedyForRefusal(const std::string& code, int status,
                                    const std::string& message,
                                    const std::string& canonical_tool, const json& data);

// The census the manifest publishes, so a test can compare it with every code
// the source emits: the codes the table remedies, and the ones it leaves
// without a remedy, each with the reason.
[[nodiscard]] std::vector<std::string> remediedRefusalCodes();
[[nodiscard]] const std::map<std::string, std::string>& refusalsWithoutRemedy();

} // namespace mcp
} // namespace didi
