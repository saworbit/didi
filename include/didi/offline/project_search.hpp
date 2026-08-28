#pragma once

#include "didi/common/json.hpp"
#include "didi/common/types.hpp"

#include <cstddef>
#include <filesystem>
#include <optional>
#include <string>
#include <vector>

namespace didi::offline {

inline constexpr size_t kSearchMaxQueryBytes = 256;
inline constexpr size_t kSearchMaxResults = 500;
inline constexpr uintmax_t kSearchMaxFileBytes = 4u * 1024u * 1024u;
inline constexpr size_t kSearchMaxFiles = 10000;
inline constexpr uintmax_t kSearchMaxTotalBytes = 64u * 1024u * 1024u;
inline constexpr size_t kSearchMaxPreviewBytes = 1024;

struct SearchOptions {
    std::string query;
    std::string search_path{"res://"};
    std::vector<std::string> extensions{".gd", ".cs", ".tscn", ".tres"};
    bool case_sensitive{true};
    bool whole_word{false};
    size_t max_results{100};
};

enum class SymbolMatch { Exact, Prefix, Contains };

struct SymbolSearchOptions : SearchOptions {
    SymbolMatch match{SymbolMatch::Prefix};
    std::vector<std::string> kinds{"class", "function", "signal", "variable", "constant", "enum"};
};

struct SearchMatch {
    std::string path;
    size_t line{0};
    size_t column{0};
    std::string preview;
    std::string name;
    std::string kind;
    std::string language;
    std::string container;

    json toJson() const;
};

struct SearchDiagnostic {
    std::string path;
    std::string reason;

    json toJson() const;
};

struct SearchResponse {
    std::vector<SearchMatch> matches;
    std::vector<SearchDiagnostic> diagnostics;
    size_t scanned_files{0};
    size_t skipped_files{0};
    uintmax_t scanned_bytes{0};
    bool truncated{false};
    std::string project_root;
    std::string search_kind;
    bool lexical{false};

    json toJson() const;
};

class ProjectSearch {
public:
    explicit ProjectSearch(std::filesystem::path project_root);

    Result<SearchResponse> searchText(const SearchOptions& options) const;
    Result<SearchResponse> searchSymbols(const SymbolSearchOptions& options) const;

private:
    std::filesystem::path m_projectRoot;
};

} // namespace didi::offline
