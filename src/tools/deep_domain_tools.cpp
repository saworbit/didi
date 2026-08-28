#include "didi/mcp/mcp_protocol.hpp"
#include "didi/common/ipc_channel.hpp"
#include "didi/common/project_path.hpp"
#include "didi/offline/deep_domain_support.hpp"
#include "didi/offline/process_runner.hpp"
#include "didi/offline/test_runner.hpp"
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
    for (const auto& component : relative) {
        if (component == "..") return Error::invalidArgument("output_path cannot contain parent traversal");
    }
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

std::string dotnetExecutable() {
    const char* configured = std::getenv("DOTNET_BIN");
    return configured && *configured ? configured : "dotnet";
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

class ScopedOfflineHelperEnvironment {
public:
    ScopedOfflineHelperEnvironment() {
        if (const auto* current = std::getenv(offline::kOfflineHelperEnvironment)) {
            previous_ = current;
        }
#if defined(_WIN32)
        ready_ = _putenv_s(offline::kOfflineHelperEnvironment, "1") == 0;
#else
        ready_ = setenv(offline::kOfflineHelperEnvironment, "1", 1) == 0;
#endif
    }

    ~ScopedOfflineHelperEnvironment() {
        if (!ready_) return;
#if defined(_WIN32)
        (void)_putenv_s(offline::kOfflineHelperEnvironment,
                        previous_.has_value() ? previous_->c_str() : "");
#else
        if (previous_.has_value()) (void)setenv(offline::kOfflineHelperEnvironment, previous_->c_str(), 1);
        else (void)unsetenv(offline::kOfflineHelperEnvironment);
#endif
    }

    bool ready() const { return ready_; }

private:
    std::optional<std::string> previous_;
    bool ready_{false};
};

Result<offline::ProcessResult> runGodot(const std::filesystem::path& root,
                                        std::vector<std::string> arguments,
                                        int timeout_seconds) {
    static std::mutex environment_mutex;
    std::lock_guard<std::mutex> environment_lock(environment_mutex);
    ScopedOfflineHelperEnvironment offline_environment;
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
    if (root.isErr()) return CallToolResult::error(root.error().message);
    auto project = selectCSharpProject(root.value(), args);
    if (project.isErr()) return CallToolResult::error(project.error().message);
    const std::string configuration = args.value("configuration", "Debug");
    if (configuration != "Debug" && configuration != "Release") {
        return CallToolResult::error("configuration must be Debug or Release");
    }
    auto timeout = timeoutSeconds(args, 60, 300);
    if (timeout.isErr()) return CallToolResult::error(timeout.error().message);

    offline::ProcessRequest request;
    request.executable = dotnetExecutable();
    request.arguments = {"build", paths::projectPathToUtf8(project.value()), "--nologo",
                         "--verbosity:minimal", "--configuration", configuration};
    request.working_directory = root.value();
    request.timeout = std::chrono::seconds(timeout.value());
    request.max_output_bytes = kMaxProcessOutput;
    auto run = offline::runProcess(request);
    if (run.isErr()) return CallToolResult::error("Failed to run dotnet build: " + run.error().message);
    if (run.value().timed_out) return CallToolResult::error("dotnet build timed out before completion");
    auto diagnostics = offline::parseMsBuildDiagnostics(run.value().output);
    const bool has_errors = run.value().exit_code != 0 || std::any_of(
        diagnostics.begin(), diagnostics.end(), [](const auto& value) { return value.severity == "error"; });
    return CallToolResult::successJson({
        {"success", !has_errors}, {"has_errors", has_errors}, {"exit_code", run.value().exit_code},
        {"project_file", asResPath(root.value(), project.value())}, {"configuration", configuration},
        {"diagnostics", diagnosticsJson(diagnostics)}, {"diagnostics_count", diagnostics.size()},
        {"duration_seconds", run.value().duration_seconds}, {"output_truncated", run.value().output_truncated},
        {"raw_output", run.value().output}, {"execution_mode", "offline_fallback"}
    });
}

CallToolResult handleShaderCheckCompile(const json& args, std::shared_ptr<ipc::IIpcClient> ipc) {
    (void)ipc;
    if (!args.is_object() || !args.contains("shader_path") || !args["shader_path"].is_string()) {
        return CallToolResult::error("shader_path is required and must be a string");
    }
    auto root = projectRoot();
    if (root.isErr()) return CallToolResult::error(root.error().message);
    const std::string requested = args["shader_path"].get<std::string>();
    auto shader = paths::resolveProjectFile(requested);
    if (shader.isErr()) return CallToolResult::error(shader.error().message);
    if (lower(shader.value().extension().string()) != ".gdshader") {
        return CallToolResult::error("shader_path must name a .gdshader file");
    }
    auto timeout = timeoutSeconds(args, 30, 300);
    if (timeout.isErr()) return CallToolResult::error(timeout.error().message);
    auto helper = TemporaryScript::create("shader-check", shaderHelperSource());
    if (helper.isErr()) return CallToolResult::error(helper.error().message);
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
    return CallToolResult::successJson({
        {"success", !has_errors}, {"has_errors", has_errors}, {"exit_code", run.value().exit_code},
        {"shader_path", requested}, {"diagnostics", diagnosticsJson(diagnostics)},
        {"diagnostics_count", diagnostics.size()}, {"duration_seconds", run.value().duration_seconds},
        {"output_truncated", run.value().output_truncated}, {"raw_output", run.value().output},
        {"execution_mode", "offline_fallback"}
    });
}

CallToolResult handleProjectListExportPresets(const json& args, std::shared_ptr<ipc::IIpcClient> ipc) {
    (void)ipc;
    if (!args.is_object() || !args.empty()) return CallToolResult::error("Export preset list arguments must be an empty object");
    auto root = projectRoot();
    if (root.isErr()) return CallToolResult::error(root.error().message);
    const auto path = root.value() / "export_presets.cfg";
    auto contents = readBounded(path, kMaxPresetFile);
    if (contents.isErr()) return CallToolResult::error(contents.error().message);
    auto presets = offline::parseExportPresets(contents.value());
    if (presets.empty() && !strings::trim(contents.value()).empty()) {
        return CallToolResult::error("export_presets.cfg is malformed or contains no complete unique presets");
    }
    return CallToolResult::successJson({{"presets", presets}, {"preset_count", presets.size()},
                                       {"execution_mode", "offline_fallback"}, {"sensitive_options_omitted", true}});
}

CallToolResult handleProjectExport(const json& args, std::shared_ptr<ipc::IIpcClient> ipc) {
    (void)ipc;
    if (!args.is_object() || !args.contains("preset") || !args["preset"].is_string() ||
        !args.contains("output_path") || !args["output_path"].is_string()) {
        return CallToolResult::error("preset and output_path are required strings");
    }
    auto root = projectRoot();
    if (root.isErr()) return CallToolResult::error(root.error().message);
    auto output = resolveOutputPath(root.value(), args["output_path"].get<std::string>());
    if (output.isErr()) return CallToolResult::error(output.error().message);
    const std::string preset = args["preset"].get<std::string>();
    auto preset_file = readBounded(root.value() / "export_presets.cfg", kMaxPresetFile);
    if (preset_file.isErr()) return CallToolResult::error(preset_file.error().message);
    const auto presets = offline::parseExportPresets(preset_file.value());
    const bool found = std::any_of(presets.begin(), presets.end(), [&](const json& item) {
        return item.value("name", "") == preset;
    });
    if (!found) return CallToolResult::error("Export preset not found: " + preset);
    const std::string mode = args.value("mode", "release");
    if (mode != "release" && mode != "debug" && mode != "pack") {
        return CallToolResult::error("mode must be release, debug, or pack");
    }
    if (args.contains("overwrite") && !args["overwrite"].is_boolean()) {
        return CallToolResult::error("overwrite must be a boolean");
    }
    std::error_code error;
    if (std::filesystem::exists(output.value(), error) && !args.value("overwrite", false)) {
        return CallToolResult::error("Export output already exists; pass overwrite: true to replace it");
    }
    auto timeout = timeoutSeconds(args, 300, 900);
    if (timeout.isErr()) return CallToolResult::error(timeout.error().message);
    std::filesystem::create_directories(output.value().parent_path(), error);
    if (error) return CallToolResult::error("Failed to create export output directory");
    const std::string flag = mode == "pack" ? "--export-pack" :
                             mode == "debug" ? "--export-debug" : "--export-release";
    auto run = runGodot(root.value(), offline::isolatedGodotArguments(
        {"--path", paths::projectPathToUtf8(root.value()), flag, preset,
         paths::projectPathToUtf8(output.value())}), timeout.value());
    if (run.isErr()) return CallToolResult::error("Failed to launch Godot export: " + run.error().message);
    if (run.value().timed_out) return CallToolResult::error("Project export timed out; output status is unknown");
    if (run.value().exit_code != 0) return CallToolResult::error("Godot export failed: " + run.value().output);
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
    if (root.isErr()) return CallToolResult::error(root.error().message);
    const std::string source_request = args["source_scene"].get<std::string>();
    auto source = paths::resolveProjectFile(source_request);
    if (source.isErr()) return CallToolResult::error(source.error().message);
    if (lower(source.value().extension().string()) != ".tscn") {
        return CallToolResult::error("source_scene must name a .tscn file");
    }
    auto output = resolveOutputPath(root.value(), args["output_path"].get<std::string>());
    if (output.isErr()) return CallToolResult::error(output.error().message);
    const auto extension = lower(output.value().extension().string());
    if (extension != ".meshlib" && extension != ".tres") {
        return CallToolResult::error("output_path must end in .meshlib or .tres");
    }
    if (args.contains("overwrite") && !args["overwrite"].is_boolean()) {
        return CallToolResult::error("overwrite must be a boolean");
    }
    if (args.contains("generate_collisions") && !args["generate_collisions"].is_boolean()) {
        return CallToolResult::error("generate_collisions must be a boolean");
    }
    std::error_code error;
    if (std::filesystem::exists(output.value(), error) && !args.value("overwrite", false)) {
        return CallToolResult::error("MeshLibrary output already exists; pass overwrite: true to replace it");
    }
    auto timeout = timeoutSeconds(args, 60, 300);
    if (timeout.isErr()) return CallToolResult::error(timeout.error().message);
    std::filesystem::create_directories(output.value().parent_path(), error);
    if (error) return CallToolResult::error("Failed to create MeshLibrary output directory");
    auto helper = TemporaryScript::create("mesh-library", meshLibraryHelperSource());
    if (helper.isErr()) return CallToolResult::error(helper.error().message);
    const std::string output_res = asResPath(root.value(), output.value());
    auto run = runGodot(root.value(), offline::isolatedGodotArguments(
        {"--path", paths::projectPathToUtf8(root.value()), "--script",
         paths::projectPathToUtf8(helper.value().path()), "--", source_request,
         output_res, args.value("generate_collisions", true) ? "true" : "false"}), timeout.value());
    if (run.isErr()) return CallToolResult::error("Failed to launch MeshLibrary conversion: " + run.error().message);
    if (run.value().timed_out) return CallToolResult::error("MeshLibrary conversion timed out; output status is unknown");
    if (run.value().exit_code != 0) return CallToolResult::error("MeshLibrary conversion failed: " + run.value().output);
    auto marker = parseMarker(run.value().output, "DIDI_PHASE5_RESULT:");
    if (marker.isErr()) return CallToolResult::error(marker.error().message);
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

} // namespace didi::mcp
