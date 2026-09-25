#include "didi/mcp/mcp_protocol.hpp"
#include "didi/tools/phase7_live_forward.hpp"
#include "didi/common/ipc_channel.hpp"
#include "didi/common/project_path.hpp"
#include "didi/common/atomic_write.hpp"
#include "didi/offline/deep_domain_support.hpp"
#include "didi/offline/process_runner.hpp"
#include "didi/offline/project_file_lock.hpp"
#include "didi/offline/test_runner.hpp"
#include "didi/common/engine_version.hpp"
#include "didi/runtime/session_client.hpp"
#include <algorithm>
#include <atomic>
#include <cstdlib>
#include <chrono>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <mutex>
#include <optional>
#include <cmath>
#include <regex>
#include <set>
#include <sstream>

namespace didi::mcp {
namespace {

constexpr size_t kMaxProcessOutput = 1024 * 1024;
constexpr size_t kMaxPresetFile = 1024 * 1024;

std::string lower(std::string value) {
    std::transform(value.begin(), value.end(), value.begin(), [](unsigned char c) {
        return static_cast<char>(std::tolower(c));
    });
    return value;
}

Result<std::filesystem::path> projectRoot() {
    std::error_code error;
    const auto root = std::filesystem::weakly_canonical(std::filesystem::current_path(), error);
    if (error || !std::filesystem::is_regular_file(root / "project.godot", error) || error) {
        return Error::invalidArgument("Current directory is not a Godot project root");
    }
    return root;
}

Result<std::filesystem::path> resolveOutputPath(const std::filesystem::path& root,
                                                const std::string& requested) {
    if (requested.empty()) return Error::invalidArgument("output_path is required");
    // The rule every other writer applies through resolveProjectFileForWrite.
    // This path is handed to Godot on its command line, where a newline or a tab
    // splits it and a NUL ends it at the filesystem boundary, and a directory was
    // created for it first (#939).
    for (const unsigned char character : requested) {
        if (character < 0x20 || character == 0x7F) {
            return Error::invalidArgument("output_path cannot contain control characters");
        }
    }
    if (const auto problem = paths::foreignSchemeProblem(requested)) {
        return Error::invalidArgument("output_path " + *problem);
    }
    std::string relative_text = requested;
    if (strings::startsWith(relative_text, "res://")) relative_text.erase(0, 6);
    std::filesystem::path relative;
    try {
        relative = paths::projectPathFromUtf8(relative_text);
    } catch (const std::filesystem::filesystem_error&) {
        return Error::invalidArgument("output_path must be valid UTF-8");
    }
    if (relative.empty() || relative.is_absolute() || relative.has_root_name()) {
        return Error::invalidArgument("output_path must be relative to the project root");
    }
    // Resolve, then compare against the project root, the same way every other
    // writer in the server does since #534.
    std::error_code error;
    const auto target = std::filesystem::weakly_canonical(root / relative, error);
    if (error || !paths::isWithinProject(root, target)) {
        return Error::invalidArgument("output_path resolves outside the project root");
    }
    return target;
}

std::string asResPath(const std::filesystem::path& root, const std::filesystem::path& path) {
    std::error_code error;
    const auto relative = std::filesystem::relative(path, root, error);
    return error ? std::string{} : "res://" + paths::projectPathToUtf8(relative);
}

Result<std::string> readBounded(const std::filesystem::path& path, size_t limit) {
    std::error_code error;
    const auto size = std::filesystem::file_size(path, error);
    if (error) return Error::notFound("File is unavailable: " + paths::projectPathToUtf8(path));
    if (size > limit) return Error::invalidArgument("File exceeds the 1 MiB Phase 5 limit");
    std::ifstream input(path, std::ios::binary);
    if (!input) return Error::notFound("File cannot be opened: " + paths::projectPathToUtf8(path));
    std::ostringstream buffer;
    buffer << input.rdbuf();
    return buffer.str();
}

json diagnosticsJson(const std::vector<offline::DomainDiagnostic>& diagnostics) {
    json output = json::array();
    for (const auto& diagnostic : diagnostics) output.push_back(diagnostic.toJson());
    return output;
}

// The same list, with every path that lives in the project spelled res://.
//
// MSBuild names the file in host terms, so diagnostics[].path carried an
// absolute path beside a project_file the same object had already converted --
// the one field naming the file the caller has to go and fix was the one field
// no path-taking tool would accept, and it carried the home directory of
// whoever ran the server into the host's transcript (#703). Paths outside the
// project are SDK and NuGet targets; those have no res:// name and are left as
// they came.
json msBuildDiagnosticsJson(const std::filesystem::path& root,
                            const std::vector<offline::DomainDiagnostic>& diagnostics) {
    json output = json::array();
    for (const auto& diagnostic : diagnostics) {
        json value = diagnostic.toJson();
        if (!diagnostic.path.empty()) {
            std::error_code error;
            std::filesystem::path candidate;
            try {
                candidate = paths::projectPathFromUtf8(diagnostic.path);
            } catch (const std::filesystem::filesystem_error&) {
                output.push_back(std::move(value));
                continue;
            }
            if (candidate.is_relative()) candidate = root / candidate;
            const auto resolved = std::filesystem::weakly_canonical(candidate, error);
            if (!error && paths::isWithinProject(root, resolved)) {
                value["path"] = asResPath(root, resolved);
            }
        }
        output.push_back(std::move(value));
    }
    return output;
}

Result<int> timeoutSeconds(const json& args, int fallback, int maximum) {
    if (!args.contains("timeout_seconds")) return fallback;
    if (!args["timeout_seconds"].is_number_integer()) {
        return Error::invalidArgument("timeout_seconds must be an integer");
    }
    const int value = args["timeout_seconds"].get<int>();
    if (value < 1 || value > maximum) {
        return Error::invalidArgument("timeout_seconds must be from 1 to " + std::to_string(maximum));
    }
    return value;
}

// What DOTNET_BIN resolved to, and what was discarded on the way.
//
// The same shape resolveGodotExecutableDetailed has carried since #656, for
// the same reason: a variable that is set and cannot be used was dropped in
// silence, so the answer named a toolchain the user never chose, or no
// toolchain at all (#704).
struct DotnetResolution {
    std::string executable{"dotnet"};   // what will actually be run
    std::string configured;             // what DOTNET_BIN held, empty when unset
    std::string configured_rejected;    // why it was not used, empty when it was
};

DotnetResolution resolveDotnet() {
    DotnetResolution resolution;
    const char* configured = std::getenv("DOTNET_BIN");
    if (!configured || !*configured) return resolution;
    resolution.configured = configured;
    std::filesystem::path path;
    try {
        path = paths::projectPathFromUtf8(configured);
    } catch (const std::filesystem::filesystem_error&) {
        resolution.configured_rejected = "that path could not be read as UTF-8";
        return resolution;
    }
    std::error_code error;
    if (!std::filesystem::exists(path, error) || error) {
        resolution.configured_rejected = "nothing exists at that path";
        return resolution;
    }
    if (std::filesystem::is_directory(path, error)) {
        resolution.configured_rejected =
            "that path is a directory, and DOTNET_BIN names an executable";
        return resolution;
    }
    resolution.executable = configured;
    return resolution;
}

// Whether the thing we are about to run is a .NET SDK, and which one.
//
// csharp_check_build published nothing about which dotnet ran, so "the SDK is
// not installed" and "your C# does not compile" arrived as the same answer:
// exit 2 on Windows, exit 127 and isError: false on POSIX, zero diagnostics in
// both (#704). One `dotnet --version` separates them, and the SDK version is
// worth having anyway because the target framework a Godot project needs
// depends on it.
struct DotnetProbe {
    bool available{false};
    std::string version;
    std::string unavailable_reason;
};

DotnetProbe probeDotnet(const std::string& executable,
                        const std::filesystem::path& working_directory) {
    DotnetProbe probe;
    offline::ProcessRequest request;
    request.executable = executable;
    request.arguments = {"--version"};
    request.working_directory = working_directory;
    request.timeout = std::chrono::seconds(30);
    request.max_output_bytes = 64 * 1024;
    auto run = offline::runProcess(request);
    if (run.isErr()) {
        probe.unavailable_reason = run.error().message;
        return probe;
    }
    if (run.value().timed_out) {
        probe.unavailable_reason = "'--version' did not answer within 30 seconds";
        return probe;
    }
    const auto first_line = strings::trim(strings::split(run.value().output, '\n').empty()
                                              ? std::string{}
                                              : strings::split(run.value().output, '\n').front());
    if (run.value().exit_code != 0) {
        probe.unavailable_reason = "'--version' exited " +
                                   std::to_string(run.value().exit_code) +
                                   (first_line.empty() ? std::string{}
                                                       : " and printed '" + first_line + "'");
        return probe;
    }
    // A DOTNET_BIN that names a real file which is not dotnet runs, exits 0 and
    // prints something else. Reporting that as an SDK version would put the
    // misconfiguration back where #704 found it.
    static const std::regex sdk_version(R"(^[0-9]+\.[0-9]+\.[0-9]+.*$)");
    if (!std::regex_match(first_line, sdk_version)) {
        probe.unavailable_reason = "'--version' printed '" + first_line +
                                   "', which is not a .NET SDK version";
        return probe;
    }
    probe.version = first_line;
    probe.available = true;
    return probe;
}

Result<std::filesystem::path> selectCSharpProject(const std::filesystem::path& root,
                                                  const json& args) {
    if (args.contains("project_file")) {
        if (!args["project_file"].is_string()) return Error::invalidArgument("project_file must be a string");
        const std::string requested = args["project_file"].get<std::string>();
        auto resolved = paths::resolveProjectFile(requested);
        if (resolved.isErr()) return resolved.error();
        const auto extension = lower(resolved.value().extension().string());
        if (extension != ".sln" && extension != ".csproj") {
            return Error::invalidArgument("project_file must name a .sln or .csproj file");
        }
        return resolved.value();
    }
    std::vector<std::filesystem::path> solutions;
    std::vector<std::filesystem::path> projects;
    std::error_code error;
    for (std::filesystem::directory_iterator it(root, error), end; !error && it != end; it.increment(error)) {
        if (!it->is_regular_file(error) || error) continue;
        const auto extension = lower(it->path().extension().string());
        if (extension == ".sln") solutions.push_back(it->path());
        else if (extension == ".csproj") projects.push_back(it->path());
    }
    const auto& candidates = !solutions.empty() ? solutions : projects;
    if (candidates.empty()) return Error::notFound("No .sln or .csproj exists at the project root");
    if (candidates.size() != 1) return Error::invalidArgument("Multiple C# project files exist; specify project_file");
    return candidates.front();
}

class TemporaryScript {
public:
    static Result<TemporaryScript> create(const std::string& stem, const std::string& source) {
        static std::atomic<uint64_t> sequence{0};
        std::error_code error;
        auto directory = std::filesystem::temp_directory_path(error);
        if (error) return Error::internal("Temporary directory is unavailable");
        auto path = directory / ("didi-" + stem + "-" + std::to_string(
            std::chrono::steady_clock::now().time_since_epoch().count()) + "-" +
            std::to_string(sequence.fetch_add(1)) + ".gd");
        std::ofstream output(path, std::ios::binary | std::ios::trunc);
        if (!output) return Error::internal("Failed to create temporary Godot helper script");
        output << source;
        if (!output) return Error::internal("Failed to write temporary Godot helper script");
        return TemporaryScript(std::move(path));
    }

    TemporaryScript(TemporaryScript&& other) noexcept : path_(std::move(other.path_)) { other.path_.clear(); }
    TemporaryScript(const TemporaryScript&) = delete;
    ~TemporaryScript() {
        if (!path_.empty()) {
            std::error_code error;
            std::filesystem::remove(path_, error);
        }
    }
    const std::filesystem::path& path() const { return path_; }

private:
    explicit TemporaryScript(std::filesystem::path path) : path_(std::move(path)) {}
    std::filesystem::path path_;
};

Result<offline::ProcessResult> runGodot(const std::filesystem::path& root,
                                        std::vector<std::string> arguments,
                                        int timeout_seconds) {
    // The guard serialises isolated launches itself.
    offline::ScopedOfflineHelperEnvironment offline_environment;
    if (!offline_environment.ready()) {
        return Error::internal("Unable to isolate the offline Godot helper environment");
    }
    offline::ProcessRequest request;
    request.executable = offline::resolveGodotExecutable();
    request.arguments = std::move(arguments);
    request.working_directory = root;
    request.timeout = std::chrono::seconds(timeout_seconds);
    request.max_output_bytes = kMaxProcessOutput;
    return offline::runProcess(request);
}

const char* shaderHelperSource() {
    return R"GD(extends SceneTree

func _initialize() -> void:
	var args := OS.get_cmdline_user_args()
	if args.size() != 1:
		push_error("DIDI_PHASE5_ERROR: expected one shader path")
		quit(2)
		return
	var resource := load(args[0])
	if resource == null or not resource is Shader:
		push_error("DIDI_PHASE5_ERROR: resource is not a Shader: " + args[0])
		quit(3)
		return
	resource.get_shader_uniform_list()
	print("DIDI_PHASE5_SHADER_OK")
	quit(0)
)GD";
}

const char* meshLibraryHelperSource() {
    return R"GD(extends SceneTree

func fail(message: String, code: int) -> void:
	push_error("DIDI_PHASE5_ERROR: " + message)
	quit(code)

func _initialize() -> void:
	var args := OS.get_cmdline_user_args()
	if args.size() != 3:
		fail("expected source, output, and collision flag", 2)
		return
	var packed := load(args[0]) as PackedScene
	if packed == null:
		fail("source is not a loadable PackedScene", 3)
		return
	var instance := packed.instantiate()
	root.add_child(instance)
	var library := MeshLibrary.new()
	var collisions := args[2] == "true"
	for item_id in range(instance.get_child_count()):
		var item := instance.get_child(item_id)
		var mesh_instance := item as MeshInstance3D
		if mesh_instance == null:
			var mesh_candidates := item.find_children("*", "MeshInstance3D", true, false)
			if not mesh_candidates.is_empty():
				mesh_instance = mesh_candidates[0] as MeshInstance3D
		if mesh_instance == null or mesh_instance.mesh == null:
			fail("item %d (%s) has no MeshInstance3D with a mesh" % [item_id, item.name], 4)
			return
		library.create_item(item_id)
		library.set_item_name(item_id, str(item.name))
		library.set_item_mesh(item_id, mesh_instance.mesh)
		var relative_transform: Transform3D = item.global_transform.affine_inverse() * mesh_instance.global_transform
		library.set_item_mesh_transform(item_id, relative_transform)
		if collisions:
			var shape := mesh_instance.mesh.create_trimesh_shape()
			if shape != null:
				library.set_item_shapes(item_id, [shape, relative_transform])
		var nav: NavigationRegion3D = null
		var nav_candidates := item.find_children("*", "NavigationRegion3D", true, false)
		if not nav_candidates.is_empty():
			nav = nav_candidates[0] as NavigationRegion3D
		if nav != null and nav.navigation_mesh != null:
			library.set_item_navigation_mesh(item_id, nav.navigation_mesh)
			library.set_item_navigation_mesh_transform(item_id, item.global_transform.affine_inverse() * nav.global_transform)
	var save_error := ResourceSaver.save(library, args[1])
	if save_error != OK:
		fail("ResourceSaver failed with Error %d" % save_error, 5)
		return
	var saved_library := load(args[1]) as MeshLibrary
	if saved_library == null:
		fail("saved resource could not be reloaded as MeshLibrary", 6)
		return
	print("DIDI_PHASE5_RESULT:" + JSON.stringify({"item_count": saved_library.get_item_list().size()}))
	quit(0)
)GD";
}

Result<json> parseMarker(const std::string& output, const std::string& marker) {
    const size_t position = output.rfind(marker);
    if (position == std::string::npos) return Error::internal("Godot helper did not return a completion marker");
    const size_t start = position + marker.size();
    const size_t end = output.find_first_of("\r\n", start);
    try {
        return json::parse(output.substr(start, end == std::string::npos ? std::string::npos : end - start));
    } catch (const json::exception&) {
        return Error::internal("Godot helper returned malformed completion metadata");
    }
}

} // namespace

CallToolResult handleCSharpCheckBuild(const json& args, std::shared_ptr<ipc::IIpcClient> ipc) {
    (void)ipc;
    if (!args.is_object()) return CallToolResult::error("C# build arguments must be an object");
    auto root = projectRoot();
    if (root.isErr()) return CallToolResult::fromError(root.error());
    auto project = selectCSharpProject(root.value(), args);
    if (project.isErr()) return CallToolResult::fromError(project.error());
    const std::string configuration = args.value("configuration", "Debug");
    if (configuration != "Debug" && configuration != "Release") {
        return CallToolResult::error("configuration must be Debug or Release");
    }
    auto timeout = timeoutSeconds(args, 60, 300);
    if (timeout.isErr()) return CallToolResult::fromError(timeout.error());

    const auto dotnet = resolveDotnet();
    const auto probe = probeDotnet(dotnet.executable, root.value());
    // A build that never ran is not C# with no errors.
    //
    // The same sentence #677 wrote for script_check_syntax, owed to the one
    // member of the family it was never asked of. A missing SDK exited 2 on
    // Windows and 127 on POSIX with isError: false, which is exactly what a
    // failed build looks like, and nothing in the payload separated them
    // (#704). The launch failure also answered with a bare string (#705).
    if (!probe.available) {
        json data = {{"code", "toolchain_unavailable"},
                     {"tool", "csharp_check_build"},
                     {"dotnet_executable", dotnet.executable},
                     {"retryable", false}};
        if (!dotnet.configured.empty() && !dotnet.configured_rejected.empty()) {
            data["dotnet_executable_configured"] = dotnet.configured;
            data["dotnet_executable_configured_rejected"] = dotnet.configured_rejected;
        }
        return CallToolResult::errorJson(
            503,
            "No .NET SDK answered, so whether this project's C# compiles is unknown: '" +
                dotnet.executable + "' " + probe.unavailable_reason +
                ". Install the .NET SDK, or set DOTNET_BIN to a dotnet executable.",
            std::move(data));
    }

    offline::ProcessRequest request;
    request.executable = dotnet.executable;
    request.arguments = {"build", paths::projectPathToUtf8(project.value()), "--nologo",
                         "--verbosity:minimal", "--configuration", configuration};
    request.working_directory = root.value();
    request.timeout = std::chrono::seconds(timeout.value());
    request.max_output_bytes = kMaxProcessOutput;
    auto run = offline::runProcess(request);
    if (run.isErr()) {
        return CallToolResult::errorJson(
            500, "Failed to run dotnet build: " + run.error().message,
            {{"code", "internal_error"}, {"tool", "csharp_check_build"},
             {"dotnet_executable", dotnet.executable}, {"retryable", false}});
    }
    if (run.value().timed_out) {
        return CallToolResult::errorJson(
            504, "dotnet build timed out before completion",
            {{"code", "timeout"}, {"tool", "csharp_check_build"},
             {"timeout_seconds", timeout.value()}, {"retryable", true}});
    }
    auto diagnostics = offline::parseMsBuildDiagnostics(run.value().output);
    const bool has_errors = run.value().exit_code != 0 || std::any_of(
        diagnostics.begin(), diagnostics.end(), [](const auto& value) { return value.severity == "error"; });
    // How many projects the compiler actually produced an assembly for. A
    // solution whose project paths do not resolve, and one MSBuild skips
    // because it carries no configuration mapping, both exit 0 having compiled
    // nothing, and calling that success was the tool saying the C# compiles
    // about code it never looked at (#706).
    const int projects_built = offline::parseMsBuildProjectOutputCount(run.value().output);
    const bool built_nothing = run.value().exit_code == 0 && projects_built == 0;
    json result = {
        {"success", !has_errors && !built_nothing}, {"has_errors", has_errors},
        {"exit_code", run.value().exit_code},
        {"project_file", asResPath(root.value(), project.value())}, {"configuration", configuration},
        {"projects_built", projects_built},
        {"diagnostics", msBuildDiagnosticsJson(root.value(), diagnostics)},
        {"diagnostics_count", diagnostics.size()},
        {"dotnet_executable", dotnet.executable}, {"dotnet_version", probe.version},
        {"duration_seconds", run.value().duration_seconds}, {"output_truncated", run.value().output_truncated},
        {"raw_output", run.value().output}, {"execution_mode", "offline_fallback"}
    };
    if (built_nothing) {
        result["nothing_built_reason"] =
            "dotnet exited 0 without building any project, so whether this project's C# "
            "compiles is unknown. A solution builds nothing when its project paths do not "
            "resolve, or when it carries no configuration mapping for them. Point "
            "project_file at the .csproj, or fix the solution.";
    }
    if (!dotnet.configured.empty() && !dotnet.configured_rejected.empty()) {
        result["dotnet_executable_configured"] = dotnet.configured;
        result["dotnet_executable_configured_rejected"] = dotnet.configured_rejected;
    }
    return CallToolResult::successJson(result);
}

CallToolResult handleShaderListUniforms(const ResolvedToolBinding& binding, const json& args,
                                        std::shared_ptr<ipc::IIpcClient> ipc) {
    return sendPhase7LiveRequest(binding, args, ipc);
}

CallToolResult handleShaderSetUniform(const ResolvedToolBinding& binding, const json& args,
                                      std::shared_ptr<ipc::IIpcClient> ipc) {
    return sendPhase7LiveRequest(binding, args, ipc);
}

CallToolResult handleShaderGetVisualGraph(const ResolvedToolBinding& binding, const json& args,
                                          std::shared_ptr<ipc::IIpcClient> ipc) {
    return sendPhase7LiveRequest(binding, args, ipc);
}

CallToolResult handleShaderCheckCompile(const json& args, std::shared_ptr<ipc::IIpcClient> ipc) {
    if (!args.is_object() || !args.contains("shader_path") || !args["shader_path"].is_string()) {
        return CallToolResult::error("shader_path is required and must be a string");
    }
    auto root = projectRoot();
    if (root.isErr()) return CallToolResult::fromError(root.error());
    const std::string requested = args["shader_path"].get<std::string>();
    auto shader = paths::resolveProjectFile(requested);
    if (shader.isErr()) return CallToolResult::fromError(shader.error());
    if (lower(shader.value().extension().string()) != ".gdshader") {
        return CallToolResult::errorJson(400, "shader_path must name a .gdshader file");
    }
    auto timeout = timeoutSeconds(args, 30, 300);
    if (timeout.isErr()) return CallToolResult::fromError(timeout.error());
    auto helper = TemporaryScript::create("shader-check", shaderHelperSource());
    if (helper.isErr()) return CallToolResult::fromError(helper.error());
    auto run = runGodot(root.value(), offline::isolatedGodotArguments(
        {"--path", paths::projectPathToUtf8(root.value()), "--script",
         paths::projectPathToUtf8(helper.value().path()), "--", requested}), timeout.value());
    if (run.isErr()) return CallToolResult::error("Failed to run Godot shader compiler: " + run.error().message);
    if (run.value().timed_out) return CallToolResult::error("Shader compilation timed out before completion");
    auto diagnostics = offline::parseGodotDiagnostics(run.value().output);
    for (auto& diagnostic : diagnostics) {
        if (diagnostic.path.empty()) diagnostic.path = requested;
    }
    const bool marker = run.value().output.find("DIDI_PHASE5_SHADER_OK") != std::string::npos;
    const bool has_errors = run.value().exit_code != 0 || !marker || !diagnostics.empty();
    if (has_errors && diagnostics.empty()) {
        diagnostics.push_back({"error", "GODOT_SHADER", "Godot did not confirm shader compilation",
                               requested, 0, 0});
    }
    json result = {
        {"success", !has_errors}, {"has_errors", has_errors}, {"exit_code", run.value().exit_code},
        {"shader_path", requested}, {"diagnostics", diagnosticsJson(diagnostics)},
        {"diagnostics_count", diagnostics.size()}, {"duration_seconds", run.value().duration_seconds},
        {"output_truncated", run.value().output_truncated}, {"raw_output", run.value().output},
        {"execution_mode", "offline_fallback"}
    };
    // Which engine answered, as a field rather than only inside raw_output.
    // Reading the selected session takes no route and changes no selection,
    // which is what an offline-only tool is allowed to do.
    const auto sessions = std::dynamic_pointer_cast<runtime::IRuntimeSessionClient>(ipc);
    const auto attached = sessions ? sessions->observableSession()
                                   : std::optional<runtime::SessionDescriptor>{};
    const auto configured = offline::resolveGodotExecutableDetailed();
    versions::annotateConfiguredEngine(result, configured.configured, configured.configured_rejected);
    versions::annotateCheckEngine(
        result, offline::engineVersionFromOutput(run.value().output), configured.executable,
        attached.has_value() ? attached->engine_version : std::string());
    return CallToolResult::successJson(result);
}

// Godot's console output with the terminal control sequences taken out.
//
// The export failure used to hand four kilobytes of this over concatenated into
// a message, carriage returns and colour escapes and progress bars included,
// with the one actionable line sixty lines down. A client that renders an error
// message into a terminal would execute the escapes (#651).
std::string withoutTerminalEscapes(const std::string& text) {
    std::string out;
    out.reserve(text.size());
    for (size_t index = 0; index < text.size(); ++index) {
        if (text[index] == '\r') continue;
        if (text[index] != '\x1b') {
            out += text[index];
            continue;
        }
        // CSI and the two-character sequences alike: skip to the byte that ends
        // the sequence rather than trying to understand it.
        ++index;
        if (index < text.size() && text[index] == '[') {
            ++index;
            while (index < text.size() && !(text[index] >= '@' && text[index] <= '~')) ++index;
        }
    }
    return out;
}

// The refusal both export tools give for a presets file that is there and
// cannot be parsed. One sentence in one place, because project_export used to
// answer "Export preset not found" for it and send the reader off to add a
// preset the file already declares. The wording lives beside the reader, so
// the cause it found is the cause both tools report.
CallToolResult malformedPresetsRefusal(const offline::ExportPresetsFile& file) {
    auto data = offline::malformedPresetsData(file);
    data["code"] = "unprocessable";
    return CallToolResult::errorJson(422, offline::malformedPresetsMessage(file),
                                     std::move(data));
}

CallToolResult handleProjectListExportPresets(const json& args, std::shared_ptr<ipc::IIpcClient> ipc) {
    (void)ipc;
    if (!args.is_object() || !args.empty()) return CallToolResult::error("Export preset list arguments must be an empty object");
    auto root = projectRoot();
    if (root.isErr()) return CallToolResult::fromError(root.error());
    const auto path = root.value() / "export_presets.cfg";
    // Godot writes this file the first time a preset is added, so a project
    // that has never configured an export simply has none. That is a normal
    // state and the honest answer is an empty list, not a file error naming a
    // path the user never created (#403). A file that is there and cannot be
    // read is still an error.
    std::error_code exists_error;
    if (!std::filesystem::exists(path, exists_error)) {
        return CallToolResult::successJson({{"presets", json::array()}, {"preset_count", 0},
                                            {"execution_mode", "offline_fallback"},
                                            {"sensitive_options_omitted", true},
                                            {"presets_file_exists", false}});
    }
    auto contents = readBounded(path, kMaxPresetFile);
    if (contents.isErr()) return CallToolResult::fromError(contents.error());
    const auto file = offline::readExportPresets(contents.value());
    // A file that is there and declares no presets is the same fact about the
    // project as having no file at all: this project has no export presets. It
    // used to be an error while the absent case was a success, which are two
    // answers to one question (#651). A file that cannot be parsed is the
    // separate state, and has its own code now.
    if (file.malformed) return malformedPresetsRefusal(file);
    // Every section is still listed, because it is in the file and the Export
    // dialog will not show it; `detected` says which ones Godot will read, and
    // `not_detected` why not (#921).
    const auto detected = std::count_if(file.presets.begin(), file.presets.end(),
                                        [](const json& item) { return item.value("detected", true); });
    return CallToolResult::successJson({{"presets", file.presets},
                                       {"preset_count", file.presets.size()},
                                       {"detected_count", detected},
                                       {"execution_mode", "offline_fallback"},
                                       {"sensitive_options_omitted", true},
                                       {"presets_file_exists", true}});
}

// Adds one export preset, which is what makes project_export reachable on a
// project nobody has exported by hand (#779).
//
// This process writes the file whether or not an editor is attached. An open
// editor reads the file once, when it starts, and writes its own list back
// over it the next time any preset changes, so a preset written underneath it
// is lost with no error anywhere. With an editor attached, the bridge is asked
// to make it read the file again. The measurements behind every choice here
// are in the project_add_export_preset amendment in docs/SURFACE_AMENDMENTS.md.
CallToolResult handleProjectAddExportPreset(const json& args, std::shared_ptr<ipc::IIpcClient> ipc) {
    auto root = projectRoot();
    if (root.isErr()) return CallToolResult::fromError(root.error());
    // Held from the plan's read of the file to the read-back, so a second
    // server's preset cannot land in between and be replaced by this one's
    // copy of the file, and the read-back cannot meet its replace (#929).
    auto lock = offline::lockProjectFile(root.value(), "export_presets.cfg");
    if (lock.isErr()) return CallToolResult::fromError(lock.error());
    auto plan = offline::planExportPresetForProject(args);
    if (plan.isErr()) return CallToolResult::fromError(plan.error());
    const auto path = root.value() / "export_presets.cfg";
    auto written = files::writeFileAtomically(path, plan.value().contents);
    if (written.isErr()) {
        return CallToolResult::fromError(written.error(), "Failed to write export_presets.cfg: ");
    }

    // Read back through the reader project_list_export_presets and
    // project_export use, so the answer is what those two will now say.
    auto contents = readBounded(path, kMaxPresetFile);
    if (contents.isErr()) return CallToolResult::fromError(contents.error());
    const auto file = offline::readExportPresets(contents.value());
    const std::string name = args["name"].get<std::string>();
    const auto record = std::find_if(file.presets.begin(), file.presets.end(),
                                     [&](const json& item) { return item.value("name", "") == name; });
    if (file.malformed || record == file.presets.end() || !record->value("detected", false)) {
        return CallToolResult::errorJson(
            500, "export_presets.cfg was written and does not read back with this preset detected",
            {{"code", "internal_error"}, {"preset", name}, {"written_to", "res://export_presets.cfg"}});
    }

    json payload = {
        {"status", "created"},
        {"preset", *record},
        {"written_to", "res://export_presets.cfg"},
        {"file_created", plan.value().file_created},
        {"section_written", plan.value().section_text},
        {"preset_count", file.presets.size()},
        {"next_step",
         "project_export with preset \"" + name + "\" and mode \"pack\" writes a .pck and needs "
         "no export templates. A release or debug build needs the export templates for " +
             record->value("platform", "") + ", and project_export says so when they are missing."}};
    // Where the preset writes when it is exported to its own path. Godot does
    // not create a missing folder there: the export fails with "Can't open
    // file for writing", naming the file rather than the folder, on 4.5.1,
    // 4.6.2 and 4.7.2 (#932). Said rather than fixed by making the folder,
    // because adding a preset should not create anything else in the project.
    if (const std::string export_path = record->value("export_path", ""); !export_path.empty()) {
        const auto relative_folder = paths::projectPathFromUtf8(export_path).parent_path();
        std::error_code folder_error;
        const bool folder_exists =
            std::filesystem::is_directory(root.value() / relative_folder, folder_error) && !folder_error;
        payload["export_path_folder_exists"] = folder_exists;
        if (!folder_exists) {
            payload["export_path_note"] =
                "res://" + paths::projectPathToUtf8(relative_folder) +
                "/ does not exist, and Godot does not create it: exporting this preset to its own "
                "export_path fails with \"Can't open file for writing\" until the folder is there. "
                "project_export takes its own output_path and creates that folder.";
        }
    }

    // The file is settled. The editor's re-read below can take seconds, and
    // another writer need not wait for it.
    lock.value().reset();

    // Adding and removing an export platform is the one public event that
    // makes the editor read the file again, and it does so on its next frame.
    // The second request is the wait for that frame: the bridge serves
    // requests from its frame callback, and a request sent only after the
    // first one was answered cannot be served in the same frame.
    bool reloaded = false;
    if (ipc && ipc->isConnected()) {
        const auto failure = [](const Result<json>& response) -> std::optional<json> {
            if (response.isErr()) {
                return json{{"code", response.error().code}, {"message", response.error().message}};
            }
            if (response.value().is_object() && response.value().contains("error")) {
                return response.value()["error"];
            }
            return std::nullopt;
        };
        auto requested = ipc->sendRequest("export.reloadPresets", json{{"step", "request"}}, 5000);
        auto problem = failure(requested);
        if (!problem) {
            auto confirmed = ipc->sendRequest("export.reloadPresets", json{{"step", "confirm"}}, 5000);
            problem = failure(confirmed);
            if (!problem) reloaded = confirmed.value().value("frame_passed", false);
        }
        if (problem) payload["editor_reload_error"] = *problem;
    }
    payload["editor_reloaded"] = reloaded;
    payload["execution_mode"] = reloaded ? "live" : "offline_fallback";
    payload["is_live_engine"] = reloaded;
    if (!reloaded) {
        payload["limitation"] =
            "No attached editor was told about this preset. A Godot editor open on this project "
            "reads export_presets.cfg only when it starts, and writes its own list of presets "
            "back over the file the next time any preset changes in its Export dialog, which "
            "would remove this one. Restart any such editor, or attach Didi to it and add the "
            "preset again, before using its Export dialog. project_export is not affected: it "
            "starts its own Godot, which reads the file.";
    }
    return CallToolResult::successJson(std::move(payload));
}

CallToolResult handleProjectExport(const json& args, std::shared_ptr<ipc::IIpcClient> ipc) {
    (void)ipc;
    if (!args.is_object() || !args.contains("preset") || !args["preset"].is_string() ||
        !args.contains("output_path") || !args["output_path"].is_string()) {
        return CallToolResult::error("preset and output_path are required strings");
    }
    auto root = projectRoot();
    if (root.isErr()) return CallToolResult::fromError(root.error());
    auto output = resolveOutputPath(root.value(), args["output_path"].get<std::string>());
    if (output.isErr()) return CallToolResult::fromError(output.error());
    const std::string preset = args["preset"].get<std::string>();
    // The same check the confirmation preview runs, called from one place, so
    // the preview and the call cannot answer differently about the same file.
    // The preview used to come back clean in all five states this file can be
    // in while the call failed in every one (#652), and the call reported an
    // unparseable file as a missing preset, sending the reader off to add one
    // the file already declares (#651).
    auto record = offline::findExportPreset(preset);
    if (record.isErr()) return CallToolResult::fromError(record.error());
    const bool platform_shipped = record.value().value("detected", true);
    const std::string platform = record.value().value("platform", "");
    const std::string mode = args.value("mode", "release");
    if (mode != "release" && mode != "debug" && mode != "pack") {
        return CallToolResult::error("mode must be release, debug, or pack");
    }
    if (args.contains("overwrite") && !args["overwrite"].is_boolean()) {
        return CallToolResult::error("overwrite must be a boolean");
    }
    std::error_code error;
    if (std::filesystem::exists(output.value(), error) && !args.value("overwrite", false)) {
        // The same envelope, and the same 409, script_create gives the same
        // collision. The whole message is an instruction to retry with a
        // different argument, and as a bare string there was no machine-readable
        // way to know that is what it said (#705). It sits behind a filesystem
        // state rather than behind an argument, which is why the error censuses
        // never reached it.
        return CallToolResult::errorJson(
            409,
            "Export output already exists; pass overwrite: true to replace it: " +
                asResPath(root.value(), output.value()),
            {{"code", "already_exists"},
             {"tool", "project_export"},
             {"output_path", asResPath(root.value(), output.value())},
             {"retry_with", {{"overwrite", true}}},
             {"retryable", false}});
    }
    auto timeout = timeoutSeconds(args, 300, 900);
    if (timeout.isErr()) return CallToolResult::fromError(timeout.error());
    std::filesystem::create_directories(output.value().parent_path(), error);
    if (error) return CallToolResult::error("Failed to create export output directory");
    const std::string flag = mode == "pack" ? "--export-pack" :
                             mode == "debug" ? "--export-debug" : "--export-release";
    // The name is encoded because Godot trims every argument and decodes %20,
    // so a name with a space at either end reached it as a different name and
    // a name of spaces alone as none at all.
    auto run = runGodot(root.value(), offline::isolatedGodotArguments(
        {"--path", paths::projectPathToUtf8(root.value()), flag,
         offline::presetNameForCommandLine(preset), paths::projectPathToUtf8(output.value())}),
        timeout.value());
    if (run.isErr()) return CallToolResult::error("Failed to launch Godot export: " + run.error().message);
    if (run.value().timed_out) return CallToolResult::error("Project export timed out; output status is unknown");
    if (run.value().exit_code != 0) {
        const auto engine_output = withoutTerminalEscapes(run.value().output);
        // Godot checked the preset and refused it, and said why in a block of
        // its own. Missing export templates is the usual one: a release or
        // debug build of any platform needs them and a pack does not. That is
        // a toolchain this machine lacks, as a missing .NET SDK is for
        // csharp_check_build, and it came back as 500 internal_error with the
        // cause left in engine_output, while project_add_export_preset's
        // next_step promised this tool would say so.
        const auto configuration = offline::exportConfigurationErrors(engine_output);
        if (!configuration.errors.empty()) {
            json data = {{"preset", preset},
                         {"platform", platform},
                         {"mode", mode},
                         {"configuration_errors", configuration.errors},
                         {"exit_code", run.value().exit_code},
                         {"engine_output", engine_output},
                         {"output_truncated", run.value().output_truncated},
                         {"retryable", false}};
            const auto others = configuration.errors.size() - configuration.missing_templates.size();
            if (!configuration.missing_templates.empty()) {
                // Godot checks the debug and the release template together and
                // lists debug first, so the one this build needs is named.
                const auto& templates = configuration.missing_templates;
                const auto needed = std::find_if(templates.begin(), templates.end(),
                                                 [&](const std::string& path) {
                                                     return path.find(mode) != std::string::npos;
                                                 });
                const auto& named = needed != templates.end() ? *needed : templates.front();
                data["code"] = "toolchain_unavailable";
                data["reason"] = "export_templates_missing";
                data["missing_templates"] = configuration.missing_templates;
                data["retry_with"] = {{"mode", "pack"}};
                return CallToolResult::errorJson(
                    503,
                    "Godot has no export templates for " + platform + " where it looks for them, "
                        "so it cannot make a " + mode + " build: " + named +
                        ". Install the export templates for this Godot version from the editor's "
                        "Export dialog (Manage Export Templates), or export with mode \"pack\", "
                        "which needs none." +
                        (others > 0 ? " Godot also reported " + std::to_string(others) +
                                          " other configuration error(s), in configuration_errors."
                                    : std::string()),
                    std::move(data));
            }
            data["code"] = "unprocessable";
            data["reason"] = "export_configuration_errors";
            return CallToolResult::errorJson(
                422,
                "Godot refused to export \"" + preset + "\" because of configuration errors, the "
                    "ones its Export dialog shows: " + configuration.errors.front() +
                    (configuration.errors.size() > 1
                         ? " configuration_errors lists all " +
                               std::to_string(configuration.errors.size()) + "."
                         : std::string()),
                std::move(data));
        }
        // The one refusal whose cause is known: the engine did not detect the
        // preset. The file check above already refuses every case it can
        // prove, so what reaches here is a platform Godot does not ship and
        // nothing registered. That is a preset the engine cannot find rather
        // than a fault in Didi, and it used to come back as 500 internal_error
        // (#921).
        if (engine_output.find("Invalid export preset name") != std::string::npos) {
            // Godot names the preset it looked for, which is the one it read
            // off its command line. When that is not the one it was handed, the
            // name did not survive the command line, and "the file changed
            // while the export ran" was the wrong story: a name starting with -
            // is taken as one of Godot's own options when it has one by that
            // name, and the output path is then read as the preset.
            const auto looked_for = offline::invalidPresetNameInEngineOutput(engine_output);
            const bool name_lost = looked_for && *looked_for != preset;
            std::string cause;
            if (name_lost) {
                cause = "Godot looked for a preset named \"" + *looked_for +
                        "\" instead, so the name did not survive its command line." +
                        (preset.front() == '-'
                             ? std::string(" Godot reads an argument that starts with - as one "
                                           "of its own options when it has one by that name. "
                                           "Rename the preset in the editor's Export dialog.")
                             : std::string());
            } else if (platform_shipped) {
                cause = "The file declared it where Godot reads it when the export was checked, "
                        "so export_presets.cfg changed while the export ran.";
            } else {
                cause = "Godot ships no export platform named \"" + platform +
                        "\", and no editor plugin or GDExtension in this project registered one.";
            }
            json data = {{"code", "not_found"},
                         {"reason", "not_detected_by_engine"},
                         {"preset", preset},
                         {"platform", platform},
                         {"mode", mode},
                         {"detected_presets", offline::detectedPresetsInEngineOutput(engine_output)},
                         {"exit_code", run.value().exit_code},
                         {"engine_output", engine_output},
                         {"output_truncated", run.value().output_truncated},
                         {"retryable", false}};
            if (name_lost) data["engine_looked_for"] = *looked_for;
            return CallToolResult::errorJson(
                404,
                "Godot did not detect the export preset \"" + preset + "\". " + cause +
                    " detected_presets is the list Godot printed.",
                std::move(data));
        }
        // The console transcript as data under a key, with the escapes gone,
        // rather than four kilobytes concatenated into a message (#651).
        return CallToolResult::errorJson(
            500, "Godot refused the export. engine_output holds what it printed.",
            json{{"code", "internal_error"},
                 {"preset", preset},
                 {"mode", mode},
                 {"exit_code", run.value().exit_code},
                 {"engine_output", engine_output},
                 {"output_truncated", run.value().output_truncated}});
    }
    if (!std::filesystem::is_regular_file(output.value(), error) || error ||
        std::filesystem::file_size(output.value(), error) == 0 || error) {
        return CallToolResult::error("Godot exited successfully but did not create a non-empty export output");
    }
    return CallToolResult::successJson({
        {"success", true}, {"preset", preset}, {"mode", mode},
        {"output_path", asResPath(root.value(), output.value())},
        {"size_bytes", std::filesystem::file_size(output.value())},
        {"duration_seconds", run.value().duration_seconds}, {"output_truncated", run.value().output_truncated},
        {"execution_mode", "offline_fallback"}
    });
}

CallToolResult handleGridmapExportMeshLibrary(const json& args, std::shared_ptr<ipc::IIpcClient> ipc) {
    (void)ipc;
    if (!args.is_object() || !args.contains("source_scene") || !args["source_scene"].is_string() ||
        !args.contains("output_path") || !args["output_path"].is_string()) {
        return CallToolResult::error("source_scene and output_path are required strings");
    }
    auto root = projectRoot();
    if (root.isErr()) return CallToolResult::fromError(root.error());
    const std::string source_request = args["source_scene"].get<std::string>();
    auto source = paths::resolveProjectFile(source_request);
    if (source.isErr()) return CallToolResult::fromError(source.error());
    if (lower(source.value().extension().string()) != ".tscn") {
        return CallToolResult::error("source_scene must name a .tscn file");
    }
    auto output = resolveOutputPath(root.value(), args["output_path"].get<std::string>());
    if (output.isErr()) return CallToolResult::fromError(output.error());
    const auto extension = lower(output.value().extension().string());
    if (extension != ".meshlib" && extension != ".tres") {
        // Reached only once source_scene is valid, which is why a census
        // generating junk arguments always got this tool's other error and
        // never this one (#705).
        return CallToolResult::errorJson(
            400, "output_path must end in .meshlib or .tres",
            {{"code", "invalid_arguments"},
             {"tool", "gridmap_export_mesh_library"},
             {"parameter", "output_path"},
             {"allowed_extensions", json::array({".meshlib", ".tres"})},
             {"retryable", false}});
    }
    if (args.contains("overwrite") && !args["overwrite"].is_boolean()) {
        return CallToolResult::error("overwrite must be a boolean");
    }
    if (args.contains("generate_collisions") && !args["generate_collisions"].is_boolean()) {
        return CallToolResult::error("generate_collisions must be a boolean");
    }
    std::error_code error;
    if (std::filesystem::exists(output.value(), error) && !args.value("overwrite", false)) {
        return CallToolResult::errorJson(
            409, "MeshLibrary output already exists; pass overwrite: true to replace it",
            {{"code", "already_exists"}, {"retry_with", {{"overwrite", true}}}});
    }
    auto timeout = timeoutSeconds(args, 60, 300);
    if (timeout.isErr()) return CallToolResult::fromError(timeout.error());
    std::filesystem::create_directories(output.value().parent_path(), error);
    if (error) return CallToolResult::error("Failed to create MeshLibrary output directory");
    auto helper = TemporaryScript::create("mesh-library", meshLibraryHelperSource());
    if (helper.isErr()) return CallToolResult::fromError(helper.error());
    const std::string output_res = asResPath(root.value(), output.value());
    auto run = runGodot(root.value(), offline::isolatedGodotArguments(
        {"--path", paths::projectPathToUtf8(root.value()), "--script",
         paths::projectPathToUtf8(helper.value().path()), "--", source_request,
         output_res, args.value("generate_collisions", true) ? "true" : "false"}), timeout.value());
    if (run.isErr()) return CallToolResult::error("Failed to launch MeshLibrary conversion: " + run.error().message);
    if (run.value().timed_out) return CallToolResult::error("MeshLibrary conversion timed out; output status is unknown");
    if (run.value().exit_code != 0) {
        // The old message ended in a colon with nothing after it: shaped to
        // carry a reason and carrying none, so a caller could not tell "this
        // scene has no MeshInstance children" from "the editor refused" (#657).
        return CallToolResult::errorJson(
            500,
            "The MeshLibrary conversion failed. engine_output holds what Godot printed; a scene "
            "with no MeshInstance3D children is the usual cause.",
            json{{"code", "internal_error"},
                 {"source_scene", source_request},
                 {"output_path", output_res},
                 {"exit_code", run.value().exit_code},
                 {"engine_output", withoutTerminalEscapes(run.value().output)},
                 {"output_truncated", run.value().output_truncated}});
    }
    auto marker = parseMarker(run.value().output, "DIDI_PHASE5_RESULT:");
    if (marker.isErr()) return CallToolResult::fromError(marker.error());
    if (!std::filesystem::is_regular_file(output.value(), error) || error) {
        return CallToolResult::error("Godot did not create the MeshLibrary output");
    }
    return CallToolResult::successJson({
        {"success", true}, {"source_scene", source_request}, {"output_path", output_res},
        {"item_count", marker.value().value("item_count", 0)},
        {"generated_collisions", args.value("generate_collisions", true)},
        {"duration_seconds", run.value().duration_seconds}, {"execution_mode", "offline_fallback"}
    });
}

CallToolResult handleUiHitTest(const json& args, std::shared_ptr<ipc::IIpcClient> ipc) {
    if (!args.is_object() || !args.contains("point") || !args["point"].is_object()) {
        return CallToolResult::error("point is required and must be an object");
    }
    const auto& point = args["point"];
    if (!point.contains("x") || !point["x"].is_number() ||
        !point.contains("y") || !point["y"].is_number()) {
        return CallToolResult::error("point.x and point.y are required finite numbers");
    }
    const double x = point["x"].get<double>();
    const double y = point["y"].get<double>();
    if (!std::isfinite(x) || !std::isfinite(y)) {
        return CallToolResult::error("point.x and point.y must be finite");
    }
    if (args.contains("root_path") && !args["root_path"].is_string()) {
        return CallToolResult::error("root_path must be a string");
    }
    if (args.contains("include_mouse_filter_ignore") &&
        !args["include_mouse_filter_ignore"].is_boolean()) {
        return CallToolResult::error("include_mouse_filter_ignore must be a boolean");
    }
    if (args.contains("max_results")) {
        if (!args["max_results"].is_number_integer()) {
            return CallToolResult::error("max_results must be an integer");
        }
        const int limit = args["max_results"].get<int>();
        if (limit < 1 || limit > 256) return CallToolResult::error("max_results must be from 1 to 256");
    }
    if (!ipc || !ipc->isConnected()) {
        return CallToolResult::error("UI hit-testing requires a live Godot editor.");
    }
    auto response = ipc->sendRequest("ui.hitTest", args, ipc::kWaitForDefinitiveResponse);
    if (response.isErr()) return CallToolResult::error("UI hit-test failed: " + response.error().message);
    if (!response.value().is_object() || !response.value().contains("hits") ||
        !response.value()["hits"].is_array()) {
        return CallToolResult::error("Live UI hit-test returned a malformed response");
    }
    return CallToolResult::successJson(response.value());
}


CallToolResult handleUiListControls(const json& args, std::shared_ptr<ipc::IIpcClient> ipc) {
    if (!args.is_object()) return CallToolResult::error("arguments must be an object");
    if (args.contains("root_path") &&
        (!args["root_path"].is_string() || args["root_path"].get<std::string>().size() > 1024)) {
        return CallToolResult::error("root_path must be a string of at most 1024 bytes");
    }
    if (args.contains("visible_only") && !args["visible_only"].is_boolean()) {
        return CallToolResult::error("visible_only must be a boolean");
    }
    if (args.contains("include_text") && !args["include_text"].is_boolean()) {
        return CallToolResult::error("include_text must be a boolean");
    }
    if (args.contains("max_results")) {
        if (!args["max_results"].is_number_integer()) {
            return CallToolResult::error("max_results must be an integer");
        }
        const int limit = args["max_results"].get<int>();
        if (limit < 1 || limit > 256) {
            return CallToolResult::error("max_results must be from 1 to 256");
        }
    }
    if (args.contains("class_filter")) {
        const auto& filter = args["class_filter"];
        if (!filter.is_array() || filter.empty() || filter.size() > 16) {
            return CallToolResult::error("class_filter must be an array of 1 to 16 class names");
        }
        for (const auto& entry : filter) {
            if (!entry.is_string() || entry.get<std::string>().empty() ||
                entry.get<std::string>().size() > 64) {
                return CallToolResult::error(
                    "class_filter entries must be 1 to 64 byte class names");
            }
        }
    }
    if (!ipc || !ipc->isConnected()) {
        return CallToolResult::error(
            "Listing live Control nodes requires a connected Godot editor or game.");
    }
    auto response = ipc->sendRequest("ui.listControls", args, ipc::kWaitForDefinitiveResponse);
    if (response.isErr()) {
        return CallToolResult::error("UI control listing failed: " + response.error().message);
    }
    if (!response.value().is_object() || !response.value().contains("controls") ||
        !response.value()["controls"].is_array()) {
        return CallToolResult::error("Live UI control listing returned a malformed response");
    }
    return CallToolResult::successJson(response.value());
}


CallToolResult handleSceneGetSelection(const json& args, std::shared_ptr<ipc::IIpcClient> ipc) {
    if (!args.is_object()) return CallToolResult::error("arguments must be an object");
    if (!args.empty()) return CallToolResult::error("scene_get_selection takes no arguments");
    if (!ipc || !ipc->isConnected()) {
        return CallToolResult::error(
            "Reading the editor selection requires a live Godot editor. There is no offline "
            "answer: a selection exists only in a running editor.");
    }
    auto response = ipc->sendRequest("editor.getSelection", json::object(),
                                     ipc::kWaitForDefinitiveResponse);
    if (response.isErr()) {
        return CallToolResult::error("Reading the editor selection failed: " +
                                     response.error().message);
    }
    if (!response.value().is_object() || !response.value().contains("selected") ||
        !response.value()["selected"].is_array()) {
        return CallToolResult::error("Live editor selection returned a malformed response");
    }
    return CallToolResult::successJson(response.value());
}

} // namespace didi::mcp
