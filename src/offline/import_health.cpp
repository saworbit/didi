#include "didi/offline/import_health.hpp"

#include "didi/common/project_path.hpp"

#include <algorithm>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <optional>
#include <regex>
#include <set>
#include <sstream>
#include <tuple>
#include <system_error>
#include <vector>

namespace didi::offline {
namespace {

namespace fs = std::filesystem;

struct ImportMetadata {
    std::string source;
    std::vector<std::string> outputs;
};

struct ImportIssue {
    std::string metadata;
    std::string kind;
    std::string source;
    std::string target;

    auto key() const { return std::tie(metadata, kind, target, source); }
};

struct IssueLess {
    bool operator()(const ImportIssue& left, const ImportIssue& right) const {
        return left.key() < right.key();
    }
};

size_t matchCount(const std::string& text, const std::regex& pattern) {
    return static_cast<size_t>(std::distance(std::sregex_iterator(text.begin(), text.end(), pattern),
                                             std::sregex_iterator()));
}

struct ImportSections {
    std::string remap{"\n"};
    std::string dependencies{"\n"};
};

ImportSections importSections(const std::string& text) {
    std::istringstream input(text);
    ImportSections sections;
    enum class ActiveSection { Other, Remap, Dependencies } active{ActiveSection::Other};
    std::string line;
    while (std::getline(input, line)) {
        const auto trimmed = strings::trim(line);
        if (!trimmed.empty() && (trimmed.front() == ';' || trimmed.front() == '#')) continue;
        if (trimmed == "[remap]") {
            active = ActiveSection::Remap;
            continue;
        }
        if (trimmed == "[deps]") {
            active = ActiveSection::Dependencies;
            continue;
        }
        if (trimmed.size() >= 2 && trimmed.front() == '[' && trimmed.back() == ']') {
            active = ActiveSection::Other;
            continue;
        }
        if (active == ActiveSection::Remap) sections.remap += line + '\n';
        if (active == ActiveSection::Dependencies) sections.dependencies += line + '\n';
    }
    return sections;
}

std::optional<std::string> readBounded(const fs::path& path) {
    std::ifstream input(path, std::ios::binary);
    if (!input) return std::nullopt;
    std::string contents(kMaxImportMetadataBytes + 1, '\0');
    input.read(contents.data(), static_cast<std::streamsize>(contents.size()));
    const auto bytes_read = static_cast<size_t>(input.gcount());
    if (bytes_read > kMaxImportMetadataBytes) return std::nullopt;
    contents.resize(bytes_read);
    return contents;
}

std::optional<ImportMetadata> parseMetadata(const std::string& text) {
    const auto sections = importSections(text);
    static const std::regex source_assignment(R"re(\n[ \t]*source_file\s*=)re");
    static const std::regex source_pattern(R"re(\n[ \t]*source_file\s*=\s*"([^"]+)")re");
    static const std::regex path_assignment(R"re(\n[ \t]*path(?:\.[A-Za-z0-9_.-]+)?\s*=)re");
    static const std::regex path_pattern(R"re(\n[ \t]*path(?:\.[A-Za-z0-9_.-]+)?\s*=\s*"([^"]*)")re");
    static const std::regex destinations_assignment(R"re(\n[ \t]*dest_files\s*=)re");
    static const std::regex destinations_pattern(R"re(\n[ \t]*dest_files\s*=\s*\[([^\]]*)\])re");
    static const std::regex quoted_value(R"re("([^"]+)")re");
    static const std::regex explicitly_invalid(R"re(\n[ \t]*valid\s*=\s*false\b)re");

    std::smatch source_match;
    if (std::regex_search(sections.remap, explicitly_invalid) ||
        matchCount(sections.dependencies, source_assignment) != 1 ||
        !std::regex_search(sections.dependencies, source_match, source_pattern) ||
        matchCount(sections.dependencies, source_pattern) != 1 ||
        matchCount(sections.remap, path_assignment) != matchCount(sections.remap, path_pattern) ||
        matchCount(sections.dependencies, destinations_assignment) > 1) {
        return std::nullopt;
    }

    ImportMetadata metadata;
    metadata.source = source_match[1].str();
    for (auto it = std::sregex_iterator(sections.remap.begin(), sections.remap.end(), path_pattern);
         it != std::sregex_iterator(); ++it) {
        const auto output = (*it)[1].str();
        if (!output.empty()) metadata.outputs.push_back(output);
    }
    std::smatch destinations_match;
    const bool destinations_declared = std::regex_search(sections.dependencies,
                                                         destinations_assignment);
    if (std::regex_search(sections.dependencies, destinations_match, destinations_pattern)) {
        const auto values = destinations_match[1].str();
        for (auto it = std::sregex_iterator(values.begin(), values.end(), quoted_value);
             it != std::sregex_iterator(); ++it) {
            metadata.outputs.push_back((*it)[1].str());
        }
        const auto residue = std::regex_replace(values, quoted_value, "");
        if (std::any_of(residue.begin(), residue.end(), [](unsigned char character) {
                return character != ',' && character != ' ' && character != '\t' &&
                       character != '\r' && character != '\n';
            })) {
            return std::nullopt;
        }
    } else if (destinations_declared) {
        return std::nullopt;
    }
    if (metadata.outputs.size() > kMaxImportPathsPerMetadata) return std::nullopt;
    return metadata;
}

std::optional<fs::path> resolveResourcePath(const fs::path& root, const std::string& value) {
    if (!strings::startsWith(value, "res://") || value.size() == 6 ||
        value.find('\\') != std::string::npos) {
        return std::nullopt;
    }
    fs::path relative;
    try {
        relative = paths::projectPathFromUtf8(value.substr(6));
    } catch (const fs::filesystem_error&) {
        return std::nullopt;
    }
    if (relative.is_absolute() || relative.has_root_name()) return std::nullopt;
    for (const auto& component : relative) {
        if (component == "." || component == ".." || component.empty()) return std::nullopt;
    }
    const auto candidate = (root / relative).lexically_normal();
    if (!paths::isWithinProject(root, candidate)) return std::nullopt;

    auto cursor = root;
    for (const auto& component : relative) {
        cursor /= component;
        std::error_code status_error;
        const auto status = fs::symlink_status(cursor, status_error);
        if (status_error == std::errc::no_such_file_or_directory) break;
        if (status_error) return std::nullopt;
        if (fs::is_symlink(status)) return std::nullopt;
        if (!fs::exists(status)) break;
    }
    return candidate;
}

std::string metadataResourcePath(const fs::path& root, const fs::path& metadata) {
    std::error_code error;
    const auto relative = fs::relative(metadata, root, error);
    return error ? std::string{} : "res://" + paths::projectPathToUtf8(relative);
}

} // namespace

json inspectImportHealth(const std::string& root_dir, size_t max_findings) {
    json result = {
        {"scanned_import_metadata", 0},
        {"import_issues", json::array()},
        {"import_issue_count", 0}
    };

    std::error_code root_error;
    const auto root = fs::weakly_canonical(paths::projectPathFromUtf8(root_dir), root_error);
    if (root_error || !fs::is_directory(root, root_error) || root_error) return result;

    size_t scanned = 0;
    bool truncated = false;
    const size_t retained_limit = std::min(max_findings, static_cast<size_t>(5000));
    size_t issue_count = 0;
    std::set<ImportIssue, IssueLess> retained_issues;
    for (auto it = fs::recursive_directory_iterator(root, fs::directory_options::skip_permission_denied);
         it != fs::recursive_directory_iterator(); ++it) {
        const auto& entry = *it;
        std::error_code status_error;
        if (fs::is_symlink(entry.symlink_status(status_error)) || status_error) {
            if (entry.is_directory(status_error)) it.disable_recursion_pending();
            continue;
        }
        if (entry.is_directory(status_error)) {
            const auto name = entry.path().filename();
            const auto name_text = paths::projectPathToUtf8(name);
            if (name == ".git" || name == ".godot" || name == "build" ||
                strings::startsWith(name_text, "build-") || name == ".worktrees" ||
                name == ".vs" || name == "out" || name == "bin") {
                it.disable_recursion_pending();
            }
            continue;
        }
        if (!entry.is_regular_file(status_error) || status_error ||
            entry.path().extension() != ".import") {
            continue;
        }
        if (scanned >= kMaxImportMetadataFiles) {
            truncated = true;
            break;
        }
        ++scanned;
        const auto metadata_path = metadataResourcePath(root, entry.path());
        std::set<ImportIssue, IssueLess> metadata_issues;
        const auto retainMetadataIssues = [&]() {
            issue_count += metadata_issues.size();
            for (const auto& issue : metadata_issues) {
                retained_issues.insert(issue);
                if (retained_issues.size() > retained_limit) {
                    retained_issues.erase(std::prev(retained_issues.end()));
                }
            }
        };
        const auto contents = readBounded(entry.path());
        const auto parsed = contents ? parseMetadata(*contents) : std::nullopt;
        if (!parsed) {
            metadata_issues.insert({metadata_path, "invalid_import_metadata", "", metadata_path});
            retainMetadataIssues();
            continue;
        }

        const auto expected_source = metadata_path.substr(0, metadata_path.size() - 7);
        if (parsed->source != expected_source) {
            metadata_issues.insert({metadata_path, "invalid_import_metadata", parsed->source,
                                    parsed->source});
            retainMetadataIssues();
            continue;
        }

        const auto source_path = resolveResourcePath(root, parsed->source);
        if (!source_path) {
            metadata_issues.insert({metadata_path, "invalid_import_metadata", parsed->source,
                                    parsed->source});
            retainMetadataIssues();
            continue;
        }
        std::error_code source_error;
        const bool source_exists = fs::is_regular_file(*source_path, source_error) && !source_error;
        if (!source_exists) {
            metadata_issues.insert({metadata_path, "missing_import_source", parsed->source,
                                    parsed->source});
        }

        for (const auto& output : parsed->outputs) {
            const auto output_path = resolveResourcePath(root, output);
            if (!output_path) {
                metadata_issues.insert({metadata_path, "invalid_import_metadata", parsed->source,
                                        output});
                continue;
            }
            std::error_code output_error;
            const bool output_exists = fs::is_regular_file(*output_path, output_error) && !output_error;
            if (!output_exists) {
                metadata_issues.insert({metadata_path, "missing_import_output", parsed->source,
                                        output});
                continue;
            }
            if (!source_exists) continue;
            std::error_code source_time_error;
            std::error_code output_time_error;
            const auto source_time = fs::last_write_time(*source_path, source_time_error);
            const auto output_time = fs::last_write_time(*output_path, output_time_error);
            if (!source_time_error && !output_time_error && source_time > output_time) {
                metadata_issues.insert({metadata_path, "source_newer_than_output", parsed->source,
                                        output});
            }
        }
        retainMetadataIssues();
    }

    result["scanned_import_metadata"] = scanned;
    result["import_issue_count"] = issue_count;
    for (const auto& issue : retained_issues) {
        result["import_issues"].push_back({
            {"metadata", issue.metadata},
            {"kind", issue.kind},
            {"source", issue.source},
            {"target", issue.target}
        });
    }
    if (truncated) result["import_scan_truncated"] = true;
    return result;
}

} // namespace didi::offline
