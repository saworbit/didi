#include "didi/offline/project_search.hpp"
#include "didi/common/project_path.hpp"

#include <chrono>
#include <filesystem>
#include <fstream>
#include <functional>
#include <stdexcept>
#include <set>
#include <string>

#define ASSERT_TRUE(cond) if (!(cond)) throw std::runtime_error("Assertion failed: " #cond)
#define ASSERT_EQ(a, b) ASSERT_TRUE((a) == (b))

void registerTest(const std::string& name, std::function<void()> fn);

namespace {

class SearchFixture {
public:
    SearchFixture() {
        const auto nonce = std::chrono::steady_clock::now().time_since_epoch().count();
        m_root = std::filesystem::temp_directory_path() /
                 ("didi-project-search-" + std::to_string(nonce));
        std::filesystem::create_directories(m_root);
    }

    ~SearchFixture() {
        std::error_code ignored;
        std::filesystem::remove_all(m_root, ignored);
    }

    void write(const std::filesystem::path& relative, const std::string& contents) {
        const auto target = m_root / relative;
        std::filesystem::create_directories(target.parent_path());
        std::ofstream output(target, std::ios::binary);
        output.write(contents.data(), static_cast<std::streamsize>(contents.size()));
    }

    const std::filesystem::path& root() const { return m_root; }

private:
    std::filesystem::path m_root;
};

void test_text_and_gdscript_symbols() {
    // Break caught: project search misses real declarations or reports declarations from comments.
    SearchFixture fixture;
    fixture.write("scripts/player.gd",
                  "class_name PlayerController\n"
                  "# func Fake()\n"
                  "func jump():\n"
                  "\tpass\n");

    didi::offline::ProjectSearch search(fixture.root());
    didi::offline::SearchOptions text_options;
    text_options.query = "PlayerController";
    const auto text = search.searchText(text_options);
    ASSERT_TRUE(text.isOk());
    ASSERT_EQ(text.value().matches.size(), 1u);
    ASSERT_EQ(text.value().matches[0].path, "res://scripts/player.gd");
    ASSERT_EQ(text.value().matches[0].line, 1u);
    ASSERT_EQ(text.value().matches[0].column, 12u);

    didi::offline::SymbolSearchOptions symbol_options;
    symbol_options.query = "Player";
    symbol_options.match = didi::offline::SymbolMatch::Prefix;
    const auto symbols = search.searchSymbols(symbol_options);
    ASSERT_TRUE(symbols.isOk());
    ASSERT_EQ(symbols.value().matches.size(), 1u);
    ASSERT_EQ(symbols.value().matches[0].name, "PlayerController");
    ASSERT_EQ(symbols.value().matches[0].kind, "class");
    ASSERT_EQ(symbols.value().matches[0].language, "gdscript");
}

void test_rejects_escape_and_invalid_limits() {
    // Break caught: an attacker can escape the project root or force unbounded result work.
    SearchFixture fixture;
    fixture.write("safe.gd", "var safe_value = 1\n");
    didi::offline::ProjectSearch search(fixture.root());

    didi::offline::SearchOptions traversal;
    traversal.query = "safe";
    traversal.search_path = "res://../";
    ASSERT_TRUE(search.searchText(traversal).isErr());

    didi::offline::SearchOptions absolute;
    absolute.query = "safe";
    absolute.search_path = "C:/Windows";
    ASSERT_TRUE(search.searchText(absolute).isErr());

    didi::offline::SearchOptions too_many;
    too_many.query = "safe";
    too_many.max_results = 501;
    ASSERT_TRUE(search.searchText(too_many).isErr());

    didi::offline::SearchOptions wrong_extension;
    wrong_extension.query = "safe";
    wrong_extension.extensions = {".md"};
    ASSERT_TRUE(search.searchText(wrong_extension).isErr());
}

void test_csharp_symbols_ignore_comments_and_strings() {
    // Break caught: symbol search omits C# declarations or treats comments/strings as code.
    SearchFixture fixture;
    fixture.write("scripts/Player.cs",
                  "namespace Game;\n"
                  "// class FakeComment {}\n"
                  "public sealed class PlayerController {\n"
                  "  private string text = \"class FakeString {}\";\n"
                  "  public void Jump() {}\n"
                  "  public int Speed { get; set; }\n"
                  "}\n");
    didi::offline::ProjectSearch search(fixture.root());

    didi::offline::SymbolSearchOptions classes;
    classes.query = "Player";
    const auto class_result = search.searchSymbols(classes);
    ASSERT_TRUE(class_result.isOk());
    ASSERT_EQ(class_result.value().matches.size(), 1u);
    ASSERT_EQ(class_result.value().matches[0].name, "PlayerController");
    ASSERT_EQ(class_result.value().matches[0].language, "csharp");

    didi::offline::SymbolSearchOptions fakes;
    fakes.query = "Fake";
    const auto fake_result = search.searchSymbols(fakes);
    ASSERT_TRUE(fake_result.isOk());
    ASSERT_TRUE(fake_result.value().matches.empty());
}

void test_csharp_call_sites_are_not_function_declarations() {
    // Break caught: any line with an opening paren became a function
    // declaration, so every call site in a C# file was reported as a symbol.
    SearchFixture fixture;
    fixture.write("scripts/Spawner.cs",
                  "public partial class Spawner : Node3D {\n"
                  "  public override void _Ready() {\n"
                  "    GD.Print(\"Loaded\");\n"
                  "    AddChild(node);\n"
                  "    var limit = Math.Min(a, b);\n"
                  "    _items.Clear();\n"
                  "    base.Configure(this);\n"
                  "    var enemy = new Enemy(hp);\n"
                  "    return;\n"
                  "  }\n"
                  "  private int Score(int hits) { return hits * 2; }\n"
                  "  public abstract void Draw();\n"
                  "}\n");
    didi::offline::ProjectSearch search(fixture.root());

    const auto functionsNamed = [&](const std::string& query) {
        didi::offline::SymbolSearchOptions options;
        options.query = query;
        const auto result = search.searchSymbols(options);
        ASSERT_TRUE(result.isOk());
        size_t functions = 0;
        for (const auto& match : result.value().matches) {
            if (match.kind == "function") ++functions;
        }
        return functions;
    };

    // Call sites, not declarations.
    ASSERT_EQ(functionsNamed("Print"), 0u);
    ASSERT_EQ(functionsNamed("AddChild"), 0u);
    ASSERT_EQ(functionsNamed("Min"), 0u);
    ASSERT_EQ(functionsNamed("Clear"), 0u);
    ASSERT_EQ(functionsNamed("Configure"), 0u);
    ASSERT_EQ(functionsNamed("Enemy"), 0u);

    // Real declarations, including the bodiless abstract member.
    ASSERT_EQ(functionsNamed("_Ready"), 1u);
    ASSERT_EQ(functionsNamed("Score"), 1u);
    ASSERT_EQ(functionsNamed("Draw"), 1u);
}

void test_csharp_symbols_ignore_multiline_strings() {
    // Break caught: declarations embedded in multiline verbatim/raw strings leak into symbol results.
    SearchFixture fixture;
    fixture.write("scripts/Multiline.cs",
                  "public class RealContainer {\n"
                  "  private string verbatim = @\"first line\n"
                  "public void FakeVerbatim() {}\n"
                  "last line\";\n"
                  "  private string raw = \"\"\"\n"
                  "public void FakeRaw() {}\n"
                  "\"\"\";\n"
                  "  public void RealMethod() {}\n"
                  "}\n");
    didi::offline::ProjectSearch search(fixture.root());

    didi::offline::SymbolSearchOptions fakes;
    fakes.query = "Fake";
    const auto fake_result = search.searchSymbols(fakes);
    ASSERT_TRUE(fake_result.isOk());
    ASSERT_TRUE(fake_result.value().matches.empty());

    didi::offline::SymbolSearchOptions real;
    real.query = "Real";
    const auto real_result = search.searchSymbols(real);
    ASSERT_TRUE(real_result.isOk());
    ASSERT_EQ(real_result.value().matches.size(), 2u);
    ASSERT_EQ(real_result.value().matches[0].name, "RealContainer");
    ASSERT_EQ(real_result.value().matches[1].name, "RealMethod");
}

void test_gdscript_symbols_ignore_multiline_strings() {
    // Break caught: declarations embedded in GDScript triple-quoted strings leak into symbol results.
    SearchFixture fixture;
    fixture.write("scripts/multiline.gd",
                  "class_name RealScript\n"
                  "var documentation = \"\"\"first line\n"
                  "func fake_double():\n"
                  "last line\"\"\"\n"
                  "var notes = '''first line\n"
                  "signal fake_single\n"
                  "last line'''\n"
                  "func real_method():\n"
                  "\tpass\n");
    didi::offline::ProjectSearch search(fixture.root());

    didi::offline::SymbolSearchOptions fakes;
    fakes.query = "fake";
    const auto fake_result = search.searchSymbols(fakes);
    ASSERT_TRUE(fake_result.isOk());
    ASSERT_TRUE(fake_result.value().matches.empty());

    didi::offline::SymbolSearchOptions real;
    real.query = "real";
    real.case_sensitive = false;
    const auto real_result = search.searchSymbols(real);
    ASSERT_TRUE(real_result.isOk());
    ASSERT_EQ(real_result.value().matches.size(), 2u);
}

void test_gdscript_symbols_include_annotations_static_and_inner_classes() {
    SearchFixture fixture;
    fixture.write("scripts/declarations.gd",
                  "@export_range(0, 20) var speed: float = 5.0\n"
                  "@onready var sprite = $Sprite\n"
                  "@rpc(\"any_peer\") func remote_jump():\n"
                  "\tpass\n"
                  "static func build_player():\n"
                  "\tpass\n"
                  "class InnerState:\n"
                  "\tpass\n");
    didi::offline::ProjectSearch search(fixture.root());

    for (const auto& [query, kind] : {
             std::pair{"speed", "variable"},
             std::pair{"sprite", "variable"},
             std::pair{"remote_jump", "function"},
             std::pair{"build_player", "function"},
             std::pair{"InnerState", "class"}}) {
        didi::offline::SymbolSearchOptions options;
        options.query = query;
        options.match = didi::offline::SymbolMatch::Exact;
        const auto result = search.searchSymbols(options);
        ASSERT_TRUE(result.isOk());
        ASSERT_EQ(result.value().matches.size(), 1u);
        ASSERT_EQ(result.value().matches[0].kind, kind);
    }
}

void test_binary_file_is_diagnostic_not_match() {
    // Break caught: a NUL-bearing binary file leaks arbitrary bytes into search results.
    SearchFixture fixture;
    fixture.write("binary.gd", std::string("Player\0Controller\n", 18));
    didi::offline::ProjectSearch search(fixture.root());
    didi::offline::SearchOptions options;
    options.query = "Player";
    const auto result = search.searchText(options);
    ASSERT_TRUE(result.isOk());
    ASSERT_TRUE(result.value().matches.empty());
    ASSERT_EQ(result.value().diagnostics.size(), 1u);
    ASSERT_EQ(result.value().diagnostics[0].reason, "binary_or_invalid_utf8");
}

void test_result_order_and_whole_word_boundary() {
    // Break caught: filesystem enumeration order leaks into results or whole-word matching accepts identifiers.
    SearchFixture fixture;
    fixture.write("z.gd", "var PlayerTwo = 1\nPlayer\n");
    fixture.write("a.gd", "Player\n");
    fixture.write(".godot/ignored.gd", "Player\n");
    didi::offline::ProjectSearch search(fixture.root());
    didi::offline::SearchOptions options;
    options.query = "player";
    options.case_sensitive = false;
    options.whole_word = true;
    const auto result = search.searchText(options);
    ASSERT_TRUE(result.isOk());
    ASSERT_EQ(result.value().matches.size(), 2u);
    ASSERT_EQ(result.value().matches[0].path, "res://a.gd");
    ASSERT_EQ(result.value().matches[1].path, "res://z.gd");
}

void test_unicode_paths_round_trip_as_utf8() {
    // Break caught: Windows narrow conversions mangle res:// paths for non-ASCII file names.
    const std::string relative_utf8 = "scripts/inimigo_a\xC3\xA7\xC3\xA3o.gd";
    SearchFixture fixture;
    fixture.write(didi::paths::projectPathFromUtf8(relative_utf8),
                  "func atacar():\n\tpass\n");

    didi::offline::ProjectSearch search(fixture.root());
    didi::offline::SearchOptions text_options;
    text_options.query = "atacar";
    const auto text = search.searchText(text_options);
    ASSERT_TRUE(text.isOk());
    ASSERT_EQ(text.value().matches.size(), 1u);
    ASSERT_EQ(text.value().matches[0].path, "res://" + relative_utf8);
}

void test_unicode_search_path_resolves_to_its_directory() {
    // Break caught: resolveSearchRoot built the directory with the narrow path
    // constructor, so a search scoped to a non-ASCII res:// directory read the
    // bytes as the active code page on Windows and came back 404.
    const std::string directory_utf8 = "scripts/a\xC3\xA7\xC3\xA3o";
    SearchFixture fixture;
    fixture.write(didi::paths::projectPathFromUtf8(directory_utf8 + "/inimigo.gd"),
                  "func atacar():\n\tpass\n");
    fixture.write("scripts/outside.gd", "func atacar():\n\tpass\n");

    didi::offline::ProjectSearch search(fixture.root());
    didi::offline::SearchOptions options;
    options.query = "atacar";
    options.search_path = "res://" + directory_utf8;
    const auto scoped = search.searchText(options);
    ASSERT_TRUE(scoped.isOk());
    ASSERT_EQ(scoped.value().matches.size(), 1u);
    ASSERT_EQ(scoped.value().matches[0].path, "res://" + directory_utf8 + "/inimigo.gd");
}

struct RegisterProjectSearchTests {
    RegisterProjectSearchTests() {
        registerTest("ProjectSearch.TextAndGdscriptSymbols", test_text_and_gdscript_symbols);
        registerTest("ProjectSearch.RejectsEscapeAndInvalidLimits", test_rejects_escape_and_invalid_limits);
        registerTest("ProjectSearch.CSharpSymbolsIgnoreCommentsAndStrings", test_csharp_symbols_ignore_comments_and_strings);
        registerTest("ProjectSearch.CSharpCallSitesAreNotDeclarations", test_csharp_call_sites_are_not_function_declarations);
        registerTest("ProjectSearch.CSharpSymbolsIgnoreMultilineStrings", test_csharp_symbols_ignore_multiline_strings);
        registerTest("ProjectSearch.GdscriptSymbolsIgnoreMultilineStrings", test_gdscript_symbols_ignore_multiline_strings);
        registerTest("ProjectSearch.GdscriptDeclarationForms", test_gdscript_symbols_include_annotations_static_and_inner_classes);
        registerTest("ProjectSearch.BinaryFileIsDiagnosticNotMatch", test_binary_file_is_diagnostic_not_match);
        registerTest("ProjectSearch.ResultOrderAndWholeWordBoundary", test_result_order_and_whole_word_boundary);
        registerTest("ProjectSearch.UnicodePathsRoundTripAsUtf8", test_unicode_paths_round_trip_as_utf8);
        registerTest("ProjectSearch.UnicodeSearchPathResolves", test_unicode_search_path_resolves_to_its_directory);
    }
} g_register_project_search_tests;

} // namespace
