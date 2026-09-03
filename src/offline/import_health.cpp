#include "didi/offline/import_health.hpp"

#include "didi/common/project_path.hpp"

#include <algorithm>
#include <cctype>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <optional>
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

struct ImportSections {
    std::vector<std::string> remap;
    std::vector<std::string> dependencies;
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
        if (active == ActiveSection::Remap) sections.remap.push_back(line);
        if (active == ActiveSection::Dependencies) sections.dependencies.push_back(line);
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

std::optional<std::pair<std::string, std::string>> assignment(const std::string& line) {
    const auto equals = line.find('=');
    if (equals == std::string::npos) return std::nullopt;
    const auto key = strings::trim(line.substr(0, equals));
    if (key.empty()) return std::nullopt;
    return std::pair{key, strings::trim(line.substr(equals + 1))};
}

bool isPathKey(const std::string& key) {
    if (key == "path") return true;
    if (!strings::startsWith(key, "path.") || key.size() == 5) return false;
    return std::all_of(key.begin() + 5, key.end(), [](unsigned char character) {
        return std::isalnum(character) != 0 || character == '_' || character == '.' ||
               character == '-';
    });
}

std::optional<std::string> quotedValue(const std::string& value, bool allow_empty) {
    if (value.size() < 2 || value.front() != '"' || value.back() != '"') return std::nullopt;
    const auto unquoted = value.substr(1, value.size() - 2);
    if ((!allow_empty && unquoted.empty()) || unquoted.find('"') != std::string::npos) {
        return std::nullopt;
    }
    return unquoted;
}

std::optional<std::vector<std::string>> destinationValues(const std::string& value) {
    if (value.size() < 2 || value.front() != '[' || value.back() != ']') return std::nullopt;
    std::vector<std::string> outputs;
    size_t cursor = 1;
    const size_t end = value.size() - 1;
    const auto skip_space = [&] {
        while (cursor < end && std::isspace(static_cast<unsigned char>(value[cursor])) != 0) {
            ++cursor;
        }
    };
    skip_space();
    if (cursor == end) return outputs;
    while (cursor < end) {
        if (value[cursor] != '"') return std::nullopt;
        const auto close = value.find('"', cursor + 1);
        if (close == std::string::npos || close >= end || close == cursor + 1) return std::nullopt;
        outputs.push_back(value.substr(cursor + 1, close - cursor - 1));
        if (outputs.size() > kMaxImportPathsPerMetadata) return std::nullopt;
        cursor = close + 1;
        skip_space();
        if (cursor == end) break;
        if (value[cursor] != ',') return std::nullopt;
        ++cursor;
        skip_space();
        if (cursor == end) return std::nullopt;
    }
    return outputs;
}

std::optional<ImportMetadata> parseMetadata(const std::string& text) {
    const auto sections = importSections(text);
    ImportMetadata metadata;
    for (const auto& line : sections.remap) {
        const auto field = assignment(line);
        if (!field) continue;
        if (field->first == "valid" && field->second == "false") return std::nullopt;
        if (isPathKey(field->first)) {
            const auto output = quotedValue(field->second, true);
            if (!output) return std::nullopt;
            if (!output->empty()) metadata.outputs.push_back(*output);
        }
    }

    size_t source_assignments = 0;
    size_t destination_assignments = 0;
    for (const auto& line : sections.dependencies) {
        const auto field = assignment(line);
        if (!field) continue;
        if (field->first == "source_file") {
            ++source_assignments;
            const auto source = quotedValue(field->second, false);
            if (!source) return std::nullopt;
            metadata.source = *source;
        } else if (field->first == "dest_files") {
            ++destination_assignments;
            const auto outputs = destinationValues(field->second);
            if (!outputs) return std::nullopt;
            metadata.outputs.insert(metadata.outputs.end(), outputs->begin(), outputs->end());
        }
    }
    if (source_assignments != 1 || destination_assignments > 1) return std::nullopt;
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
