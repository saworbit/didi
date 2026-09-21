#include "didi/offline/import_health.hpp"

#include "didi/common/config_file_syntax.hpp"
#include "didi/common/project_path.hpp"

#include <algorithm>
#include <cctype>
#include <cstdint>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <optional>
#include <set>
#include <string_view>
#include <utility>
#include <tuple>
#include <system_error>
#include <vector>

namespace didi::offline {
namespace {

namespace fs = std::filesystem;

struct ImportMetadata {
    std::string source;
    // Every path the sidecar names, for the existence checks. `[remap] path`
    // and `dest_files` overlap on an ordinary texture, so the same output
    // appears twice and the order means nothing.
    std::vector<std::string> outputs;
    // The `dest_files` values alone, in the order the sidecar declares them.
    // `dest_md5` is one digest over that vector concatenated, so neither the
    // set above nor any other order reproduces it.
    std::vector<std::string> dest_files;
};

// What the record Godot wrote for this asset says about the last import. Either
// half can be absent: a sidecar that declares no outputs is written with no
// `dest_md5`, and a record from an older engine may carry only the source.
struct RecordedDigests {
    std::optional<std::string> source;
    std::optional<std::string> destination;
};

struct ImportIssue {
    std::string metadata;
    std::string kind;
    std::string source;
    std::string target;
    // Why the engine refuses the file, and the line it refuses. Empty on every
    // finding that is about what the sidecar says rather than about whether it
    // parses, which is every other kind.
    std::string detail;
    int line{0};

    auto key() const { return std::tie(metadata, kind, target, source, detail, line); }
};

struct IssueLess {
    bool operator()(const ImportIssue& left, const ImportIssue& right) const {
        return left.key() < right.key();
    }
};

using ImportField = std::pair<std::string, std::string>;

struct ImportSections {
    std::vector<ImportField> remap;
    std::vector<ImportField> dependencies;
};

// What the engine refuses about this sidecar, or nothing when the scan proves
// nothing against it.
//
// A .import is a ConfigFile, so it is the two failures #817 and #820 named for
// project.godot, one file along. Neither was tested here: a file ending inside
// a value and a file whose value the parser will not start were both walked as
// though they had loaded, and the asset they describe was reported healthy.
//
// Measured on 4.5.1, 4.6.2 and 4.7.2. `compress/mode=)`, `path=res://a.ctex`
// unquoted and `metadata={` with no `}` are all `ERR_PARSE_ERROR`, and the
// editor's answer to one is worth stating: it prints the parse error, reimports
// the asset with the importer's defaults and rewrites the file. So the asset
// survives and everything the sidecar chose about it does not, including the
// uid every `uid://` reference in the project resolves through.
struct MetadataRefusal {
    std::string detail;
    int line{0};
};

std::optional<MetadataRefusal> parseRefusal(const config_file::Scan& scanned) {
    // Ends inside a value. The engine reads nothing from that value on, which
    // on a sidecar is usually [params] and always the tail of it.
    if (!scanned.complete) {
        MetadataRefusal refusal;
        refusal.detail =
            "This .import ends part-way through a value, so Godot answers ERR_PARSE_ERROR for "
            "it. The next reimport discards every setting the file holds, imports the asset "
            "with the importer's defaults and writes a new uid, so any uid:// reference to "
            "this asset stops resolving.";
        // The last key read is the one whose value never closed, which is the
        // line to repair.
        if (!scanned.entries.empty()) refusal.line = scanned.entries.back().line;
        return refusal;
    }
    // Balanced is not loadable. `compress/mode=)` closes every bracket it opens
    // and is still ERR_PARSE_ERROR, so counting brackets called it healthy.
    for (const auto& entry : scanned.entries) {
        const auto problem = config_file::valueProblem(entry.value_text);
        if (problem.empty()) continue;
        MetadataRefusal refusal;
        refusal.line = entry.line;
        refusal.detail = "Godot's parser refuses the value of \"" + entry.key +
                         "\" on line " + std::to_string(entry.line) + ", because " + problem +
                         ". It answers ERR_PARSE_ERROR for the file, so the next reimport "
                         "discards every setting the file holds, imports the asset with the "
                         "importer's defaults and writes a new uid, and any uid:// reference "
                         "to this asset stops resolving.";
        return refusal;
    }
    return std::nullopt;
}

// Read through the same rule as every other ConfigFile. Comparing a header as a
// whole line read `[ remap ]` as some other section, collected nothing under
// it, and reported a .import the engine loads without complaint as invalid
// metadata (#814).
ImportSections importSections(const config_file::Scan& scanned) {
    ImportSections sections;
    for (const auto& entry : scanned.entries) {
        if (entry.key.empty()) continue;
        if (entry.section == "remap") sections.remap.push_back({entry.key, entry.value_text});
        else if (entry.section == "deps") {
            sections.dependencies.push_back({entry.key, entry.value_text});
        }
    }
    return sections;
}

std::optional<std::string> readBounded(const fs::path& path,
                                       size_t limit = kMaxImportMetadataBytes) {
    std::ifstream input(path, std::ios::binary);
    if (!input) return std::nullopt;
    std::string contents(limit + 1, '\0');
    input.read(contents.data(), static_cast<std::streamsize>(contents.size()));
    const auto bytes_read = static_cast<size_t>(input.gcount());
    if (bytes_read > limit) return std::nullopt;
    contents.resize(bytes_read);
    return contents;
}

// MD5 (RFC 1321), for one purpose and no other.
//
// Godot records the digest of the source it imported in a `.md5` under the
// project data directory, and answering "does this asset need reimporting" the
// way the engine answers it means computing the same digest and comparing. The
// algorithm is the file format's, not a choice made here, and nothing about
// this is security: it reads a record the project already contains.
constexpr uint32_t kMd5Sine[64] = {
    0xd76aa478u, 0xe8c7b756u, 0x242070dbu, 0xc1bdceeeu,
    0xf57c0fafu, 0x4787c62au, 0xa8304613u, 0xfd469501u,
    0x698098d8u, 0x8b44f7afu, 0xffff5bb1u, 0x895cd7beu,
    0x6b901122u, 0xfd987193u, 0xa679438eu, 0x49b40821u,
    0xf61e2562u, 0xc040b340u, 0x265e5a51u, 0xe9b6c7aau,
    0xd62f105du, 0x02441453u, 0xd8a1e681u, 0xe7d3fbc8u,
    0x21e1cde6u, 0xc33707d6u, 0xf4d50d87u, 0x455a14edu,
    0xa9e3e905u, 0xfcefa3f8u, 0x676f02d9u, 0x8d2a4c8au,
    0xfffa3942u, 0x8771f681u, 0x6d9d6122u, 0xfde5380cu,
    0xa4beea44u, 0x4bdecfa9u, 0xf6bb4b60u, 0xbebfbc70u,
    0x289b7ec6u, 0xeaa127fau, 0xd4ef3085u, 0x04881d05u,
    0xd9d4d039u, 0xe6db99e5u, 0x1fa27cf8u, 0xc4ac5665u,
    0xf4292244u, 0x432aff97u, 0xab9423a7u, 0xfc93a039u,
    0x655b59c3u, 0x8f0ccc92u, 0xffeff47du, 0x85845dd1u,
    0x6fa87e4fu, 0xfe2ce6e0u, 0xa3014314u, 0x4e0811a1u,
    0xf7537e82u, 0xbd3af235u, 0x2ad7d2bbu, 0xeb86d391u};

constexpr int kMd5Shift[64] = {
    7, 12, 17, 22, 7, 12, 17, 22, 7, 12, 17, 22, 7, 12, 17, 22,
    5,  9, 14, 20, 5,  9, 14, 20, 5,  9, 14, 20, 5,  9, 14, 20,
    4, 11, 16, 23, 4, 11, 16, 23, 4, 11, 16, 23, 4, 11, 16, 23,
    6, 10, 15, 21, 6, 10, 15, 21, 6, 10, 15, 21, 6, 10, 15, 21};

class Md5Digest {
public:
    void update(const unsigned char* data, size_t length) {
        m_bits += static_cast<uint64_t>(length) * 8;
        absorb(data, length);
    }

    // Finishes the digest and returns it the way Godot writes it: lower-case
    // hex, little-endian within each state word.
    std::string hex() {
        const uint64_t bit_count = m_bits;
        // absorb() drains a full block, so m_pending is always below 64 and the
        // padding always fits. Taken modulo the block size so that is true by
        // construction rather than by argument.
        const size_t pending = m_pending % sizeof(m_block);
        unsigned char padding[sizeof(m_block)] = {0x80};
        absorb(padding, pending < 56 ? 56 - pending : sizeof(m_block) + 56 - pending);
        unsigned char length_bytes[8];
        for (int index = 0; index < 8; ++index) {
            length_bytes[index] = static_cast<unsigned char>((bit_count >> (8 * index)) & 0xffu);
        }
        absorb(length_bytes, sizeof(length_bytes));

        static constexpr char kHexDigits[] = "0123456789abcdef";
        std::string text;
        text.reserve(32);
        for (const uint32_t word : m_state) {
            for (int byte = 0; byte < 4; ++byte) {
                const auto value = static_cast<unsigned char>((word >> (8 * byte)) & 0xffu);
                text.push_back(kHexDigits[value >> 4]);
                text.push_back(kHexDigits[value & 0x0fu]);
            }
        }
        return text;
    }

private:
    // Feeds bytes through without counting them, so the padding the digest
    // appends does not change the length it encodes.
    void absorb(const unsigned char* data, size_t length) {
        while (length > 0) {
            const size_t taken = std::min(length, sizeof(m_block) - m_pending);
            std::memcpy(m_block + m_pending, data, taken);
            m_pending += taken;
            data += taken;
            length -= taken;
            if (m_pending == sizeof(m_block)) {
                transform();
                m_pending = 0;
            }
        }
    }

    void transform() {
        uint32_t words[16];
        for (int index = 0; index < 16; ++index) {
            words[index] = static_cast<uint32_t>(m_block[index * 4]) |
                           (static_cast<uint32_t>(m_block[index * 4 + 1]) << 8) |
                           (static_cast<uint32_t>(m_block[index * 4 + 2]) << 16) |
                           (static_cast<uint32_t>(m_block[index * 4 + 3]) << 24);
        }
        uint32_t a = m_state[0];
        uint32_t b = m_state[1];
        uint32_t c = m_state[2];
        uint32_t d = m_state[3];
        for (int round = 0; round < 64; ++round) {
            uint32_t mixed = 0;
            int word = 0;
            if (round < 16) {
                mixed = (b & c) | (~b & d);
                word = round;
            } else if (round < 32) {
                mixed = (d & b) | (~d & c);
                word = (5 * round + 1) % 16;
            } else if (round < 48) {
                mixed = b ^ c ^ d;
                word = (3 * round + 5) % 16;
            } else {
                mixed = c ^ (b | ~d);
                word = (7 * round) % 16;
            }
            const uint32_t sum = a + mixed + kMd5Sine[round] + words[word];
            const int shift = kMd5Shift[round];
            a = d;
            d = c;
            c = b;
            b += (sum << shift) | (sum >> (32 - shift));
        }
        m_state[0] += a;
        m_state[1] += b;
        m_state[2] += c;
        m_state[3] += d;
    }

    uint32_t m_state[4] = {0x67452301u, 0xefcdab89u, 0x98badcfeu, 0x10325476u};
    uint64_t m_bits = 0;
    unsigned char m_block[64] = {};
    size_t m_pending = 0;
};

std::string md5Hex(std::string_view text) {
    Md5Digest digest;
    digest.update(reinterpret_cast<const unsigned char*>(text.data()), text.size());
    return digest.hex();
}

// The digest of a file's bytes, or nothing when it cannot be read whole inside
// the budget.
std::optional<std::string> md5FileHex(const fs::path& path, size_t max_bytes) {
    std::error_code size_error;
    const auto size = fs::file_size(path, size_error);
    if (size_error || size > max_bytes) return std::nullopt;
    std::ifstream input(path, std::ios::binary);
    if (!input) return std::nullopt;
    Md5Digest digest;
    std::vector<char> chunk(64 * 1024);
    while (input.read(chunk.data(), static_cast<std::streamsize>(chunk.size())) || input.gcount() > 0) {
        digest.update(reinterpret_cast<const unsigned char*>(chunk.data()),
                      static_cast<size_t>(input.gcount()));
        if (!input) break;
    }
    if (input.bad()) return std::nullopt;
    return digest.hex();
}

// One digest over several files concatenated, which is what `dest_md5` is.
//
// `FileAccess::get_multiple_md5` opens each path in turn and feeds the bytes
// into a single MD5 context, so the answer depends on the order of the vector
// and there is no per-file digest to compare instead. Nothing is claimed
// unless every file is read whole inside the budget, because a partial
// concatenation hashes to something that is neither a match nor a real
// mismatch.
std::optional<std::string> md5FilesHex(const std::vector<fs::path>& paths, size_t max_bytes) {
    size_t remaining = max_bytes;
    Md5Digest digest;
    for (const auto& path : paths) {
        std::error_code size_error;
        const auto size = fs::file_size(path, size_error);
        if (size_error || size > remaining) return std::nullopt;
        remaining -= static_cast<size_t>(size);
        std::ifstream input(path, std::ios::binary);
        if (!input) return std::nullopt;
        std::vector<char> chunk(64 * 1024);
        while (input.read(chunk.data(), static_cast<std::streamsize>(chunk.size())) ||
               input.gcount() > 0) {
            digest.update(reinterpret_cast<const unsigned char*>(chunk.data()),
                          static_cast<size_t>(input.gcount()));
            if (!input) break;
        }
        if (input.bad()) return std::nullopt;
    }
    return digest.hex();
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

std::optional<ImportMetadata> parseMetadata(const config_file::Scan& scanned) {
    const auto sections = importSections(scanned);
    ImportMetadata metadata;
    for (const auto& field : sections.remap) {
        if (field.first == "valid" && field.second == "false") return std::nullopt;
        if (isPathKey(field.first)) {
            const auto output = quotedValue(field.second, true);
            if (!output) return std::nullopt;
            if (!output->empty()) metadata.outputs.push_back(*output);
        }
    }

    size_t source_assignments = 0;
    size_t destination_assignments = 0;
    for (const auto& field : sections.dependencies) {
        if (field.first == "source_file") {
            ++source_assignments;
            const auto source = quotedValue(field.second, false);
            if (!source) return std::nullopt;
            metadata.source = *source;
        } else if (field.first == "dest_files") {
            ++destination_assignments;
            const auto outputs = destinationValues(field.second);
            if (!outputs) return std::nullopt;
            metadata.outputs.insert(metadata.outputs.end(), outputs->begin(), outputs->end());
            metadata.dest_files = *outputs;
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

// The one directory Godot writes every import record into.
//
// `ResourceFormatImporter::get_import_base_path` joins
// `ProjectSettings::get_imported_files_path()` with the source's stem, and that
// path is the project data directory plus `imported`. It has nothing to do with
// where the outputs land. `csv_translation` writes its `.translation` files
// beside the CSV it imported and its record still goes here, so taking the
// directory from a declared output found the record for a texture and for
// nothing else, and every other importer fell through to the timestamps #827
// was filed about (#833).
//
// The data directory is `.godot`, or `godot` when
// `application/config/use_hidden_project_data_directory` is off. Measured both
// ways on all three lines: the setting moves the record and moves no output.
// The file is read through the shared ConfigFile scan rather than a regex
// because `config / use_hidden_project_data_directory = false` is the same
// setting to the engine, and it moves the directory too (#809).
//
// A project.godot that is missing, too large to read or unparseable leaves
// `.godot`, which is the engine's default. A project whose manifest does not
// load does not open at all, so there is no record under either name; what the
// readers of an unloadable manifest should say is #826.
fs::path importedDirectory(const fs::path& root) {
    std::string data_directory = ".godot";
    const auto manifest = root / "project.godot";
    std::error_code status_error;
    const auto status = fs::symlink_status(manifest, status_error);
    // A symlink is refused here for the reason the scan refuses one anywhere
    // else: it can leave the project. The answer is then the default, which is
    // the answer for every manifest this cannot read.
    if (status_error || fs::is_symlink(status) || !fs::is_regular_file(status)) {
        return root / data_directory / "imported";
    }
    if (const auto contents = readBounded(manifest, kMaxProjectManifestBytes)) {
        const auto scanned = config_file::scan(*contents);
        for (const auto& entry : scanned.entries) {
            if (entry.section != "application" ||
                entry.key != "config/use_hidden_project_data_directory") {
                continue;
            }
            // No break: a key the file declares twice is the last one to the
            // engine, so it is the last one here.
            // The rule lives in config_file, because the engine applies it to
            // every value it wants a bool from and this is one of two places
            // that has to reproduce it.
            data_directory = config_file::booleanize(entry.value_text) ? ".godot" : "godot";
        }
    }
    return root / data_directory / "imported";
}

// What Godot recorded when it last imported this asset, or nothing where it
// left no record to read.
//
// `ResourceFormatImporter::get_import_base_path` names the record
// `<imported dir>/<file name>-<md5 of the res:// path>.md5`, and
// `EditorFileSystem::_test_for_reimport` reads both `source_md5` and
// `dest_md5` out of it, so this is the file the engine consults and the name
// it consults it under. A symlink is refused here for the reason the scan
// refuses one anywhere else.
//
// Both halves come back from one read, because they are two keys in one file
// and the file is opened once per sidecar.
std::optional<RecordedDigests> recordedDigests(const fs::path& imported_directory,
                                               const std::string& source) {
    const auto separator = source.find_last_of('/');
    if (separator == std::string::npos || separator + 1 == source.size()) return std::nullopt;
    const auto record_name = source.substr(separator + 1) + "-" + md5Hex(source) + ".md5";
    fs::path record_path;
    try {
        record_path = imported_directory / paths::projectPathFromUtf8(record_name);
    } catch (const fs::filesystem_error&) {
        return std::nullopt;
    }
    std::error_code status_error;
    const auto status = fs::symlink_status(record_path, status_error);
    if (status_error || fs::is_symlink(status) || !fs::is_regular_file(status)) return std::nullopt;
    const auto contents = readBounded(record_path);
    if (!contents) return std::nullopt;
    const auto record = config_file::scan(*contents);
    if (!record.complete) return std::nullopt;
    RecordedDigests digests;
    for (const auto& entry : record.entries) {
        if (entry.key == "source_md5") {
            if (auto recorded = quotedValue(entry.value_text, false)) {
                digests.source = std::move(recorded);
            }
        } else if (entry.key == "dest_md5") {
            if (auto recorded = quotedValue(entry.value_text, false)) {
                digests.destination = std::move(recorded);
            }
        }
    }
    // A record with neither key is not a record.
    if (!digests.source && !digests.destination) return std::nullopt;
    return digests;
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

    // One per scan. The setting that names it is a property of the project, not
    // of any asset, so reading project.godot once per sidecar would ask the
    // same question of the same file for every import in the tree.
    const auto imported_directory = importedDirectory(root);

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
            // `.godot` stays on the list by name, because a project that has
            // since turned the hidden directory off still has the old one
            // sitting there. The comparison is what covers `godot`, which is
            // the generated directory for every project that turned it off and
            // is an ordinary folder name for every project that did not.
            if (name == ".git" || name == ".godot" || name == "build" ||
                strings::startsWith(name_text, "build-") || name == ".worktrees" ||
                name == ".vs" || name == "out" || name == "bin" ||
                entry.path() == imported_directory.parent_path()) {
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
        if (!contents) {
            metadata_issues.insert({metadata_path, "invalid_import_metadata", "", metadata_path});
            retainMetadataIssues();
            continue;
        }
        // Named for the file rather than `scanned`, which is the counter above.
        const auto sidecar = config_file::scan(*contents);
        // Asked before anything is read out of the scan, because a file the
        // engine will not parse describes nothing: its source, its outputs and
        // its uid are values the engine never got to. Reporting a missing
        // output for one of them is a finding about a file that does not load.
        if (const auto refused = parseRefusal(sidecar)) {
            metadata_issues.insert({metadata_path, "unparseable_import_metadata", "",
                                    metadata_path, refused->detail, refused->line});
            retainMetadataIssues();
            continue;
        }
        const auto parsed = parseMetadata(sidecar);
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

        // Godot writes its own answer to "does this need reimporting" into the
        // project data directory: a `.md5` holding the digest of the source it
        // imported. Modification times are not that answer. Git does not record
        // them, so after any clone, checkout or worktree the ordering of a
        // committed source and a committed output is whichever order the
        // checkout happened to write the two files in, and every imported asset
        // in the project reports as stale -- which is what this repository's own
        // demo did, three findings out of three, all false (#827).
        //
        // Read once per sidecar, because the record is about the source and
        // every output of one asset shares it.
        std::optional<RecordedDigests> recorded;
        std::optional<std::string> source_digest;
        if (source_exists) {
            recorded = recordedDigests(imported_directory, parsed->source);
            if (recorded && recorded->source) {
                // Asked before the file is opened, so that "too large to hash"
                // and "could not be read" stay apart. They arrived as the same
                // empty digest before, and the timestamp finding that followed
                // named a remedy that does nothing for the first of them
                // (#831).
                std::error_code size_error;
                const auto source_bytes = fs::file_size(*source_path, size_error);
                if (!size_error && source_bytes > kMaxImportSourceDigestBytes) {
                    metadata_issues.insert(
                        {metadata_path, "import_freshness_unchecked", parsed->source,
                         parsed->source,
                         "Godot recorded a digest for this source and it was not compared, "
                         "because the source is larger than the " +
                             std::to_string(kMaxImportSourceDigestBytes / (1024 * 1024)) +
                             " MiB this audit hashes. Nothing here says whether the asset is "
                             "current, and reimporting it will not change that."});
                } else {
                    source_digest = md5FileHex(*source_path, kMaxImportSourceDigestBytes);
                    if (!source_digest) {
                        metadata_issues.insert(
                            {metadata_path, "import_freshness_unchecked", parsed->source,
                             parsed->source,
                             "Godot recorded a digest for this source and it was not compared, "
                             "because the source could not be read through."});
                    }
                }
            }
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
            if (recorded && recorded->source) {
                // The engine's own comparison. A match is not a finding, and no
                // timestamp is read either way. Where the digest could not be
                // taken the asset is already reported as unchecked above, and a
                // timestamp answer is not offered instead: the record proves
                // the engine will compare digests, so the weaker signal would
                // be wrong for the same reason it was wrong in #827.
                if (source_digest && *source_digest != *recorded->source) {
                    metadata_issues.insert({metadata_path, "source_changed_since_import",
                                            parsed->source, output});
                }
                continue;
            }
            // No record to read. Timestamps are what is left, they are reported
            // under the name that says so, and the remedy is to open the
            // project in the editor once so a record exists.
            std::error_code source_time_error;
            std::error_code output_time_error;
            const auto source_time = fs::last_write_time(*source_path, source_time_error);
            const auto output_time = fs::last_write_time(*output_path, output_time_error);
            if (!source_time_error && !output_time_error && source_time > output_time) {
                metadata_issues.insert({metadata_path, "source_newer_than_output", parsed->source,
                                        output});
            }
        }

        // The other half of the same record (#830). A changed source is usually
        // deliberate and a changed output never is, so it is a finding of its
        // own rather than another source_changed_since_import.
        //
        // `dest_md5` is one digest over the `dest_files` values concatenated in
        // the order the sidecar declares them, so `outputs` cannot stand in for
        // them: it merges the `[remap] path` values in as well, and on an
        // ordinary texture that lists the same file twice.
        if (recorded && recorded->destination && !parsed->dest_files.empty()) {
            std::vector<fs::path> dest_paths;
            size_t dest_bytes = 0;
            bool readable = true;
            for (const auto& dest : parsed->dest_files) {
                const auto dest_path = resolveResourcePath(root, dest);
                std::error_code dest_error;
                if (!dest_path || !fs::is_regular_file(*dest_path, dest_error) || dest_error) {
                    // Already reported as a missing output or invalid metadata
                    // by the loop above. A digest over a set with a hole in it
                    // is neither a match nor a real mismatch.
                    readable = false;
                    break;
                }
                std::error_code size_error;
                const auto size = fs::file_size(*dest_path, size_error);
                if (size_error) {
                    readable = false;
                    break;
                }
                dest_bytes += static_cast<size_t>(size);
                dest_paths.push_back(*dest_path);
            }
            if (readable && dest_bytes > kMaxImportOutputDigestBytes) {
                metadata_issues.insert(
                    {metadata_path, "import_freshness_unchecked", parsed->source, parsed->source,
                     "Godot recorded a digest for the outputs of this import and it was not "
                     "compared, because they come to more than the " +
                         std::to_string(kMaxImportOutputDigestBytes / (1024 * 1024)) +
                         " MiB this audit hashes."});
            } else if (readable) {
                const auto dest_digest = md5FilesHex(dest_paths, kMaxImportOutputDigestBytes);
                if (!dest_digest) {
                    metadata_issues.insert(
                        {metadata_path, "import_freshness_unchecked", parsed->source,
                         parsed->source,
                         "Godot recorded a digest for the outputs of this import and it was not "
                         "compared, because one of them could not be read through."});
                } else if (*dest_digest != *recorded->destination) {
                    const auto count = parsed->dest_files.size();
                    const std::string subject =
                        count == 1 ? std::string("The file this import wrote no longer hashes")
                                   : "The " + std::to_string(count) +
                                         " files this import wrote no longer hash";
                    metadata_issues.insert(
                        {metadata_path, "output_changed_since_import", parsed->source,
                         parsed->source,
                         subject +
                             " to the dest_md5 Godot recorded, so the editor reimports the asset "
                             "the next time it scans the project without a warm cache. Nothing "
                             "edits an imported output on purpose, so this is a truncated write, "
                             "a bad merge or a partial checkout rather than a change to undo."});
                }
            }
        }
        retainMetadataIssues();
    }

    result["scanned_import_metadata"] = scanned;
    result["import_issue_count"] = issue_count;
    for (const auto& issue : retained_issues) {
        json finding = {
            {"metadata", issue.metadata},
            {"kind", issue.kind},
            {"source", issue.source},
            {"target", issue.target}
        };
        // A finding says what it knows. The parse refusal knows the line it
        // refuses; a freshness finding is about the whole file and has none,
        // so the two were published together until a second kind needed a
        // detail without one. `line: 0` would have been a line nobody can open.
        if (!issue.detail.empty()) finding["detail"] = issue.detail;
        if (issue.line > 0) finding["line"] = issue.line;
        result["import_issues"].push_back(std::move(finding));
    }
    if (truncated) result["import_scan_truncated"] = true;
    return result;
}

} // namespace didi::offline
