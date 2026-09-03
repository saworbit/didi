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
    for (const auto& issue : report["import_issues"]) {
        ASSERT_EQ(issue["kind"], "invalid_import_metadata");
    }
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
    for (const auto& issue : report["import_issues"]) {
        ASSERT_EQ(issue["kind"], "invalid_import_metadata");
    }
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
    }
} g_registerImportHealthTests;

} // namespace
