#pragma once

#include <optional>
#include <utility>
#include <string>
#include <vector>
#include <unordered_map>
#include <memory>
#include "didi/mcp/mcp_protocol.hpp"

namespace didi {
namespace mcp {

class PromptRegistry {
public:
    static PromptRegistry& instance();

    void registerPrompt(PromptDefinition prompt);
    const PromptDefinition* getPrompt(const std::string& name) const;
    std::vector<PromptDefinition> listPrompts() const;
    Result<json> getPromptResult(const std::string& name, const json& args);

    // The first argument the caller passed that this prompt does not declare,
    // if any, with the declared names sorted for the message.
    //
    // #397 and #418 closed unknown arguments on tools/call and prompts/get
    // never got the same treatment, so a caller who misremembered an argument
    // name was handed a prompt rendered from defaults and no signal that what
    // they passed went nowhere (#511).
    std::optional<std::pair<std::string, std::vector<std::string>>> unknownArgument(
        const std::string& name, const json& args) const;

    // The first required argument the caller left out, if any.
    std::optional<std::string> missingRequiredArgument(const std::string& name,
                                                       const json& args) const;

    void registerAllDefaultPrompts();

private:
    PromptRegistry() = default;
    std::unordered_map<std::string, PromptDefinition> m_prompts;
};

} // namespace mcp
} // namespace didi
