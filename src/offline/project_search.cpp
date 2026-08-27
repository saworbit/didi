#include "didi/offline/project_search.hpp"

#include <algorithm>
#include <cctype>
#include <fstream>
#include <set>
#include <sstream>
#include <string_view>

namespace didi::offline {
namespace fs = std::filesystem;
namespace {

const std::set<std::string> kAllowedExtensions = {".gd", ".cs", ".tscn", ".tres"};
const std::set<std::string> kSkippedDirectories = {
    ".git", ".godot", ".worktrees", "build", "build-clean", "build-vs", "out", "bin", ".vs"
};

std::string normalizedPath(const fs::path& path) {
    auto value = path.generic_string();
#if defined(_WIN32)
    std::transform(value.begin(), value.end(), value.begin(), [](unsigned char c) {
        return static_cast<char>(std::tolower(c));
    });
#endif
    return value;
}

bool isWithin(const fs::path& root, const fs::path& candidate) {
    const auto root_value = normalizedPath(root.lexically_normal());
    const auto candidate_value = normalizedPath(candidate.lexically_normal());
    if (candidate_value == root_value) return true;
    return candidate_value.size() > root_value.size() &&
           candidate_value.compare(0, root_value.size(), root_value) == 0 &&
           candidate_value[root_value.size()] == '/';
}

Result<fs::path> resolveSearchRoot(const fs::path& project_root, const std::string& search_path) {
    if (!strings::startsWith(search_path, "res://")) {
        return Error::invalidArgument("search_path must begin with res://");
    }
    const fs::path relative(search_path.substr(6));
    if (relative.is_absolute() || relative.has_root_name()) {
        return Error::invalidArgument("search_path must remain beneath res://");
    }
    for (const auto& part : relative) {
        if (part == "..") return Error::invalidArgument("search_path cannot contain parent traversal");
    }
    std::error_code ec;
    const auto target = fs::weakly_canonical(project_root / relative, ec);
    if (ec || !isWithin(project_root, target)) {
        return Error::invalidArgument("search_path resolves outside the project root");
    }
    if (!fs::is_directory(target, ec) || ec) {
        return Error::notFound("search_path does not identify a project directory");
    }
    return target;
}

Result<std::set<std::string>> validateExtensions(const std::vector<std::string>& extensions) {
    if (extensions.empty()) return Error::invalidArgument("extensions must not be empty");
    std::set<std::string> selected;
    for (auto extension : extensions) {
        std::transform(extension.begin(), extension.end(), extension.begin(), [](unsigned char c) {
            return static_cast<char>(std::tolower(c));
        });
        if (!kAllowedExtensions.count(extension)) {
            return Error::invalidArgument("extensions may contain only .gd, .cs, .tscn, and .tres");
        }
        selected.insert(std::move(extension));
    }
    return selected;
}

Result<void> validateCommon(const SearchOptions& options) {
    if (options.query.empty() || options.query.size() > kSearchMaxQueryBytes ||
        options.query.find('\0') != std::string::npos) {
        return Error::invalidArgument("query must contain 1 to 256 bytes and no NUL");
    }
    if (options.max_results < 1 || options.max_results > kSearchMaxResults) {
        return Error::invalidArgument("max_results must be from 1 to 500");
    }
    return Result<void>::ok();
}

std::string asciiFold(std::string value) {
    std::transform(value.begin(), value.end(), value.begin(), [](unsigned char c) {
        return static_cast<char>(std::tolower(c));
    });
    return value;
}

bool isWordByte(char value) {
    const auto c = static_cast<unsigned char>(value);
    return std::isalnum(c) != 0 || value == '_';
}

std::string previewOf(std::string_view line) {
    if (line.size() <= kSearchMaxPreviewBytes) return std::string(line);
    return std::string(line.substr(0, kSearchMaxPreviewBytes));
}

struct FileRecord {
    fs::path disk_path;
    std::string resource_path;
    uintmax_t size{0};
};

bool isValidUtf8Text(std::string_view value) {
    for (size_t i = 0; i < value.size();) {
        const auto first = static_cast<unsigned char>(value[i]);
        if (first == 0) return false;
        if (first < 0x80) {
            ++i;
            continue;
        }
        size_t continuation_count = 0;
        uint32_t codepoint = 0;
        if ((first & 0xE0u) == 0xC0u) {
            continuation_count = 1;
            codepoint = first & 0x1Fu;
            if (codepoint < 2) return false;
        } else if ((first & 0xF0u) == 0xE0u) {
            continuation_count = 2;
            codepoint = first & 0x0Fu;
        } else if ((first & 0xF8u) == 0xF0u) {
            continuation_count = 3;
            codepoint = first & 0x07u;
        } else {
            return false;
        }
        if (i + continuation_count >= value.size()) return false;
        for (size_t offset = 1; offset <= continuation_count; ++offset) {
            const auto next = static_cast<unsigned char>(value[i + offset]);
            if ((next & 0xC0u) != 0x80u) return false;
            codepoint = (codepoint << 6u) | (next & 0x3Fu);
        }
        if ((continuation_count == 2 && codepoint < 0x800u) ||
            (continuation_count == 3 && codepoint < 0x10000u) ||
            codepoint > 0x10FFFFu || (codepoint >= 0xD800u && codepoint <= 0xDFFFu)) {
            return false;
        }
        i += continuation_count + 1;
    }
    return true;
}

Result<std::string> readTextFile(const FileRecord& file) {
    std::ifstream input(file.disk_path, std::ios::binary);
    if (!input) return Error::internal("unreadable");
    std::string contents(static_cast<size_t>(file.size), '\0');
    input.read(contents.data(), static_cast<std::streamsize>(contents.size()));
    if (input.gcount() != static_cast<std::streamsize>(contents.size())) {
        return Error::internal("unreadable");
    }
    if (!isValidUtf8Text(contents)) return Error(415, "binary_or_invalid_utf8");
    return contents;
}

Result<std::vector<FileRecord>> collectFiles(const fs::path& root,
                                             const fs::path& search_root,
                                             const std::set<std::string>& extensions,
                                             SearchResponse& response) {
    std::vector<FileRecord> files;
    std::error_code ec;
    fs::recursive_directory_iterator iterator(search_root, fs::directory_options::skip_permission_denied, ec);
    if (ec) return Error::internal("Unable to traverse search_path");
    const fs::recursive_directory_iterator end;
    for (; iterator != end; iterator.increment(ec)) {
        if (ec) {
            ++response.skipped_files;
            ec.clear();
            continue;
        }
        const auto status = iterator->symlink_status(ec);
        if (ec) {
            ++response.skipped_files;
            ec.clear();
            continue;
        }
        if (fs::is_symlink(status)) {
            if (fs::is_directory(iterator->path(), ec)) iterator.disable_recursion_pending();
            ++response.skipped_files;
            ec.clear();
            continue;
        }
        if (fs::is_directory(status)) {
            if (kSkippedDirectories.count(iterator->path().filename().string())) {
                iterator.disable_recursion_pending();
            }
            continue;
        }
        if (!fs::is_regular_file(status)) continue;
        if (files.size() >= kSearchMaxFiles) {
            response.truncated = true;
            break;
        }
        auto extension = asciiFold(iterator->path().extension().string());
        if (!extensions.count(extension)) continue;
        const auto size = iterator->file_size(ec);
        if (ec || size > kSearchMaxFileBytes || response.scanned_bytes + size > kSearchMaxTotalBytes) {
            ++response.skipped_files;
            response.truncated = response.truncated || (!ec && response.scanned_bytes + size > kSearchMaxTotalBytes);
            ec.clear();
            continue;
        }
        const auto canonical = fs::canonical(iterator->path(), ec);
        if (ec || !isWithin(root, canonical)) {
            ++response.skipped_files;
            ec.clear();
            continue;
        }
        auto relative = fs::relative(canonical, root, ec);
        if (ec) {
            ++response.skipped_files;
            ec.clear();
            continue;
        }
        files.push_back({canonical, "res://" + relative.generic_string(), size});
        response.scanned_bytes += size;
    }
    std::sort(files.begin(), files.end(), [](const auto& left, const auto& right) {
        return left.resource_path < right.resource_path;
    });
    return files;
}

std::string maskGdscriptLine(std::string_view line) {
    std::string masked(line);
    bool single = false;
    bool double_quote = false;
    bool escaped = false;
    for (size_t i = 0; i < masked.size(); ++i) {
        const char c = masked[i];
        if (escaped) {
            masked[i] = ' ';
            escaped = false;
            continue;
        }
        if ((single || double_quote) && c == '\\') {
            masked[i] = ' ';
            escaped = true;
            continue;
        }
        if (!double_quote && c == '\'') {
            single = !single;
            masked[i] = ' ';
            continue;
        }
        if (!single && c == '"') {
            double_quote = !double_quote;
            masked[i] = ' ';
            continue;
        }
        if (!single && !double_quote && c == '#') {
            std::fill(masked.begin() + static_cast<std::ptrdiff_t>(i), masked.end(), ' ');
            break;
        }
        if (single || double_quote) masked[i] = ' ';
    }
    return masked;
}

std::string maskCSharpLine(std::string_view line, bool& block_comment) {
    std::string masked(line);
    bool string_literal = false;
    bool char_literal = false;
    bool escaped = false;
    for (size_t i = 0; i < masked.size(); ++i) {
        if (block_comment) {
            masked[i] = ' ';
            if (i + 1 < masked.size() && line[i] == '*' && line[i + 1] == '/') {
                masked[i + 1] = ' ';
                block_comment = false;
                ++i;
            }
            continue;
        }
        const char c = line[i];
        if (escaped) {
            masked[i] = ' ';
            escaped = false;
            continue;
        }
        if ((string_literal || char_literal) && c == '\\') {
            masked[i] = ' ';
            escaped = true;
            continue;
        }
        if (!char_literal && c == '"') {
            string_literal = !string_literal;
            masked[i] = ' ';
            continue;
        }
        if (!string_literal && c == '\'') {
            char_literal = !char_literal;
            masked[i] = ' ';
            continue;
        }
        if (string_literal || char_literal) {
            masked[i] = ' ';
            continue;
        }
        if (i + 1 < masked.size() && c == '/' && line[i + 1] == '/') {
            std::fill(masked.begin() + static_cast<std::ptrdiff_t>(i), masked.end(), ' ');
            break;
        }
        if (i + 1 < masked.size() && c == '/' && line[i + 1] == '*') {
            masked[i] = masked[i + 1] = ' ';
            block_comment = true;
            ++i;
        }
    }
    return masked;
}

struct IdentifierToken {
    std::string value;
    size_t offset{0};
};

std::vector<IdentifierToken> identifiers(std::string_view line) {
    std::vector<IdentifierToken> tokens;
    for (size_t i = 0; i < line.size();) {
        if (!(std::isalpha(static_cast<unsigned char>(line[i])) || line[i] == '_')) {
            ++i;
            continue;
        }
        const size_t start = i++;
        while (i < line.size() &&
               (std::isalnum(static_cast<unsigned char>(line[i])) || line[i] == '_')) ++i;
        tokens.push_back({std::string(line.substr(start, i - start)), start});
    }
    return tokens;
}

std::optional<std::pair<std::string, std::string>> csharpDeclaration(std::string_view line) {
    const auto tokens = identifiers(line);
    for (size_t i = 0; i + 1 < tokens.size(); ++i) {
        if (tokens[i].value == "class" || tokens[i].value == "struct" ||
            tokens[i].value == "interface") {
            return std::pair<std::string, std::string>{tokens[i + 1].value, "class"};
        }
        if (tokens[i].value == "enum") {
            return std::pair<std::string, std::string>{tokens[i + 1].value, "enum"};
        }
        if (tokens[i].value == "event") {
            return std::pair<std::string, std::string>{tokens.back().value, "signal"};
        }
    }
    const auto open_paren = line.find('(');
    if (open_paren != std::string_view::npos) {
        const auto before = identifiers(line.substr(0, open_paren));
        if (!before.empty()) {
            static const std::set<std::string> controls = {"if", "for", "foreach", "while", "switch", "catch", "using"};
            if (!controls.count(before.back().value)) {
                return std::pair<std::string, std::string>{before.back().value, "function"};
            }
        }
    }
    const auto brace = line.find('{');
    if (brace != std::string_view::npos &&
        (line.find("get", brace) != std::string_view::npos || line.find("set", brace) != std::string_view::npos)) {
        const auto before = identifiers(line.substr(0, brace));
        if (!before.empty()) return std::pair<std::string, std::string>{before.back().value, "variable"};
    }
    if (line.find(';') != std::string_view::npos && !tokens.empty() &&
        tokens.front().value != "using" && tokens.front().value != "namespace" &&
        tokens.front().value != "return") {
        const auto assignment = line.find('=');
        const auto before = identifiers(line.substr(0, assignment == std::string_view::npos ? line.find(';') : assignment));
        if (!before.empty()) {
            const bool constant = std::any_of(before.begin(), before.end(), [](const auto& token) {
                return token.value == "const";
            });
            return std::pair<std::string, std::string>{before.back().value, constant ? "constant" : "variable"};
        }
    }
    return std::nullopt;
}

std::optional<std::pair<std::string, std::string>> gdscriptDeclaration(std::string_view line) {
    const auto first = line.find_first_not_of(" \t");
    if (first == std::string_view::npos) return std::nullopt;
    line.remove_prefix(first);
    const std::pair<std::string_view, const char*> prefixes[] = {
        {"class_name ", "class"}, {"func ", "function"}, {"signal ", "signal"},
        {"var ", "variable"}, {"const ", "constant"}, {"enum ", "enum"}
    };
    for (const auto& [prefix, kind] : prefixes) {
        if (!strings::startsWith(line, prefix)) continue;
        line.remove_prefix(prefix.size());
        size_t length = 0;
        while (length < line.size() &&
               (std::isalnum(static_cast<unsigned char>(line[length])) || line[length] == '_')) {
            ++length;
        }
        if (length == 0) return std::nullopt;
        return std::pair<std::string, std::string>{std::string(line.substr(0, length)), kind};
    }
    return std::nullopt;
}

bool symbolMatches(const std::string& name, const SymbolSearchOptions& options) {
    const auto candidate = options.case_sensitive ? name : asciiFold(name);
    const auto query = options.case_sensitive ? options.query : asciiFold(options.query);
    switch (options.match) {
        case SymbolMatch::Exact: return candidate == query;
        case SymbolMatch::Prefix: return strings::startsWith(candidate, query);
        case SymbolMatch::Contains: return candidate.find(query) != std::string::npos;
    }
    return false;
}

bool allowedKind(const std::string& kind, const std::vector<std::string>& kinds) {
    return std::find(kinds.begin(), kinds.end(), kind) != kinds.end();
}

} // namespace

json SearchMatch::toJson() const {
    json value = {{"path", path}, {"line", line}, {"column", column}, {"preview", preview}};
    if (!name.empty()) value["name"] = name;
    if (!kind.empty()) value["kind"] = kind;
    if (!language.empty()) value["language"] = language;
    if (!container.empty()) value["container"] = container;
    return value;
}

json SearchDiagnostic::toJson() const { return {{"path", path}, {"reason", reason}}; }

json SearchResponse::toJson() const {
    json match_values = json::array();
    for (const auto& match : matches) match_values.push_back(match.toJson());
    json diagnostic_values = json::array();
    for (const auto& diagnostic : diagnostics) diagnostic_values.push_back(diagnostic.toJson());
    return {{"matches", std::move(match_values)}, {"diagnostics", std::move(diagnostic_values)},
            {"scanned_files", scanned_files}, {"skipped_files", skipped_files},
            {"scanned_bytes", scanned_bytes}, {"truncated", truncated},
            {"project_root", project_root}, {"search_kind", search_kind},
            {"lexical", lexical}, {"execution_mode", "offline_fallback"}};
}

ProjectSearch::ProjectSearch(fs::path project_root) {
    std::error_code ec;
    m_projectRoot = fs::weakly_canonical(std::move(project_root), ec);
    if (ec) m_projectRoot.clear();
}

Result<SearchResponse> ProjectSearch::searchText(const SearchOptions& options) const {
    if (m_projectRoot.empty()) return Error::invalidArgument("project root is unavailable");
    const auto valid = validateCommon(options);
    if (valid.isErr()) return valid.error();
    const auto extensions = validateExtensions(options.extensions);
    if (extensions.isErr()) return extensions.error();
    const auto search_root = resolveSearchRoot(m_projectRoot, options.search_path);
    if (search_root.isErr()) return search_root.error();

    SearchResponse response;
    response.project_root = m_projectRoot.string();
    response.search_kind = "text";
    const auto files = collectFiles(m_projectRoot, search_root.value(), extensions.value(), response);
    if (files.isErr()) return files.error();
    const auto query = options.case_sensitive ? options.query : asciiFold(options.query);
    for (const auto& file : files.value()) {
        const auto contents = readTextFile(file);
        if (contents.isErr()) {
            ++response.skipped_files;
            response.diagnostics.push_back({file.resource_path, contents.error().message});
            continue;
        }
        ++response.scanned_files;
        std::istringstream input(contents.value());
        std::string line;
        size_t line_number = 0;
        while (std::getline(input, line)) {
            ++line_number;
            if (!line.empty() && line.back() == '\r') line.pop_back();
            const auto haystack = options.case_sensitive ? line : asciiFold(line);
            size_t start = 0;
            while ((start = haystack.find(query, start)) != std::string::npos) {
                const bool left_ok = !options.whole_word || start == 0 || !isWordByte(haystack[start - 1]);
                const size_t after = start + query.size();
                const bool right_ok = !options.whole_word || after == haystack.size() || !isWordByte(haystack[after]);
                if (left_ok && right_ok) {
                    response.matches.push_back({file.resource_path, line_number, start + 1, previewOf(line)});
                    if (response.matches.size() >= options.max_results) {
                        response.truncated = true;
                        return response;
                    }
                }
                start += std::max<size_t>(1, query.size());
            }
        }
    }
    return response;
}

Result<SearchResponse> ProjectSearch::searchSymbols(const SymbolSearchOptions& options) const {
    if (m_projectRoot.empty()) return Error::invalidArgument("project root is unavailable");
    const auto valid = validateCommon(options);
    if (valid.isErr()) return valid.error();
    const auto extensions = validateExtensions(options.extensions);
    if (extensions.isErr()) return extensions.error();
    const auto search_root = resolveSearchRoot(m_projectRoot, options.search_path);
    if (search_root.isErr()) return search_root.error();

    SearchResponse response;
    response.project_root = m_projectRoot.string();
    response.search_kind = "symbols";
    response.lexical = true;
    const auto files = collectFiles(m_projectRoot, search_root.value(), extensions.value(), response);
    if (files.isErr()) return files.error();
    for (const auto& file : files.value()) {
        const bool gdscript = strings::endsWith(file.resource_path, ".gd");
        const bool csharp = strings::endsWith(file.resource_path, ".cs");
        if (!gdscript && !csharp) continue;
        const auto contents = readTextFile(file);
        if (contents.isErr()) {
            ++response.skipped_files;
            response.diagnostics.push_back({file.resource_path, contents.error().message});
            continue;
        }
        ++response.scanned_files;
        std::istringstream input(contents.value());
        std::string line;
        size_t line_number = 0;
        bool csharp_block_comment = false;
        while (std::getline(input, line)) {
            ++line_number;
            if (!line.empty() && line.back() == '\r') line.pop_back();
            const auto declaration = gdscript
                ? gdscriptDeclaration(maskGdscriptLine(line))
                : csharpDeclaration(maskCSharpLine(line, csharp_block_comment));
            if (!declaration || !allowedKind(declaration->second, options.kinds) ||
                !symbolMatches(declaration->first, options)) continue;
            const auto offset = line.find(declaration->first);
            response.matches.push_back({file.resource_path, line_number,
                                        offset == std::string::npos ? 1u : offset + 1,
                                        previewOf(line), declaration->first,
                                        declaration->second, gdscript ? "gdscript" : "csharp", {}});
            if (response.matches.size() >= options.max_results) {
                response.truncated = true;
                return response;
            }
        }
    }
    return response;
}

} // namespace didi::offline
