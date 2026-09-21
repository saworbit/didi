#include "didi/offline/import_health.hpp"

#include <chrono>
#include <filesystem>
#include <fstream>
#include <functional>
#include <stdexcept>
#include <string>

#define ASSERT_TRUE(cond) if (!(cond)) throw std::runtime_error("Assertion failed: " #cond);
#define ASSERT_EQ(a, b) ASSERT_TRUE((a) == (b))

void registerTest(const std::string& name, std::function<void()> fn);

namespace {

class ImportHealthFixture {
public:
    explicit ImportHealthFixture(const std::string& suffix) {
        m_root = std::filesystem::temp_directory_path() /
                 ("didi-import-health-" + suffix + "-" +
                  std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()));
        std::filesystem::create_directories(m_root);
    }

    ~ImportHealthFixture() {
        std::error_code ignored;
        std::filesystem::remove_all(m_root, ignored);
    }

    std::filesystem::path write(const std::string& relative, const std::string& contents) {
        const auto target = m_root / std::filesystem::path(relative);
        std::filesystem::create_directories(target.parent_path());
        std::ofstream(target, std::ios::binary) << contents;
        return target;
    }

    const std::filesystem::path& root() const { return m_root; }

private:
    std::filesystem::path m_root;
};

// The kind reported against one sidecar. Findings are ordered by their whole
// key, so looking one up by the file it is about says what is being asserted.
std::string kindFor(const didi::json& report, const std::string& metadata_path) {
    for (const auto& issue : report["import_issues"]) {
        if (issue["metadata"] == metadata_path) return issue["kind"].get<std::string>();
    }
    return {};
}

std::string metadata(const std::string& source, const std::string& output) {
    return "[remap]\n"
           "importer=\"texture\"\n"
           "type=\"CompressedTexture2D\"\n"
           "path=\"" + output + "\"\n\n"
           "[deps]\n"
           "source_file=\"" + source + "\"\n"
           "dest_files=[\"" + output + "\"]\n";
}

void test_healthy_import_metadata_has_no_issues() {
    ImportHealthFixture fixture("healthy");
    const auto source = fixture.write("art/icon.png", "source");
    const auto output = fixture.write(".godot/imported/icon.ctex", "output");
    fixture.write("art/icon.png.import",
                  metadata("res://art/icon.png", "res://.godot/imported/icon.ctex"));
    const auto now = std::filesystem::file_time_type::clock::now();
    std::filesystem::last_write_time(source, now - std::chrono::hours(2));
    std::filesystem::last_write_time(output, now - std::chrono::hours(1));

    const auto report = didi::offline::inspectImportHealth(fixture.root().string(), 500);

    ASSERT_EQ(report["scanned_import_metadata"], 1u);
    ASSERT_EQ(report["import_issue_count"], 0u);
    ASSERT_TRUE(report["import_issues"].empty());
}

void test_missing_source_is_reported() {
    ImportHealthFixture fixture("missing-source");
    fixture.write(".godot/imported/icon.ctex", "output");
    fixture.write("art/icon.png.import",
                  metadata("res://art/icon.png", "res://.godot/imported/icon.ctex"));

    const auto report = didi::offline::inspectImportHealth(fixture.root().string(), 500);

    ASSERT_EQ(report["import_issue_count"], 1u);
    ASSERT_EQ(report["import_issues"][0]["kind"], "missing_import_source");
    ASSERT_EQ(report["import_issues"][0]["metadata"], "res://art/icon.png.import");
    ASSERT_EQ(report["import_issues"][0]["source"], "res://art/icon.png");
    ASSERT_EQ(report["import_issues"][0]["target"], "res://art/icon.png");
}

void test_missing_output_is_deduplicated() {
    ImportHealthFixture fixture("missing-output");
    fixture.write("art/icon.png", "source");
    fixture.write("art/icon.png.import",
                  metadata("res://art/icon.png", "res://.godot/imported/icon.ctex"));

    const auto report = didi::offline::inspectImportHealth(fixture.root().string(), 500);

    ASSERT_EQ(report["import_issue_count"], 1u);
    ASSERT_EQ(report["import_issues"][0]["kind"], "missing_import_output");
    ASSERT_EQ(report["import_issues"][0]["target"], "res://.godot/imported/icon.ctex");
}

void test_newer_source_is_reported_as_timestamp_evidence() {
    ImportHealthFixture fixture("newer-source");
    const auto source = fixture.write("art/icon.png", "source");
    const auto output = fixture.write(".godot/imported/icon.ctex", "output");
    fixture.write("art/icon.png.import",
                  metadata("res://art/icon.png", "res://.godot/imported/icon.ctex"));
    const auto now = std::filesystem::file_time_type::clock::now();
    std::filesystem::last_write_time(output, now - std::chrono::hours(2));
    std::filesystem::last_write_time(source, now - std::chrono::hours(1));

    const auto report = didi::offline::inspectImportHealth(fixture.root().string(), 500);

    ASSERT_EQ(report["import_issue_count"], 1u);
    ASSERT_EQ(report["import_issues"][0]["kind"], "source_newer_than_output");
    ASSERT_EQ(report["import_issues"][0]["source"], "res://art/icon.png");
    ASSERT_EQ(report["import_issues"][0]["target"], "res://.godot/imported/icon.ctex");
}

void test_malformed_and_oversized_metadata_fail_closed() {
    ImportHealthFixture fixture("malformed");
    fixture.write("art/icon.png", "source");
    fixture.write("art/missing-source.png.import",
                  "[remap]\npath=\"res://.godot/imported/missing.ctex\"\n");
    fixture.write("art/malformed-output.png.import",
                  "[deps]\nsource_file=\"res://art/icon.png\"\n"
                  "dest_files=[res://.godot/imported/icon.ctex]\n");
    fixture.write("art/oversized.png.import",
                  std::string(didi::offline::kMaxImportMetadataBytes + 1, 'x'));

    const auto report = didi::offline::inspectImportHealth(fixture.root().string(), 500);

    ASSERT_EQ(report["scanned_import_metadata"], 3u);
    ASSERT_EQ(report["import_issue_count"], 3u);
    // A sidecar with no source_file and one too large to read are both files
    // that say the wrong thing about an asset.
    ASSERT_EQ(kindFor(report, "res://art/missing-source.png.import"), "invalid_import_metadata");
    ASSERT_EQ(kindFor(report, "res://art/oversized.png.import"), "invalid_import_metadata");
    // An unquoted res:// path inside dest_files is the separate state: `res` is
    // not a value Godot has, so ConfigFile.load answers ERR_PARSE_ERROR for the
    // whole file on 4.5.1, 4.6.2 and 4.7.2 rather than for that one field
    // (#823). The remedy differs too -- the next reimport throws this file away
    // and writes a new uid, which no other finding here implies.
    ASSERT_EQ(kindFor(report, "res://art/malformed-output.png.import"),
              "unparseable_import_metadata");
}

void test_unsafe_resource_paths_fail_closed() {
    ImportHealthFixture fixture("unsafe-paths");
    fixture.write("art/icon.png", "source");
    fixture.write("art/traversal.png.import",
                  metadata("res://art/icon.png", "res://../outside.ctex"));
    fixture.write("art/backslash.png.import",
                  metadata("res://art/icon.png", "res://.godot\\imported\\icon.ctex"));

    const auto report = didi::offline::inspectImportHealth(fixture.root().string(), 500);

    ASSERT_EQ(report["import_issue_count"], 2u);
    for (const auto& issue : report["import_issues"]) {
        ASSERT_EQ(issue["kind"], "invalid_import_metadata");
    }
}

void test_findings_are_sorted_capped_and_counted_before_cap() {
    ImportHealthFixture fixture("ordering");
    fixture.write("art/a.png", "a");
    fixture.write("art/b.png", "b");
    fixture.write("z.png.import",
                  metadata("res://art/b.png", "res://.godot/imported/z.ctex"));
    fixture.write("a.png.import",
                  metadata("res://art/a.png", "res://.godot/imported/a.ctex"));

    const auto report = didi::offline::inspectImportHealth(fixture.root().string(), 1);

    ASSERT_EQ(report["import_issue_count"], 2u);
    ASSERT_EQ(report["import_issues"].size(), 1u);
    ASSERT_EQ(report["import_issues"][0]["metadata"], "res://a.png.import");
}

void test_symlinked_metadata_and_output_are_not_followed() {
    ImportHealthFixture fixture("symlinks");
    const auto source = fixture.write("art/icon.png", "source");
    (void)source;
    const auto real_metadata = fixture.write(
        "metadata.txt", metadata("res://art/icon.png", "res://.godot/imported/icon.ctex"));
    std::filesystem::create_directories(fixture.root() / "art");
    std::error_code metadata_link_error;
    std::filesystem::create_symlink(real_metadata, fixture.root() / "art/icon.png.import",
                                    metadata_link_error);
    if (!metadata_link_error) {
        const auto skipped = didi::offline::inspectImportHealth(fixture.root().string(), 500);
        ASSERT_EQ(skipped["scanned_import_metadata"], 0u);
    }

    const auto outside = fixture.write("outside.ctex", "output");
    std::filesystem::create_directories(fixture.root() / ".godot/imported");
    std::error_code output_link_error;
    std::filesystem::create_symlink(outside, fixture.root() / ".godot/imported/icon.ctex",
                                    output_link_error);
    if (!output_link_error) {
        fixture.write("art/direct.png.import",
                      metadata("res://art/icon.png", "res://.godot/imported/icon.ctex"));
        const auto report = didi::offline::inspectImportHealth(fixture.root().string(), 500);
        ASSERT_EQ(report["import_issue_count"], 1u);
        ASSERT_EQ(report["import_issues"][0]["kind"], "invalid_import_metadata");
    }
}

void test_invalid_flag_malformed_path_and_source_mismatch_fail_closed() {
    ImportHealthFixture fixture("invalid-contract");
    fixture.write("art/icon.png", "source");
    fixture.write("art/other.png", "source");
    fixture.write("art/invalid.png.import",
                  "[remap]\nvalid=false\n"
                  "path=\"res://.godot/imported/invalid.ctex\"\n\n"
                  "[deps]\nsource_file=\"res://art/invalid.png\"\n"
                  "dest_files=[\"res://.godot/imported/invalid.ctex\"]\n");
    fixture.write("art/malformed-path.png.import",
                  "[remap]\npath=res://.godot/imported/icon.ctex\n\n"
                  "[deps]\nsource_file=\"res://art/malformed-path.png\"\n"
                  "dest_files=[]\n");
    fixture.write("art/icon.png.import",
                  metadata("res://art/other.png", "res://.godot/imported/icon.ctex"));

    const auto report = didi::offline::inspectImportHealth(fixture.root().string(), 500);

    ASSERT_EQ(report["import_issue_count"], 3u);
    ASSERT_EQ(kindFor(report, "res://art/invalid.png.import"), "invalid_import_metadata");
    ASSERT_EQ(kindFor(report, "res://art/icon.png.import"), "invalid_import_metadata");
    // `path=res://...` with no quotes is the same ERR_PARSE_ERROR as an
    // unquoted path inside an array, so the file does not load at all (#823).
    ASSERT_EQ(kindFor(report, "res://art/malformed-path.png.import"),
              "unparseable_import_metadata");
}

void test_comment_decoys_are_ignored() {
    ImportHealthFixture fixture("comments");
    const auto source = fixture.write("art/icon.png", "source");
    const auto output = fixture.write(".godot/imported/icon.ctex", "output");
    fixture.write("art/icon.png.import",
                  "; source_file=\"res://outside.png\"\n"
                  "# dest_files=[\"res://outside.ctex\"]\n" +
                      metadata("res://art/icon.png", "res://.godot/imported/icon.ctex"));
    const auto now = std::filesystem::file_time_type::clock::now();
    std::filesystem::last_write_time(source, now - std::chrono::hours(2));
    std::filesystem::last_write_time(output, now - std::chrono::hours(1));

    const auto report = didi::offline::inspectImportHealth(fixture.root().string(), 500);

    ASSERT_EQ(report["import_issue_count"], 0u);
}

void test_generated_directories_are_not_metadata_sources() {
    ImportHealthFixture fixture("generated-dirs");
    fixture.write(".godot/imported/noise.import", "not metadata");
    fixture.write("build-ninja/noise.import", "not metadata");
    fixture.write("art/icon.png", "source");
    fixture.write("art/icon.png.import", "[deps]\nsource_file=\"res://art/icon.png\"\n");

    const auto report = didi::offline::inspectImportHealth(fixture.root().string(), 500);

    ASSERT_EQ(report["scanned_import_metadata"], 1u);
    ASSERT_EQ(report["import_issue_count"], 0u);
}

void test_output_path_amplification_fails_as_one_invalid_metadata_issue() {
    ImportHealthFixture fixture("path-cap");
    fixture.write("art/icon.png", "source");
    std::string contents = "[deps]\nsource_file=\"res://art/icon.png\"\ndest_files=[";
    for (size_t index = 0; index <= didi::offline::kMaxImportPathsPerMetadata; ++index) {
        if (index != 0) contents += ',';
        contents += "\"res://.godot/imported/" + std::to_string(index) + ".ctex\"";
    }
    contents += "]\n";
    fixture.write("art/icon.png.import", contents);

    const auto report = didi::offline::inspectImportHealth(fixture.root().string(), 500);

    ASSERT_EQ(report["import_issue_count"], 1u);
    ASSERT_EQ(report["import_issues"].size(), 1u);
    ASSERT_EQ(report["import_issues"][0]["kind"], "invalid_import_metadata");
}

void test_importer_params_cannot_impersonate_remap_or_dependency_fields() {
    ImportHealthFixture fixture("section-scope");
    const auto source = fixture.write("art/icon.png", "source");
    const auto output = fixture.write(".godot/imported/icon.ctex", "output");
    fixture.write("art/icon.png.import",
                  metadata("res://art/icon.png", "res://.godot/imported/icon.ctex") +
                      "\n[params]\n"
                      "path=\"res://outside.ctex\"\n"
                      "valid=false\n"
                      "source_file=\"res://outside.png\"\n"
                      "dest_files=[\"res://outside.ctex\"]\n");
    const auto now = std::filesystem::file_time_type::clock::now();
    std::filesystem::last_write_time(source, now - std::chrono::hours(2));
    std::filesystem::last_write_time(output, now - std::chrono::hours(1));

    const auto report = didi::offline::inspectImportHealth(fixture.root().string(), 500);

    ASSERT_EQ(report["import_issue_count"], 0u);
}

void test_empty_remap_path_is_valid_for_importers_without_outputs() {
    ImportHealthFixture fixture("empty-path");
    fixture.write("data/source.csv", "source");
    fixture.write("data/source.csv.import",
                  "[remap]\nimporter=\"keep\"\ntype=\"\"\npath=\"\"\n\n"
                  "[deps]\nsource_file=\"res://data/source.csv\"\ndest_files=[]\n");

    const auto report = didi::offline::inspectImportHealth(fixture.root().string(), 500);

    ASSERT_EQ(report["import_issue_count"], 0u);
}

void test_multisegment_feature_path_is_checked() {
    ImportHealthFixture fixture("feature-path");
    fixture.write("art/icon.png", "source");
    fixture.write("art/icon.png.import",
                  "[remap]\n"
                  "path.etc2.mobile=\"res://.godot/imported/icon.mobile.ctex\"\n\n"
                  "[deps]\nsource_file=\"res://art/icon.png\"\ndest_files=[]\n");

    const auto report = didi::offline::inspectImportHealth(fixture.root().string(), 500);

    ASSERT_EQ(report["import_issue_count"], 1u);
    ASSERT_EQ(report["import_issues"][0]["kind"], "missing_import_output");
    ASSERT_EQ(report["import_issues"][0]["target"],
              "res://.godot/imported/icon.mobile.ctex");
}

void test_a_spaced_section_header_is_still_remap_and_deps() {
    // Godot trims a section name, so `[ remap ]` is the remap section and the
    // file loads without complaint. Comparing the header as a whole line
    // collected nothing under either section, parseMetadata saw no source_file,
    // and a valid .import was reported as invalid metadata (#814).
    ImportHealthFixture fixture("spaced-header");
    const auto source = fixture.write("art/icon.png", "source");
    fixture.write(".godot/imported/icon.ctex", "output");
    fixture.write("art/icon.png.import",
                  "[ remap ]\n"
                  "importer=\"texture\"\n"
                  "path=\"res://.godot/imported/icon.ctex\"\n\n"
                  "[ deps ]\n"
                  "source_file=\"res://art/icon.png\"\n"
                  "dest_files=[\"res://.godot/imported/icon.ctex\"]\n");
    (void)source;
    const auto report = didi::offline::inspectImportHealth(fixture.root().string(), 50);
    ASSERT_EQ(report["scanned_import_metadata"].get<size_t>(), 1u);
    ASSERT_EQ(report["import_issue_count"].get<size_t>(), 0u);
}

void test_a_trailing_comment_is_not_part_of_a_dependency_path() {
    // A `; note` on a line is a comment for Godot, so this .import loads
    // without complaint. Carrying it into the value left a string that no
    // longer ended in a quote, quotedValue gave up, and the audit reported
    // invalid_import_metadata against a healthy file (#816).
    ImportHealthFixture fixture("trailing-comment");
    fixture.write("art/icon.png", "source");
    fixture.write(".godot/imported/icon.ctex", "output");
    fixture.write("art/icon.png.import",
                  "[remap]\n"
                  "importer=\"texture\"\n"
                  "path=\"res://.godot/imported/icon.ctex\"\n\n"
                  "[deps]\n"
                  "source_file=\"res://art/icon.png\" ; keep this\n"
                  "dest_files=[\"res://.godot/imported/icon.ctex\"]\n");
    const auto report = didi::offline::inspectImportHealth(fixture.root().string(), 50);
    ASSERT_EQ(report["scanned_import_metadata"].get<size_t>(), 1u);
    ASSERT_EQ(report["import_issue_count"].get<size_t>(), 0u);
}

void test_a_value_the_parser_refuses_makes_the_sidecar_unparseable() {
    // `compress/mode=)` closes every bracket it opens, so counting brackets
    // called this file complete and the audit answered import_issue_count: 0
    // for a sidecar Godot answers ERR_PARSE_ERROR for (#823). Everything else
    // about the file is healthy -- the source is there, the output is there and
    // the output is newer -- so the finding can only come from the value.
    ImportHealthFixture fixture("unloadable-value");
    const auto source = fixture.write("art/icon.png", "source");
    const auto output = fixture.write(".godot/imported/icon.ctex", "output");
    fixture.write("art/icon.png.import",
                  metadata("res://art/icon.png", "res://.godot/imported/icon.ctex") +
                      "\n[params]\n"
                      "compress/mode=)\n");
    const auto now = std::filesystem::file_time_type::clock::now();
    std::filesystem::last_write_time(source, now - std::chrono::hours(2));
    std::filesystem::last_write_time(output, now - std::chrono::hours(1));

    const auto report = didi::offline::inspectImportHealth(fixture.root().string(), 500);

    ASSERT_EQ(report["scanned_import_metadata"], 1u);
    ASSERT_EQ(report["import_issue_count"], 1u);
    ASSERT_EQ(report["import_issues"][0]["kind"], "unparseable_import_metadata");
    ASSERT_EQ(report["import_issues"][0]["metadata"], "res://art/icon.png.import");
    // The line and the reason, because the remedy is to repair one line and
    // nothing else in the report says which.
    ASSERT_EQ(report["import_issues"][0]["line"], 11);
    ASSERT_TRUE(report["import_issues"][0]["detail"].get<std::string>().find("compress/mode") !=
                std::string::npos);
}

void test_a_sidecar_that_ends_inside_a_value_is_unparseable() {
    // #817's failure in a reader #817 did not reach. The file ends part-way
    // through the metadata dictionary every texture sidecar carries, which is
    // ERR_PARSE_ERROR on 4.5.1, 4.6.2 and 4.7.2, and it was walked as though it
    // had loaded.
    ImportHealthFixture fixture("truncated-value");
    const auto source = fixture.write("art/icon.png", "source");
    const auto output = fixture.write(".godot/imported/icon.ctex", "output");
    fixture.write("art/icon.png.import",
                  metadata("res://art/icon.png", "res://.godot/imported/icon.ctex") +
                      "metadata={\n\"vram_texture\": false\n");
    const auto now = std::filesystem::file_time_type::clock::now();
    std::filesystem::last_write_time(source, now - std::chrono::hours(2));
    std::filesystem::last_write_time(output, now - std::chrono::hours(1));

    const auto report = didi::offline::inspectImportHealth(fixture.root().string(), 500);

    ASSERT_EQ(report["import_issue_count"], 1u);
    ASSERT_EQ(report["import_issues"][0]["kind"], "unparseable_import_metadata");
    ASSERT_TRUE(report["import_issues"][0]["detail"].get<std::string>().find(
                    "ends part-way through a value") != std::string::npos);
}

void test_a_healthy_sidecar_still_publishes_no_detail() {
    // The parse check has to be provable-only, or it refuses files Godot loads.
    // Every value a real editor-written texture sidecar carries is here,
    // including the multi-line metadata dictionary and the empty containers,
    // and all of them load on 4.5.1, 4.6.2 and 4.7.2.
    ImportHealthFixture fixture("real-shapes");
    const auto source = fixture.write("art/icon.svg", "source");
    const auto output = fixture.write(".godot/imported/icon.ctex", "output");
    fixture.write("art/icon.svg.import",
                  "[remap]\n\n"
                  "importer=\"texture\"\n"
                  "type=\"CompressedTexture2D\"\n"
                  "uid=\"uid://bd2j02c8rf0m8\"\n"
                  "path=\"res://.godot/imported/icon.ctex\"\n"
                  "metadata={\n\"vram_texture\": false\n}\n\n"
                  "[deps]\n\n"
                  "source_file=\"res://art/icon.svg\"\n"
                  "dest_files=[\"res://.godot/imported/icon.ctex\"]\n\n"
                  "[params]\n\n"
                  "compress/mode=0\n"
                  "compress/lossy_quality=0.7\n"
                  "mipmaps/limit=-1\n"
                  "roughness/src_normal=\"\"\n"
                  "process/fix_alpha_border=true\n"
                  "svg/scale=1.0\n"
                  "_subresources={}\n"
                  "nodes/root_type=\"\"\n");
    const auto now = std::filesystem::file_time_type::clock::now();
    std::filesystem::last_write_time(source, now - std::chrono::hours(2));
    std::filesystem::last_write_time(output, now - std::chrono::hours(1));

    const auto report = didi::offline::inspectImportHealth(fixture.root().string(), 500);

    ASSERT_EQ(report["scanned_import_metadata"], 1u);
    ASSERT_EQ(report["import_issue_count"], 0u);
}

// The record Godot writes beside an imported output, for a source at
// res://art/icon.png. The stem is the engine's own:
// `<file name>-<md5 of the res:// path>`, and md5("res://art/icon.png") is
// c3d958876317c5b9ff9d73fef1b6123f. Both digests below are written out rather
// than computed here, so the test disagrees with the implementation if the
// implementation's MD5 is wrong.
const char* const kIconRecordPath =
    ".godot/imported/icon.png-c3d958876317c5b9ff9d73fef1b6123f.md5";
// md5("source"), which is what the fixtures write into art/icon.png.
const char* const kSourceDigest = "36cd38f49b9afa08222c0dc9ebfe35eb";
// md5("output"), which is what the fixtures write into every single-output
// .ctex. `dest_md5` is one digest over the dest_files concatenated, so for one
// output it is the digest of that file's bytes. Measured against Godot on this
// repository's own demo/: the record beside didi_mark.svg's .ctex carries the
// md5 of that .ctex and nothing else.
const char* const kOutputDigest = "78e6221f6393d1356681db398f14ce6d";

std::string record(const std::string& source_md5, const std::string& dest_md5) {
    return "source_md5=\"" + source_md5 + "\"\ndest_md5=\"" + dest_md5 + "\"\n";
}

void test_the_recorded_digest_answers_before_the_timestamps() {
    // #827: git does not carry mtimes, so after a clone the source is often
    // written after the output and every committed import reports as stale.
    // Godot's own record says the asset is current, and it is read first.
    ImportHealthFixture fixture("digest-match");
    const auto source = fixture.write("art/icon.png", "source");
    const auto output = fixture.write(".godot/imported/icon.ctex", "output");
    fixture.write(kIconRecordPath, record(kSourceDigest, kOutputDigest));
    fixture.write("art/icon.png.import",
                  metadata("res://art/icon.png", "res://.godot/imported/icon.ctex"));
    const auto now = std::filesystem::file_time_type::clock::now();
    std::filesystem::last_write_time(output, now - std::chrono::hours(2));
    std::filesystem::last_write_time(source, now - std::chrono::hours(1));

    const auto report = didi::offline::inspectImportHealth(fixture.root().string(), 500);

    ASSERT_EQ(report["scanned_import_metadata"], 1u);
    ASSERT_EQ(report["import_issue_count"], 0u);
    ASSERT_TRUE(report["import_issues"].empty());
}

void test_a_source_that_does_not_match_the_record_is_reported() {
    // The mirror image, and the one the timestamps get wrong in the other
    // direction: the source was edited and written back with an older
    // modification time than the output, so only the digest catches it.
    ImportHealthFixture fixture("digest-mismatch");
    const auto source = fixture.write("art/icon.png", "changed source");
    const auto output = fixture.write(".godot/imported/icon.ctex", "output");
    fixture.write(kIconRecordPath, record(kSourceDigest, kOutputDigest));
    fixture.write("art/icon.png.import",
                  metadata("res://art/icon.png", "res://.godot/imported/icon.ctex"));
    const auto now = std::filesystem::file_time_type::clock::now();
    std::filesystem::last_write_time(source, now - std::chrono::hours(2));
    std::filesystem::last_write_time(output, now - std::chrono::hours(1));

    const auto report = didi::offline::inspectImportHealth(fixture.root().string(), 500);

    ASSERT_EQ(report["import_issue_count"], 1u);
    ASSERT_EQ(report["import_issues"][0]["kind"], "source_changed_since_import");
    ASSERT_EQ(report["import_issues"][0]["source"], "res://art/icon.png");
    ASSERT_EQ(report["import_issues"][0]["target"], "res://.godot/imported/icon.ctex");
    // The engine's comparison is not a parse refusal, so it carries neither.
    ASSERT_TRUE(!report["import_issues"][0].contains("detail"));
    ASSERT_TRUE(!report["import_issues"][0].contains("line"));
}

void test_a_record_under_the_wrong_name_is_not_read() {
    // The stem is md5 of the res:// path, not of the file name, and a record
    // that does not match the name the engine looks under is not the record
    // for this asset. Falling through to the timestamps is the honest answer.
    ImportHealthFixture fixture("digest-wrong-name");
    const auto source = fixture.write("art/icon.png", "source");
    const auto output = fixture.write(".godot/imported/icon.ctex", "output");
    fixture.write(".godot/imported/icon.png-" + std::string(32, '0') + ".md5",
                  record(kSourceDigest, kOutputDigest));
    fixture.write("art/icon.png.import",
                  metadata("res://art/icon.png", "res://.godot/imported/icon.ctex"));
    const auto now = std::filesystem::file_time_type::clock::now();
    std::filesystem::last_write_time(output, now - std::chrono::hours(2));
    std::filesystem::last_write_time(source, now - std::chrono::hours(1));

    const auto report = didi::offline::inspectImportHealth(fixture.root().string(), 500);

    ASSERT_EQ(report["import_issue_count"], 1u);
    ASSERT_EQ(report["import_issues"][0]["kind"], "source_newer_than_output");
}

void test_the_digest_holds_across_block_boundaries() {
    // The digest is computed here rather than taken from a library, so the
    // lengths that exercise its padding are worth pinning: 56 bytes fills the
    // block to exactly the point where the length no longer fits and a second
    // block is needed, 64 fills one block exactly, and 200 spans four. Every
    // digest below was produced outside this program.
    ImportHealthFixture fixture("digest-boundaries");
    struct Asset {
        const char* name;
        size_t length;
        const char* path_digest;
        const char* body_digest;
    };
    const Asset assets[] = {
        {"a", 56, "0180b64844aadf81c6b18dd2c9268d2e", "3b0c8ac703f828b04c6c197006d17218"},
        {"b", 64, "3838221894d6aec06988724fc3ad7b07", "0b649bcb5a82868817fec9a6e709d233"},
        {"c", 200, "0ae7871a778bb5ac004e5dfadd30f18a", "0e90e342b5a27b934ec465dfa46d1ad2"},
    };
    const auto now = std::filesystem::file_time_type::clock::now();
    for (const auto& asset : assets) {
        const std::string name = asset.name;
        const auto source =
            fixture.write("art/" + name + ".png", std::string(asset.length, asset.name[0]));
        const auto output = fixture.write(".godot/imported/" + name + ".ctex", "output");
        fixture.write(".godot/imported/" + name + ".png-" + asset.path_digest + ".md5",
                      record(asset.body_digest, kOutputDigest));
        fixture.write("art/" + name + ".png.import",
                      metadata("res://art/" + name + ".png",
                               "res://.godot/imported/" + name + ".ctex"));
        // Every source is newer than its output, so a digest that comes out
        // wrong falls back to the timestamps and reports all three.
        std::filesystem::last_write_time(output, now - std::chrono::hours(2));
        std::filesystem::last_write_time(source, now - std::chrono::hours(1));
    }

    const auto report = didi::offline::inspectImportHealth(fixture.root().string(), 500);

    ASSERT_EQ(report["scanned_import_metadata"], 3u);
    ASSERT_EQ(report["import_issue_count"], 0u);
}

void test_a_symlinked_record_is_not_read() {
    ImportHealthFixture fixture("digest-symlink");
    const auto source = fixture.write("art/icon.png", "source");
    const auto output = fixture.write(".godot/imported/icon.ctex", "output");
    const auto real = fixture.write(".godot/imported/real.md5", record(kSourceDigest, kOutputDigest));
    std::error_code link_error;
    std::filesystem::create_symlink(real, fixture.root() / kIconRecordPath, link_error);
    if (link_error) return; // No symlink privilege on this machine.
    fixture.write("art/icon.png.import",
                  metadata("res://art/icon.png", "res://.godot/imported/icon.ctex"));
    const auto now = std::filesystem::file_time_type::clock::now();
    std::filesystem::last_write_time(output, now - std::chrono::hours(2));
    std::filesystem::last_write_time(source, now - std::chrono::hours(1));

    const auto report = didi::offline::inspectImportHealth(fixture.root().string(), 500);

    ASSERT_EQ(report["import_issue_count"], 1u);
    ASSERT_EQ(report["import_issues"][0]["kind"], "source_newer_than_output");
}

// A sidecar shaped the way an importer with several outputs writes one. Godot's
// csv_translation importer produces exactly this on a three-locale CSV: no
// `[remap] path`, and a `dest_files` list with an order that matters.
std::string multiOutputMetadata(const std::string& source,
                                const std::string& first,
                                const std::string& second) {
    return "[remap]\n"
           "importer=\"csv_translation\"\n"
           "type=\"Translation\"\n\n"
           "[deps]\n"
           "source_file=\"" + source + "\"\n"
           "dest_files=[\"" + first + "\", \"" + second + "\"]\n";
}

// The record for res://art/one.png. md5("res://art/one.png") is the stem.
const char* const kOneRecordPath =
    ".godot/imported/one.png-9a01884b0d623b8e0ece0eb1b76ab7fc.md5";
// md5("output" + "second") and md5("second" + "output"). One digest over the
// two files concatenated, which is what FileAccess::get_multiple_md5 computes,
// so the two orders are different answers. Both produced outside this program,
// and the declared-order value was confirmed against a real three-locale
// .translation set Godot imported: the dest_md5 it wrote is the md5 of the
// three files concatenated in the order dest_files lists them.
const char* const kDeclaredOrderDigest = "c8b828ab4e5513ed3d344831736b1c6a";
const char* const kReversedOrderDigest = "4e4c6a0b02374c56d2320e17b261380d";

void test_a_changed_output_is_reported() {
    // #830: the record holds both halves and only the source half was read, so
    // an imported output that no longer matches what was imported reported
    // clean. The engine compares dest_md5 too -- measured on 4.6.2, where
    // corrupting one .translation and scanning with a cold filesystem cache
    // reimports the asset.
    ImportHealthFixture fixture("dest-mismatch");
    const auto source = fixture.write("art/icon.png", "source");
    const auto output = fixture.write(".godot/imported/icon.ctex", "corrupted output");
    fixture.write(kIconRecordPath, record(kSourceDigest, kOutputDigest));
    fixture.write("art/icon.png.import",
                  metadata("res://art/icon.png", "res://.godot/imported/icon.ctex"));
    // Both files older than the other way round, so a timestamp answer would
    // report nothing here and the finding can only come from the digest.
    const auto now = std::filesystem::file_time_type::clock::now();
    std::filesystem::last_write_time(source, now - std::chrono::hours(2));
    std::filesystem::last_write_time(output, now - std::chrono::hours(1));

    const auto report = didi::offline::inspectImportHealth(fixture.root().string(), 500);

    ASSERT_EQ(report["import_issue_count"], 1u);
    ASSERT_EQ(report["import_issues"][0]["kind"], "output_changed_since_import");
    ASSERT_EQ(report["import_issues"][0]["source"], "res://art/icon.png");
    // A detail with no line, which the two were not allowed to be before.
    ASSERT_TRUE(report["import_issues"][0].contains("detail"));
    ASSERT_TRUE(!report["import_issues"][0].contains("line"));
}

void test_several_outputs_hash_in_the_order_dest_files_declares() {
    ImportHealthFixture fixture("dest-order-match");
    fixture.write("art/one.png", "source");
    fixture.write(".godot/imported/one.a", "output");
    fixture.write(".godot/imported/one.b", "second");
    fixture.write(kOneRecordPath, record(kSourceDigest, kDeclaredOrderDigest));
    fixture.write("art/one.png.import",
                  multiOutputMetadata("res://art/one.png", "res://.godot/imported/one.a",
                                      "res://.godot/imported/one.b"));

    const auto report = didi::offline::inspectImportHealth(fixture.root().string(), 500);

    ASSERT_EQ(report["scanned_import_metadata"], 1u);
    ASSERT_EQ(report["import_issue_count"], 0u);
}

void test_several_outputs_hashed_in_the_wrong_order_do_not_match() {
    // The control for the test above. If the outputs were hashed as a set, or
    // in any order but the declared one, the test above would pass for the
    // wrong reason and this one would pass too.
    ImportHealthFixture fixture("dest-order-reversed");
    fixture.write("art/one.png", "source");
    fixture.write(".godot/imported/one.a", "output");
    fixture.write(".godot/imported/one.b", "second");
    fixture.write(kOneRecordPath, record(kSourceDigest, kReversedOrderDigest));
    fixture.write("art/one.png.import",
                  multiOutputMetadata("res://art/one.png", "res://.godot/imported/one.a",
                                      "res://.godot/imported/one.b"));

    const auto report = didi::offline::inspectImportHealth(fixture.root().string(), 500);

    ASSERT_EQ(report["import_issue_count"], 1u);
    ASSERT_EQ(report["import_issues"][0]["kind"], "output_changed_since_import");
}

void test_a_missing_output_makes_no_digest_claim() {
    // A digest over a set with a hole in it is neither a match nor a real
    // mismatch, so the missing output is the only finding.
    ImportHealthFixture fixture("dest-incomplete");
    fixture.write("art/one.png", "source");
    fixture.write(".godot/imported/one.a", "output");
    fixture.write(kOneRecordPath, record(kSourceDigest, kDeclaredOrderDigest));
    fixture.write("art/one.png.import",
                  multiOutputMetadata("res://art/one.png", "res://.godot/imported/one.a",
                                      "res://.godot/imported/one.b"));

    const auto report = didi::offline::inspectImportHealth(fixture.root().string(), 500);

    ASSERT_EQ(report["import_issue_count"], 1u);
    ASSERT_EQ(report["import_issues"][0]["kind"], "missing_import_output");
}

void test_a_source_above_the_budget_is_unchecked_rather_than_timestamped() {
    // #831: an empty source digest meant either "no record" or "too large to
    // hash", and both arrived as source_newer_than_output. The documented
    // remedy for that finding is to open the project in the editor once, which
    // does nothing here: the record is already there and is deliberately not
    // compared. Resized rather than written, so the budget is exceeded without
    // moving 64 MiB through the disk.
    ImportHealthFixture fixture("source-over-budget");
    const auto source = fixture.write("art/icon.png", "source");
    const auto output = fixture.write(".godot/imported/icon.ctex", "output");
    std::error_code resize_error;
    std::filesystem::resize_file(source, didi::offline::kMaxImportSourceDigestBytes + 1,
                                 resize_error);
    if (resize_error) return; // No room for the file on this machine.
    fixture.write(kIconRecordPath, record(kSourceDigest, kOutputDigest));
    fixture.write("art/icon.png.import",
                  metadata("res://art/icon.png", "res://.godot/imported/icon.ctex"));
    const auto now = std::filesystem::file_time_type::clock::now();
    std::filesystem::last_write_time(output, now - std::chrono::hours(2));
    std::filesystem::last_write_time(source, now - std::chrono::hours(1));

    const auto report = didi::offline::inspectImportHealth(fixture.root().string(), 500);

    ASSERT_EQ(report["import_issue_count"], 1u);
    ASSERT_EQ(report["import_issues"][0]["kind"], "import_freshness_unchecked");
    ASSERT_TRUE(report["import_issues"][0]["detail"].get<std::string>().find("64 MiB") !=
                std::string::npos);
    ASSERT_TRUE(!report["import_issues"][0].contains("line"));
}

// A csv_translation sidecar: every output beside the CSV at the project root,
// and the record nowhere near any of them. Godot's own three-locale CSV writes
// exactly this shape, measured on 4.5.1, 4.6.2 and 4.7.2.
std::string translationMetadata(const std::string& source,
                                const std::string& first,
                                const std::string& second) {
    return "[remap]\n"
           "importer=\"csv_translation\"\n"
           "type=\"Translation\"\n\n"
           "[deps]\n"
           "source_file=\"" + source + "\"\n"
           "dest_files=[\"" + first + "\", \"" + second + "\"]\n";
}

// md5("res://strings.csv"), which is the stem Godot names the record with. It
// is written out rather than computed, and the engine agrees: importing a
// strings.csv at the root of a scratch project on all three lines writes
// .godot/imported/strings.csv-d31afa37497be317b120fe83e1aa65d6.md5.
const char* const kTranslationRecordName =
    "strings.csv-d31afa37497be317b120fe83e1aa65d6.md5";

// The outputs and the times that make a timestamp answer a finding, so a test
// that expects silence can only get it from the record.
void writeTranslationAsset(ImportHealthFixture& fixture) {
    const auto source = fixture.write("strings.csv", "source");
    const auto first = fixture.write("strings.en.translation", "output");
    const auto second = fixture.write("strings.fr.translation", "second");
    fixture.write("strings.csv.import",
                  translationMetadata("res://strings.csv", "res://strings.en.translation",
                                      "res://strings.fr.translation"));
    const auto now = std::filesystem::file_time_type::clock::now();
    std::filesystem::last_write_time(first, now - std::chrono::hours(2));
    std::filesystem::last_write_time(second, now - std::chrono::hours(2));
    std::filesystem::last_write_time(source, now - std::chrono::hours(1));
}

void test_the_record_is_found_when_no_output_is_near_it() {
    // #833: the record was looked for beside a declared output, which is where
    // it sits for a texture and nowhere else. An importer that writes to the
    // project root got no record, fell through to the timestamps, and reported
    // source_newer_than_output for an asset the engine considers current.
    ImportHealthFixture fixture("record-away-from-outputs");
    writeTranslationAsset(fixture);
    fixture.write(std::string(".godot/imported/") + kTranslationRecordName,
                  record(kSourceDigest, kDeclaredOrderDigest));

    const auto report = didi::offline::inspectImportHealth(fixture.root().string(), 500);

    ASSERT_EQ(report["scanned_import_metadata"], 1u);
    ASSERT_EQ(report["import_issue_count"], 0u);
}

void test_a_visible_project_data_directory_moves_the_record() {
    // The one thing that moves the record: the project data directory is
    // `godot` rather than `.godot` when the setting is off. Measured both ways
    // on 4.5.1, 4.6.2 and 4.7.2 -- the record moves and no output moves at all.
    ImportHealthFixture fixture("record-visible-data-dir");
    writeTranslationAsset(fixture);
    fixture.write("project.godot",
                  "config_version=5\n\n[application]\n\n"
                  "config/use_hidden_project_data_directory=false\n");
    fixture.write(std::string("godot/imported/") + kTranslationRecordName,
                  record(kSourceDigest, kDeclaredOrderDigest));

    const auto report = didi::offline::inspectImportHealth(fixture.root().string(), 500);

    ASSERT_EQ(report["scanned_import_metadata"], 1u);
    ASSERT_EQ(report["import_issue_count"], 0u);
}

void test_a_record_left_in_the_hidden_directory_is_not_read() {
    // The control for the test above, and the case that makes reading the
    // setting worth it rather than looking under both names: turning the
    // setting off leaves the old `.godot` behind with a record in it, and that
    // record is not the one the engine consults. The key is spelled the way
    // #809 was filed about, because the engine drops the whitespace inside a
    // key and this is the same setting to it.
    ImportHealthFixture fixture("record-stale-hidden-dir");
    writeTranslationAsset(fixture);
    fixture.write("project.godot",
                  "config_version=5\n\n[application]\n\n"
                  "config / use_hidden_project_data_directory = false\n");
    fixture.write(std::string(".godot/imported/") + kTranslationRecordName,
                  record(kSourceDigest, kDeclaredOrderDigest));

    const auto report = didi::offline::inspectImportHealth(fixture.root().string(), 500);

    ASSERT_EQ(report["import_issue_count"], 2u);
    ASSERT_EQ(report["import_issues"][0]["kind"], "source_newer_than_output");
    ASSERT_EQ(report["import_issues"][1]["kind"], "source_newer_than_output");
}

void test_a_symlinked_manifest_is_not_read() {
    // The manifest is read for one setting and is refused as a symlink for the
    // reason every other read in this scan refuses one: it can leave the
    // project. Refused, the answer is the default, so the record that really is
    // under `godot/imported` is not found and the timestamps answer instead.
    ImportHealthFixture fixture("manifest-symlink");
    writeTranslationAsset(fixture);
    const auto real = fixture.write("elsewhere.godot",
                                    "config_version=5\n\n[application]\n\n"
                                    "config/use_hidden_project_data_directory=false\n");
    std::error_code link_error;
    std::filesystem::create_symlink(real, fixture.root() / "project.godot", link_error);
    if (link_error) return; // No symlink privilege on this machine.
    fixture.write(std::string("godot/imported/") + kTranslationRecordName,
                  record(kSourceDigest, kDeclaredOrderDigest));

    const auto report = didi::offline::inspectImportHealth(fixture.root().string(), 500);

    ASSERT_EQ(report["import_issue_count"], 2u);
    ASSERT_EQ(report["import_issues"][0]["kind"], "source_newer_than_output");
}

struct RegisterImportHealthTests {
    RegisterImportHealthTests() {
        registerTest("ImportHealth.Healthy", test_healthy_import_metadata_has_no_issues);
        registerTest("ImportHealth.MissingSource", test_missing_source_is_reported);
        registerTest("ImportHealth.MissingOutputDeduplicated", test_missing_output_is_deduplicated);
        registerTest("ImportHealth.NewerSource", test_newer_source_is_reported_as_timestamp_evidence);
        registerTest("ImportHealth.MalformedAndOversized",
                     test_malformed_and_oversized_metadata_fail_closed);
        registerTest("ImportHealth.UnsafePaths", test_unsafe_resource_paths_fail_closed);
        registerTest("ImportHealth.OrderingAndCap",
                     test_findings_are_sorted_capped_and_counted_before_cap);
        registerTest("ImportHealth.Symlinks", test_symlinked_metadata_and_output_are_not_followed);
        registerTest("ImportHealth.InvalidContract",
                     test_invalid_flag_malformed_path_and_source_mismatch_fail_closed);
        registerTest("ImportHealth.CommentDecoys", test_comment_decoys_are_ignored);
        registerTest("ImportHealth.GeneratedDirectories",
                     test_generated_directories_are_not_metadata_sources);
        registerTest("ImportHealth.PathAmplification",
                     test_output_path_amplification_fails_as_one_invalid_metadata_issue);
        registerTest("ImportHealth.SectionScope",
                     test_importer_params_cannot_impersonate_remap_or_dependency_fields);
        registerTest("ImportHealth.EmptyRemapPath",
                     test_empty_remap_path_is_valid_for_importers_without_outputs);
        registerTest("ImportHealth.MultisegmentFeaturePath",
                     test_multisegment_feature_path_is_checked);
        registerTest("ImportHealth.SpacedSectionHeader",
                     test_a_spaced_section_header_is_still_remap_and_deps);
        registerTest("ImportHealth.TrailingComment",
                     test_a_trailing_comment_is_not_part_of_a_dependency_path);
        registerTest("ImportHealth.UnloadableValue",
                     test_a_value_the_parser_refuses_makes_the_sidecar_unparseable);
        registerTest("ImportHealth.TruncatedValue",
                     test_a_sidecar_that_ends_inside_a_value_is_unparseable);
        registerTest("ImportHealth.RealSidecarShapes",
                     test_a_healthy_sidecar_still_publishes_no_detail);
        registerTest("ImportHealth.RecordedDigestMatch",
                     test_the_recorded_digest_answers_before_the_timestamps);
        registerTest("ImportHealth.RecordedDigestMismatch",
                     test_a_source_that_does_not_match_the_record_is_reported);
        registerTest("ImportHealth.RecordUnderWrongName",
                     test_a_record_under_the_wrong_name_is_not_read);
        registerTest("ImportHealth.DigestBlockBoundaries",
                     test_the_digest_holds_across_block_boundaries);
        registerTest("ImportHealth.SymlinkedRecord", test_a_symlinked_record_is_not_read);
        registerTest("ImportHealth.OutputChanged", test_a_changed_output_is_reported);
        registerTest("ImportHealth.OutputDigestOrder",
                     test_several_outputs_hash_in_the_order_dest_files_declares);
        registerTest("ImportHealth.OutputDigestWrongOrder",
                     test_several_outputs_hashed_in_the_wrong_order_do_not_match);
        registerTest("ImportHealth.OutputDigestIncomplete",
                     test_a_missing_output_makes_no_digest_claim);
        registerTest("ImportHealth.RecordAwayFromOutputs",
                     test_the_record_is_found_when_no_output_is_near_it);
        registerTest("ImportHealth.VisibleProjectDataDirectory",
                     test_a_visible_project_data_directory_moves_the_record);
        registerTest("ImportHealth.StaleHiddenRecord",
                     test_a_record_left_in_the_hidden_directory_is_not_read);
        registerTest("ImportHealth.SymlinkedManifest", test_a_symlinked_manifest_is_not_read);
        registerTest("ImportHealth.SourceOverBudget",
                     test_a_source_above_the_budget_is_unchecked_rather_than_timestamped);
    }
} g_registerImportHealthTests;

} // namespace
