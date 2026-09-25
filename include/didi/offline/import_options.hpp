#pragma once

#include "didi/common/config_file_syntax.hpp"
#include "didi/common/json.hpp"
#include "didi/common/types.hpp"

#include <cstdint>
#include <filesystem>
#include <optional>
#include <string>
#include <vector>

namespace didi::offline {

// What asset_configure_import changes, and what resource_inspect reports,
// about an asset's .import sidecar (#958).
//
// Every rule here comes from a measurement recorded in the
// asset_configure_import amendment in docs/SURFACE_AMENDMENTS.md, made by
// tools/vibe/probes/import_config_engine.py on 4.5.1, 4.6.2 and 4.7.2. The
// engine checks no import option: a loop mode it has no name for, a loop end
// past the stream and a begin after the end all load without a word, and no
// bind an extension can call lists an importer's options or their ranges. So
// the checks are Didi's, and they cover only the options that were measured.

// The arguments, before any file is read.
struct ImportConfigureRequest {
    std::string asset_path;
    // A non-empty object. Its keys and values are checked against the sidecar
    // by planImportChanges, because which keys exist is the engine's answer.
    json options;
};

// 400 naming the argument in data.parameter.
Result<ImportConfigureRequest> parseImportConfigureRequest(const json& arguments);

// A sidecar as Godot's parser reads it.
struct ImportSidecar {
    std::string text;
    config_file::Scan scan;
    std::string importer;       // [remap] importer, empty when absent
    std::string resource_type;  // [remap] type
    std::string uid;            // [remap] uid, empty when absent
    std::optional<bool> valid;  // [remap] valid, which the engine writes only on a failure
};

// 422 with the line when the engine would refuse the file. It would then
// reimport the asset with every option at its default and mint a new uid, so a
// file in that state is repaired by hand rather than edited further.
Result<ImportSidecar> readImportSidecar(std::string text);

// The sidecar beside a source asset, read whole and capped at 1 MiB.
Result<std::string> readImportSidecarFile(const std::filesystem::path& sidecar);

// A [params] value as JSON: a boolean, an integer, a number or a string where
// the text is one of those, and the text itself for anything else.
json importParamValue(const std::string& value_text);

// The importers and keys asset_configure_import sets.
bool configurableImporter(const std::string& importer);
std::vector<std::string> configurableKeys(const std::string& importer);

// What the running engine says about the imported stream, for the checks that
// depend on the track itself.
struct ImportedStreamFacts {
    std::optional<double> length_seconds;
    std::optional<int64_t> mix_rate;
};

// One key to change.
struct ImportOptionChange {
    std::string key;
    json value;                 // the typed value written
    std::string text;           // as it is written into the sidecar
    json previous;              // importParamValue of the line it replaces
    std::string previous_text;
};

// Checks the options against the sidecar and, for the bounds that depend on
// the track, against the stream. 400 naming the key. Sorted by key.
Result<std::vector<ImportOptionChange>> planImportChanges(
    const ImportSidecar& sidecar, const json& options,
    const std::optional<ImportedStreamFacts>& stream);

// The sidecar text with only the named keys' lines replaced. Every other byte
// is kept, line endings included. 422 for a key that spans lines, shares its
// line with another key or appears twice: only a hand-edited file has one.
Result<std::string> applyImportChanges(const ImportSidecar& sidecar,
                                       const std::vector<ImportOptionChange>& changes);

// What differs, after the reimport, between what was asked and what the
// sidecar holds. Empty when every value held.
std::vector<std::string> sidecarDivergences(const ImportSidecar& after,
                                            const std::vector<ImportOptionChange>& changes,
                                            const std::string& uid_before);

// What differs between what was asked and what the stream loads as, from the
// `properties` object asset.readImportedStream reports.
std::vector<std::string> streamDivergences(const json& stream,
                                           const std::vector<ImportOptionChange>& changes);

// resource_inspect's `import` block for a sidecar's text.
json describeImportSidecar(const std::string& text);

} // namespace didi::offline
