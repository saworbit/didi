#pragma once

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

    void registerAllDefaultPrompts();

private:
    PromptRegistry() = default;
    std::unordered_map<std::string, PromptDefinition> m_prompts;
};

} // namespace mcp
} // namespace didi
