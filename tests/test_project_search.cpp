#include "didi/offline/project_search.hpp"

#include <chrono>
#include <filesystem>
#include <fstream>
#include <functional>
#include <stdexcept>
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

struct RegisterProjectSearchTests {
    RegisterProjectSearchTests() {
        registerTest("ProjectSearch.TextAndGdscriptSymbols", test_text_and_gdscript_symbols);
        registerTest("ProjectSearch.RejectsEscapeAndInvalidLimits", test_rejects_escape_and_invalid_limits);
        registerTest("ProjectSearch.CSharpSymbolsIgnoreCommentsAndStrings", test_csharp_symbols_ignore_comments_and_strings);
        registerTest("ProjectSearch.BinaryFileIsDiagnosticNotMatch", test_binary_file_is_diagnostic_not_match);
        registerTest("ProjectSearch.ResultOrderAndWholeWordBoundary", test_result_order_and_whole_word_boundary);
    }
} g_register_project_search_tests;

} // namespace
