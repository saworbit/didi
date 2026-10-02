#pragma once

#include <string>
#include <vector>

namespace didi::setup {

// `didi setup` and `didi doctor`. `arguments` are the ones after the
// subcommand's name. Each returns the process exit status: 0 when nothing
// failed, 1 when a step failed, 2 when the command line was refused.
//
// Neither is a tool. They run before any client is configured, which is the
// state a tool cannot reach (#382), and they print for a person or, with
// --json, for whatever ran them.
int runSetupCommand(const std::vector<std::string>& arguments);
int runDoctorCommand(const std::vector<std::string>& arguments);

}  // namespace didi::setup
