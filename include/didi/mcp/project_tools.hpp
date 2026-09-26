#pragma once

#include "didi/mcp/mcp_protocol.hpp"
#include "didi/common/ipc_channel.hpp"
#include <memory>
#include <optional>

namespace didi::mcp {

// project_set_setting's rules for the files a value names, which need no
// engine and so run before the route is chosen: live and offline, and for a
// dry run, refuse the same values (#989). Every res:// string inside an array
// must name a file in the project, and internationalization/locale/translations
// takes only the kinds of file Godot registers a translation from. A value that
// is a single string is checked by each route itself, after the live route's
// type check, which is the refusal that call has always met first.
std::optional<Error> checkSettingValuePaths(const json& args);

CallToolResult handleProjectListAutoloads(const json& args, std::shared_ptr<ipc::IIpcClient> ipc);
CallToolResult handleProjectSetAutoload(const json& args, std::shared_ptr<ipc::IIpcClient> ipc);
CallToolResult handleProjectRemoveAutoload(const json& args, std::shared_ptr<ipc::IIpcClient> ipc);
CallToolResult handleProjectListInputActions(const json& args, std::shared_ptr<ipc::IIpcClient> ipc);
CallToolResult handleProjectSetInputAction(const json& args, std::shared_ptr<ipc::IIpcClient> ipc);
CallToolResult handleProjectRemoveInputAction(const json& args, std::shared_ptr<ipc::IIpcClient> ipc);
CallToolResult handleProjectGetSetting(const json& args, std::shared_ptr<ipc::IIpcClient> ipc);
CallToolResult handleProjectSetSetting(const json& args, std::shared_ptr<ipc::IIpcClient> ipc);
CallToolResult handleProjectSearchText(const json& args, std::shared_ptr<ipc::IIpcClient> ipc);
CallToolResult handleProjectSearchSymbols(const json& args, std::shared_ptr<ipc::IIpcClient> ipc);

} // namespace didi::mcp
