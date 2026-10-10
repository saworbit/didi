#include "didi/offline/test_reports.hpp"

#include "didi/common/project_path.hpp"
#include "didi/common/secure_random.hpp"

#include <algorithm>
#include <cctype>
#include <fstream>
#include <iterator>
#include <regex>
#include <set>
#include <sstream>

namespace didi::offline {

namespace fs = std::filesystem;

namespace {

Error refuse(int status, std::string message, const std::string& field, json extra = json::object()) {
    json data = {{"code", status == 404 ? "not_found" : (status == 409 ? "conflict" : "invalid_arguments")},
                 {"field", field}};
    for (auto& [key, value] : extra.items()) data[key] = std::move(value);
    return Error(status, std::move(message), std::move(data));
}

bool integerIn(const json& value, int64_t low, int64_t high) {
    if (value.is_number_unsigned()) {
        return value.get<uint64_t>() >= static_cast<uint64_t>(std::max<int64_t>(low, 0)) &&
               value.get<uint64_t>() <= static_cast<uint64_t>(high);
    }
    return value.is_number_integer() && value.get<int64_t>() >= low && value.get<int64_t>() <= high;
}

bool validName(const std::string& name) {
    if (name.empty() || name.size() > 64) return false;
    if (!std::isalnum(static_cast<unsigned char>(name.front()))) return false;
    return std::all_of(name.begin(), name.end(), [](unsigned char c) {
        return std::isalnum(c) != 0 || c == '_' || c == '-';
    });
}

std::optional<fs::path> underRoot(const fs::path& root, const std::string& res_path) {
    if (res_path.rfind("res://", 0) != 0) return std::nullopt;
    fs::path relative;
    try {
        relative = paths::projectPathFromUtf8(res_path.substr(6)).lexically_normal();
    } catch (const std::exception&) {
        return std::nullopt;
    }
    if (relative.is_absolute() || relative.has_root_name() ||
        (!relative.empty() && *relative.begin() == "..")) {
        return std::nullopt;
    }
    return relative.empty() ? root : root / relative;
}

std::string readSmallText(const fs::path& path) {
    std::ifstream in(path, std::ios::binary);
    if (!in) return {};
    std::string text(std::istreambuf_iterator<char>(in), {});
    if (text.size() > 64 * 1024) text.resize(64 * 1024);
    return text;
}

std::string pluginVersion(const fs::path& plugin_cfg) {
    static const std::regex version(R"re(^\s*version\s*=\s*"([^"]*)")re");
    std::istringstream lines(readSmallText(plugin_cfg));
    std::string line;
    while (std::getline(lines, line)) {
        std::smatch match;
        if (std::regex_search(line, match, version)) return match[1].str();
    }
    return {};
}

// --- XML -----------------------------------------------------------------------

// Just enough XML for a JUnit report: elements, attributes, text and CDATA,
// with comments, declarations and doctypes skipped and the five named entities
// and numeric references decoded. Anything else is refused rather than guessed.
class JUnitScanner {
public:
    explicit JUnitScanner(std::string_view xml) : m_xml(xml) {}

    Result<JUnitReport> scan() {
        while (m_at < m_xml.size()) {
            const auto open = m_xml.find('<', m_at);
            if (open == std::string_view::npos) break;
            if (m_case && m_capture) m_text += decode(m_xml.substr(m_at, open - m_at));
            m_at = open;
            if (startsWith("<!--")) {
                if (!skipPast("-->")) return Error(422, "A comment in the report never ends.");
            } else if (startsWith("<![CDATA[")) {
                const auto end = m_xml.find("]]>", m_at + 9);
                if (end == std::string_view::npos) return Error(422, "A CDATA section in the report never ends.");
                if (m_case && m_capture) m_text += std::string(m_xml.substr(m_at + 9, end - m_at - 9));
                m_at = end + 3;
            } else if (startsWith("<?") || startsWith("<!")) {
                if (!skipPast(">")) return Error(422, "A declaration in the report never ends.");
            } else if (startsWith("</")) {
                const auto end = m_xml.find('>', m_at);
                if (end == std::string_view::npos) return Error(422, "A closing tag in the report never ends.");
                auto closed = trim(std::string(m_xml.substr(m_at + 2, end - m_at - 2)));
                m_at = end + 1;
                auto ended = close(closed);
                if (ended.isErr()) return ended.error();
            } else {
                auto opened = openElement();
                if (opened.isErr()) return opened.error();
            }
        }
        if (!m_stack.empty()) return Error(422, "The report ends inside <" + m_stack.back() + ">.");
        return std::move(m_report);
    }

private:
    bool startsWith(std::string_view prefix) const {
        return m_xml.substr(m_at, prefix.size()) == prefix;
    }

    bool skipPast(std::string_view terminator) {
        const auto end = m_xml.find(terminator, m_at);
        if (end == std::string_view::npos) return false;
        m_at = end + terminator.size();
        return true;
    }

    static std::string trim(std::string text) {
        const auto first = text.find_first_not_of(" \t\r\n");
        if (first == std::string::npos) return {};
        const auto last = text.find_last_not_of(" \t\r\n");
        return text.substr(first, last - first + 1);
    }

    static std::string decode(std::string_view text) {
        std::string out;
        out.reserve(text.size());
        for (size_t i = 0; i < text.size(); ++i) {
            if (text[i] != '&') {
                out += text[i];
                continue;
            }
            const auto semi = text.find(';', i);
            if (semi == std::string_view::npos || semi - i > 10) {
                out += text[i];
                continue;
            }
            const auto entity = text.substr(i + 1, semi - i - 1);
            if (entity == "lt") out += '<';
            else if (entity == "gt") out += '>';
            else if (entity == "amp") out += '&';
            else if (entity == "quot") out += '"';
            else if (entity == "apos") out += '\'';
            else if (!entity.empty() && entity[0] == '#') {
                unsigned long code = 0;
                try {
                    code = entity.size() > 1 && (entity[1] == 'x' || entity[1] == 'X')
                               ? std::stoul(std::string(entity.substr(2)), nullptr, 16)
                               : std::stoul(std::string(entity.substr(1)), nullptr, 10);
                } catch (const std::exception&) {
                    out += text.substr(i, semi - i + 1);
                    i = semi;
                    continue;
                }
                appendUtf8(out, code);
            } else {
                out += text.substr(i, semi - i + 1);
            }
            i = semi;
        }
        return out;
    }

    static void appendUtf8(std::string& out, unsigned long code) {
        if (code < 0x80) {
            out += static_cast<char>(code);
        } else if (code < 0x800) {
            out += static_cast<char>(0xC0 | (code >> 6));
            out += static_cast<char>(0x80 | (code & 0x3F));
        } else if (code < 0x10000) {
            out += static_cast<char>(0xE0 | (code >> 12));
            out += static_cast<char>(0x80 | ((code >> 6) & 0x3F));
            out += static_cast<char>(0x80 | (code & 0x3F));
        } else if (code < 0x110000) {
            out += static_cast<char>(0xF0 | (code >> 18));
            out += static_cast<char>(0x80 | ((code >> 12) & 0x3F));
            out += static_cast<char>(0x80 | ((code >> 6) & 0x3F));
            out += static_cast<char>(0x80 | (code & 0x3F));
        }
    }

    Result<void> openElement() {
        size_t i = m_at + 1;
        const auto name_start = i;
        while (i < m_xml.size() && !std::isspace(static_cast<unsigned char>(m_xml[i])) &&
               m_xml[i] != '>' && m_xml[i] != '/') {
            ++i;
        }
        const std::string name(m_xml.substr(name_start, i - name_start));
        if (name.empty()) return Error(422, "The report has a tag with no name.");
        json attributes = json::object();
        bool self_closing = false;
        while (true) {
            while (i < m_xml.size() && std::isspace(static_cast<unsigned char>(m_xml[i]))) ++i;
            if (i >= m_xml.size()) return Error(422, "The report ends inside <" + name + ">.");
            if (m_xml[i] == '>') { ++i; break; }
            if (m_xml[i] == '/') {
                if (i + 1 < m_xml.size() && m_xml[i + 1] == '>') { self_closing = true; i += 2; break; }
                return Error(422, "A stray '/' in <" + name + ">.");
            }
            const auto key_start = i;
            while (i < m_xml.size() && m_xml[i] != '=' && !std::isspace(static_cast<unsigned char>(m_xml[i])) &&
                   m_xml[i] != '>') {
                ++i;
            }
            const std::string key(m_xml.substr(key_start, i - key_start));
            while (i < m_xml.size() && std::isspace(static_cast<unsigned char>(m_xml[i]))) ++i;
            if (i >= m_xml.size() || m_xml[i] != '=') return Error(422, "Attribute " + key + " in <" + name + "> has no value.");
            ++i;
            while (i < m_xml.size() && std::isspace(static_cast<unsigned char>(m_xml[i]))) ++i;
            if (i >= m_xml.size() || (m_xml[i] != '"' && m_xml[i] != '\'')) {
                return Error(422, "Attribute " + key + " in <" + name + "> is not quoted.");
            }
            const char quote = m_xml[i++];
            const auto value_end = m_xml.find(quote, i);
            if (value_end == std::string_view::npos) return Error(422, "Attribute " + key + " in <" + name + "> never ends.");
            attributes[key] = decode(m_xml.substr(i, value_end - i));
            i = value_end + 1;
        }
        m_at = i;
        start(name, attributes);
        if (self_closing) return close(name);
        m_stack.push_back(name);
        if (m_stack.size() > 32) return Error(422, "The report nests deeper than a test report does.");
        return Result<void>::ok();
    }

    void start(const std::string& name, const json& attributes) {
        const auto attr = [&](const char* key) {
            return attributes.contains(key) ? attributes[key].get<std::string>() : std::string();
        };
        if (name == "testsuite") {
            m_suite = attr("name");
            const auto package = attr("package");
            if (!package.empty() && !m_suite.empty()) m_suite = package + "/" + m_suite;
        } else if (name == "testcase") {
            m_case = TestCaseResult{};
            m_case->suite = attr("classname").empty() ? m_suite : attr("classname");
            m_case->name = attr("name");
            m_case->outcome = "passed";
            try {
                m_case->seconds = attr("time").empty() ? 0.0 : std::stod(attr("time"));
            } catch (const std::exception&) {
                m_case->seconds = 0.0;
            }
            // GUT says outright when a test asserted nothing; it passes there.
            const auto status = attr("status");
            if (status == "no asserts" || status == "risky") m_case->outcome = "no_assertions";
        } else if (m_case && (name == "failure" || name == "error" || name == "skipped")) {
            // A failure outranks a skip, and an error outranks both.
            const std::string outcome = name == "failure" ? "failed" : (name == "error" ? "error" : "skipped");
            const auto rank = [](const std::string& o) {
                return o == "error" ? 3 : o == "failed" ? 2 : o == "skipped" ? 1 : 0;
            };
            if (rank(outcome) >= rank(m_case->outcome)) m_case->outcome = outcome;
            m_case->message = attr("message");
            m_capture = true;
            m_text.clear();
        }
    }

    Result<void> close(const std::string& name) {
        if (!m_stack.empty() && m_stack.back() == name) {
            m_stack.pop_back();
        } else if (!m_stack.empty()) {
            return Error(422, "</" + name + "> closes <" + m_stack.back() + ">.");
        }
        if (m_case && (name == "failure" || name == "error" || name == "skipped")) {
            const auto text = trim(m_text);
            if (!text.empty()) {
                m_case->message = m_case->message.empty() ? text : m_case->message + "\n" + text;
            }
            m_capture = false;
        } else if (name == "testcase" && m_case) {
            m_report.cases.push_back(std::move(*m_case));
            m_case.reset();
        } else if (name == "testsuite") {
            m_suite.clear();
        }
        return Result<void>::ok();
    }

    std::string_view m_xml;
    size_t m_at{0};
    std::vector<std::string> m_stack;
    JUnitReport m_report;
    std::string m_suite;
    std::optional<TestCaseResult> m_case;
    bool m_capture{false};
    std::string m_text;
};

// Where a failure happened, from what either framework writes: GdUnit4 names
// "res://test/x_test.gd:10", GUT names its suite by path and says "at line 10".
void locate(TestCaseResult& test) {
    static const std::regex res_line(R"((res://[^\s:'"()]+\.gd):([0-9]+))");
    static const std::regex at_line(R"(at line ([0-9]+))");
    std::smatch match;
    if (std::regex_search(test.message, match, res_line)) {
        test.file = match[1].str();
        test.line = std::stoi(match[2].str());
        return;
    }
    if (test.suite.size() > 3 && test.suite.compare(test.suite.size() - 3, 3, ".gd") == 0) {
        test.file = test.suite.rfind("res://", 0) == 0 ? test.suite : "res://" + test.suite;
    }
    if (std::regex_search(test.message, match, at_line)) test.line = std::stoi(match[1].str());
}

// Scripts the run could not load. A test file that does not parse is dropped
// from GUT's report without a word in it, so the output is the only record. On
// 4.5.1, 4.6.2 and 4.7.2 a parse error prints
//   SCRIPT ERROR: Parse Error: <message>
//      at: GDScript::reload (res://test/test_broken.gd:4)
// and a load that failed prints ERROR: Failed to load script "res://..." ...
json scriptsThatDidNotLoad(const std::string& output) {
    static const std::regex at_location(R"(at:\s*\S*\s*\((res://[^:()]+):([0-9]+)\))");
    static const std::regex failed_load(R"re(Failed to load script "(res://[^"]+)")re");
    json listed = json::array();
    std::set<std::string> seen;
    std::istringstream lines(output);
    std::string line;
    std::string pending;
    while (std::getline(lines, line) && listed.size() < 32) {
        if (!line.empty() && line.back() == '\r') line.pop_back();
        std::smatch match;
        if (line.find("SCRIPT ERROR:") != std::string::npos && line.find("Parse Error") != std::string::npos) {
            pending = line.substr(line.find("Parse Error"));
            continue;
        }
        if (!pending.empty() && std::regex_search(line, match, at_location)) {
            const auto file = match[1].str();
            if (seen.insert(file).second) {
                listed.push_back({{"file", file}, {"line", std::stoi(match[2].str())}, {"message", pending}});
            }
            pending.clear();
            continue;
        }
        if (std::regex_search(line, match, failed_load)) {
            const auto file = match[1].str();
            if (seen.insert(file).second) {
                listed.push_back({{"file", file}, {"line", 0}, {"message", "Failed to load script"}});
            }
        }
        pending.clear();
    }
    return listed;
}

std::string tail(const std::string& output, size_t lines_wanted) {
    size_t count = 0;
    size_t at = output.size();
    while (at > 0 && count < lines_wanted) {
        const auto newline = output.rfind('\n', at - 1);
        if (newline == std::string::npos) { at = 0; break; }
        at = newline;
        ++count;
    }
    auto text = output.substr(at == 0 ? 0 : at + 1);
    if (text.size() > 4000) text = text.substr(text.size() - 4000);
    return text;
}

}  // namespace

const char* testFrameworkName(TestFramework framework) {
    return framework == TestFramework::gut ? "gut" : "gdunit4";
}

Result<TestRunRequest> parseTestRunRequest(const json& arguments) {
    if (!arguments.is_object()) return refuse(400, "project_run_tests arguments must be an object.", "arguments");
    static const std::set<std::string> known = {"framework", "paths", "name", "timeout_seconds"};
    for (const auto& [key, value] : arguments.items()) {
        (void)value;
        if (!known.count(key)) return refuse(400, "Unknown argument: " + key + ".", key);
    }
    TestRunRequest request;
    if (arguments.contains("framework")) {
        const auto& value = arguments["framework"];
        if (!value.is_string() || (value != "gut" && value != "gdunit4" && value != "auto")) {
            return refuse(400, "framework must be gut, gdunit4 or auto.", "framework");
        }
        if (value == "gut") request.framework = TestFramework::gut;
        if (value == "gdunit4") request.framework = TestFramework::gdunit4;
    }
    if (arguments.contains("paths")) {
        const auto& paths = arguments["paths"];
        if (!paths.is_array() || paths.size() > kMaxTestPaths) {
            return refuse(400, "paths must be an array of at most " + std::to_string(kMaxTestPaths) +
                                   " res:// directories or .gd files.",
                          "paths");
        }
        for (const auto& path : paths) {
            if (!path.is_string() || path.get<std::string>().rfind("res://", 0) != 0 ||
                path.get<std::string>().size() > 1024) {
                return refuse(400, "Each entry of paths must be a res:// directory or .gd file.", "paths");
            }
            request.paths.push_back(path.get<std::string>());
        }
    }
    if (arguments.contains("name")) {
        if (!arguments["name"].is_string() || !validName(arguments["name"].get<std::string>())) {
            return refuse(400, "name must be 1 to 64 letters, digits, '_' or '-', starting with a letter "
                               "or digit.",
                          "name");
        }
        request.name = arguments["name"].get<std::string>();
    }
    if (arguments.contains("timeout_seconds")) {
        if (!integerIn(arguments["timeout_seconds"], kMinTestTimeoutSeconds, kMaxTestTimeoutSeconds)) {
            return refuse(400, "timeout_seconds must be an integer from " + std::to_string(kMinTestTimeoutSeconds) +
                                   " to " + std::to_string(kMaxTestTimeoutSeconds) + ".",
                          "timeout_seconds");
        }
        request.timeout_seconds = static_cast<int>(arguments["timeout_seconds"].get<int64_t>());
    }
    return request;
}

std::vector<InstalledTestFramework> detectTestFrameworks(const fs::path& root) {
    std::vector<InstalledTestFramework> installed;
    std::error_code error;
    if (fs::is_regular_file(root / "addons" / "gut" / "gut_cmdln.gd", error)) {
        installed.push_back({TestFramework::gut, "res://addons/gut/gut_cmdln.gd",
                             pluginVersion(root / "addons" / "gut" / "plugin.cfg")});
    }
    if (fs::is_regular_file(root / "addons" / "gdUnit4" / "bin" / "GdUnitCmdTool.gd", error)) {
        installed.push_back({TestFramework::gdunit4, "res://addons/gdUnit4/bin/GdUnitCmdTool.gd",
                             pluginVersion(root / "addons" / "gdUnit4" / "plugin.cfg")});
    }
    return installed;
}

Result<InstalledTestFramework> chooseTestFramework(const TestRunRequest& request,
                                                   const std::vector<InstalledTestFramework>& installed) {
    if (request.framework) {
        for (const auto& framework : installed) {
            if (framework.framework == *request.framework) return framework;
        }
        const std::string name = testFrameworkName(*request.framework);
        return refuse(404,
                      std::string(name == "gut" ? "GUT" : "GdUnit4") + " is not in this project: there is no " +
                          (name == "gut" ? "addons/gut/gut_cmdln.gd" : "addons/gdUnit4/bin/GdUnitCmdTool.gd") +
                          ". Install it the way its documentation says, or name the framework the project has.",
                      "framework", {{"no_remedy", "Installing a test framework is the project's choice."}});
    }
    if (installed.empty()) {
        return refuse(404,
                      "Neither GUT (addons/gut) nor GdUnit4 (addons/gdUnit4) is in this project, so there is no "
                      "test runner to start.",
                      "framework", {{"no_remedy", "Installing a test framework is the project's choice."}});
    }
    if (installed.size() > 1) {
        return refuse(409,
                      "This project has both GUT and GdUnit4. Name the one to run with framework: \"gut\" or "
                      "framework: \"gdunit4\".",
                      "framework");
    }
    return installed.front();
}

Result<std::vector<std::string>> resolveTestPaths(const TestRunRequest& request,
                                                  const InstalledTestFramework& framework,
                                                  const fs::path& root) {
    std::error_code error;
    if (!request.paths.empty()) {
        for (const auto& path : request.paths) {
            const auto file = underRoot(root, path);
            const bool there = file && (fs::is_directory(*file, error) ||
                                        (fs::is_regular_file(*file, error) && file->extension() == ".gd"));
            if (!there) {
                return refuse(404, "No resource at " + path + ": paths names res:// directories or .gd files in "
                                   "this project.",
                              "paths");
            }
        }
        return request.paths;
    }
    if (fs::is_directory(root / "test", error)) return std::vector<std::string>{"res://test"};
    if (framework.framework == TestFramework::gut && fs::is_regular_file(root / ".gutconfig.json", error)) {
        // GUT reads its own configuration, which names the directories.
        return std::vector<std::string>{};
    }
    return refuse(400,
                  "There is no res://test" +
                      std::string(framework.framework == TestFramework::gut ? " and no .gutconfig.json" : "") +
                      " to find tests in. Name the directories or files with paths.",
                  "paths");
}

Result<std::string> newTestRunId(int64_t started_at_ms) {
    auto suffix = security::secureRandomHex(6);
    if (suffix.isErr()) return suffix.error();
    return std::to_string(started_at_ms) + "-" + suffix.value();
}

fs::path testRunDirectory(const fs::path& project_root, const std::string& name, const std::string& run) {
    return project_root / ".didi" / "tests" / name / run;
}

std::optional<fs::path> findTestReport(const fs::path& run_directory, TestFramework framework) {
    std::error_code error;
    if (framework == TestFramework::gut) {
        const auto file = run_directory / "results.xml";
        if (fs::is_regular_file(file, error)) return file;
        return std::nullopt;
    }
    for (fs::recursive_directory_iterator it(run_directory, error), end; !error && it != end;
         it.increment(error)) {
        if (it->path().filename() == "results.xml") return it->path();
    }
    return std::nullopt;
}

void pruneTestRuns(const fs::path& name_directory, const std::string& keep, int64_t now_ms) {
    std::error_code error;
    std::vector<fs::path> stale;
    for (fs::directory_iterator it(name_directory, error), end; !error && it != end; it.increment(error)) {
        const auto entry = it->path().filename().string();
        if (entry == keep) continue;
        // A run's directory starts with the millisecond it started. Anything
        // else is what the layout before #1242 left, which no run reads now.
        const auto dash = entry.find('-');
        int64_t started = 0;
        const bool run = dash != std::string::npos && dash > 0 &&
                         std::all_of(entry.begin(), entry.begin() + static_cast<std::ptrdiff_t>(dash),
                                     [](unsigned char c) { return std::isdigit(c) != 0; });
        if (run) {
            try {
                started = std::stoll(entry.substr(0, dash));
            } catch (...) {
                started = 0;
            }
            if (now_ms - started < kTestRunRetentionMs) continue;
        }
        stale.push_back(it->path());
    }
    for (const auto& path : stale) fs::remove_all(path, error);
}

std::vector<std::string> testCommandArguments(const InstalledTestFramework& framework,
                                              const std::vector<std::string>& paths,
                                              const std::string& report) {
    std::vector<std::string> arguments = {"-s", framework.entry};
    if (framework.framework == TestFramework::gut) {
        // -gexit is what a headless GUT does anyway; saying so keeps a project
        // whose .gutconfig.json says otherwise from waiting for a window.
        arguments.push_back("-gexit");
        std::vector<std::string> dirs, files;
        for (const auto& path : paths) {
            (path.size() > 3 && path.compare(path.size() - 3, 3, ".gd") == 0 ? files : dirs).push_back(path);
        }
        if (!dirs.empty()) {
            std::string joined;
            for (const auto& dir : dirs) joined += (joined.empty() ? "" : ",") + dir;
            arguments.push_back("-gdir=" + joined);
            arguments.push_back("-ginclude_subdirs");
        }
        if (!files.empty()) {
            std::string joined;
            for (const auto& file : files) joined += (joined.empty() ? "" : ",") + file;
            arguments.push_back("-gtest=" + joined);
        }
        arguments.push_back("-gjunit_xml_file=" + report);
    } else {
        // GdUnit4 refuses a headless run unless told it may (exit 103).
        arguments.push_back("--ignoreHeadlessMode");
        for (const auto& path : paths) {
            arguments.push_back("-a");
            arguments.push_back(path);
        }
        arguments.push_back("-rd");
        arguments.push_back(report);
        arguments.push_back("-rc");
        arguments.push_back("1");
    }
    return arguments;
}

Result<JUnitReport> parseJUnitReport(std::string_view xml) {
    if (xml.size() > kMaxReportBytes) {
        return Error(413, "The test report is larger than the " + std::to_string(kMaxReportBytes / (1024 * 1024)) +
                              " MiB this reads.");
    }
    auto report = JUnitScanner(xml).scan();
    if (report.isErr()) return report;
    for (auto& test : report.value().cases) locate(test);
    return report;
}

TestRunVerdict judgeTestRun(const TestRunFacts& facts) {
    TestRunVerdict verdict;
    const bool gut = facts.framework.framework == TestFramework::gut;
    json counts = {{"total", 0}, {"passed", 0}, {"failed", 0}, {"errors", 0}, {"skipped", 0},
                   {"no_assertions", 0}};
    json tests = json::array();
    json failed = json::array();
    bool truncated = false;
    std::optional<JUnitReport> report;
    std::string report_problem;
    if (facts.report_xml) {
        auto parsed = parseJUnitReport(*facts.report_xml);
        if (parsed.isOk()) {
            report = std::move(parsed.value());
        } else {
            report_problem = parsed.error().message;
        }
    }
    if (report) {
        for (const auto& test : report->cases) {
            counts["total"] = counts["total"].get<int>() + 1;
            const char* bucket = test.outcome == "passed" ? "passed"
                                 : test.outcome == "failed" ? "failed"
                                 : test.outcome == "error" ? "errors"
                                 : test.outcome == "skipped" ? "skipped"
                                                             : "no_assertions";
            counts[bucket] = counts[bucket].get<int>() + 1;
            json entry = {{"suite", test.suite}, {"name", test.name}, {"outcome", test.outcome},
                          {"seconds", test.seconds}};
            if (!test.message.empty()) {
                entry["message"] = test.message.size() > 2000 ? test.message.substr(0, 1997) + "..." : test.message;
            }
            if (!test.file.empty()) entry["file"] = test.file;
            if (test.line > 0) entry["line"] = test.line;
            if (test.outcome == "failed" || test.outcome == "error") {
                failed.push_back(test.suite + " :: " + test.name);
            }
            if (tests.size() < kMaxReportedTests) {
                tests.push_back(std::move(entry));
            } else {
                truncated = true;
            }
        }
    }
    // A test script, or project code, that did not load can take tests out of
    // the report without a word. An addon's own script that did not load is
    // the framework's or a plugin's business unless it is what is being
    // tested: GUT 9.7.1 has two that do not parse on 4.5.1 and 4.6.2
    // (godot_singletons.gd names AccessibilityServer), and its tests run all
    // the same.
    json did_not_load = json::array();
    json addon_did_not_load = json::array();
    for (auto& script : scriptsThatDidNotLoad(facts.output)) {
        const auto file = script.value("file", std::string());
        const bool tested = std::any_of(facts.paths.begin(), facts.paths.end(), [&](const std::string& path) {
            if (file == path) return true;
            const auto prefix = path.back() == '/' ? path : path + "/";
            return file.rfind(prefix, 0) == 0;
        });
        const bool addon = file.rfind("res://addons/", 0) == 0;
        (addon && !tested ? addon_did_not_load : did_not_load).push_back(std::move(script));
    }
    const int total = counts["total"].get<int>();
    const int passed = counts["passed"].get<int>();
    const int bad = counts["failed"].get<int>() + counts["errors"].get<int>();

    // What the exit code says, in each framework's own terms, beside what the
    // report says. GUT exits 1 when a test failed and 0 otherwise; GdUnit4
    // exits 100 on a failure, 101 on warnings, and 103 to 105 when it could not
    // run at all.
    std::optional<bool> exit_says_failed;
    if (gut) {
        if (facts.exit_code == 0) exit_says_failed = false;
        if (facts.exit_code == 1) exit_says_failed = true;
    } else {
        if (facts.exit_code == 0 || facts.exit_code == 101) exit_says_failed = false;
        if (facts.exit_code == 100) exit_says_failed = true;
    }

    const auto set = [&](std::string v, std::string reason, std::string summary) {
        verdict.verdict = std::move(v);
        verdict.reason = std::move(reason);
        verdict.summary = std::move(summary);
    };
    const std::string framework_name = gut ? "GUT" : "GdUnit4";
    if (facts.cancelled) {
        set("error", "cancelled", "The job running the tests was cancelled, so the run was stopped.");
    } else if (facts.timed_out) {
        set("error", "timeout", "The tests ran past timeout_seconds and were stopped.");
    } else if (!facts.report_xml) {
        std::string why = framework_name + " exited " + std::to_string(facts.exit_code) + " and wrote no report";
        if (facts.output.find("have not been imported") != std::string::npos ||
            facts.output.find("Could not find type \"GdUnit") != std::string::npos) {
            why += ": its classes are not imported in this project yet. Open the project in the Godot editor "
                   "once, or run godot --headless --import in it, then run the tests again";
            verdict.report["not_imported"] = true;
        } else if (facts.output.find("No test cases found") != std::string::npos) {
            set("error", "no_tests", framework_name + " found no tests to run in " +
                                         (facts.paths.empty() ? std::string("its configured directories")
                                                              : std::string("the paths given")) +
                                         ", so nothing was proved.");
        } else if (!did_not_load.empty()) {
            // GdUnit4 stops on a test script that does not parse (exit 105)
            // and writes nothing.
            set("error", "scripts_did_not_load",
                std::to_string(did_not_load.size()) + " script(s) did not load and " + framework_name +
                    " wrote no report: " + did_not_load[0].value("file", std::string()) + " first.");
        }
        if (verdict.verdict.empty()) set("error", "no_report", why + ".");
    } else if (!report) {
        set("error", "report_unreadable", "The report " + framework_name + " wrote could not be read: " +
                                              report_problem);
    } else if (!did_not_load.empty()) {
        set(bad > 0 ? "fail" : "error", "scripts_did_not_load",
            std::to_string(did_not_load.size()) + " script(s) did not load, so their tests are not in the "
            "report: " + did_not_load[0].value("file", std::string()) + " first.");
    } else if (total == 0) {
        set("error", "no_tests", framework_name + " ran no tests, so nothing was proved.");
    } else if (exit_says_failed.has_value() && *exit_says_failed != (bad > 0)) {
        set("error", "exit_code_disagrees",
            framework_name + " exited " + std::to_string(facts.exit_code) + ", which says " +
                (*exit_says_failed ? "a test failed" : "every test passed") + ", and its report has " +
                std::to_string(bad) + " failing test(s).");
    } else if (!exit_says_failed.has_value()) {
        set("error", "exit_code_disagrees",
            framework_name + " exited " + std::to_string(facts.exit_code) +
                ", which is neither a pass nor a failure: it could not run the tests as asked.");
    } else if (bad > 0) {
        set("fail", "tests_failed", std::to_string(bad) + " of " + std::to_string(total) + " tests failed: " +
                                        failed[0].get<std::string>() + " first.");
    } else if (passed == 0) {
        set("error", "nothing_proved", "No test passed: every one of the " + std::to_string(total) +
                                           " was skipped or asserted nothing.");
    } else if (facts.output_truncated) {
        // Godot's output is kept to its first MiB, and a script that did not
        // load can be named after that. GUT leaves its tests out of the report
        // and exits 0, so nothing else would show it (#1243).
        set("error", "output_truncated",
            "Godot printed more than this run keeps, so a script that did not load could be named in the part "
            "that was dropped. Have the tests print less, or run fewer paths at a time.");
    } else {
        set("pass", "", std::to_string(passed) + " of " + std::to_string(total) + " tests passed and none failed" +
                            (counts["skipped"].get<int>() + counts["no_assertions"].get<int>() > 0
                                 ? "; the rest were skipped or asserted nothing."
                                 : "."));
    }

    verdict.report["framework"] = {{"name", testFrameworkName(facts.framework.framework)},
                                   {"version", facts.framework.version.empty() ? json(nullptr)
                                                                               : json(facts.framework.version)}};
    verdict.report["verdict"] = verdict.verdict;
    verdict.report["summary"] = verdict.summary;
    verdict.report["reason"] = verdict.reason.empty() ? json(nullptr) : json(verdict.reason);
    verdict.report["paths"] = facts.paths;
    verdict.report["exit_code"] = facts.exit_code;
    verdict.report["counts"] = counts;
    verdict.report["tests"] = tests;
    verdict.report["failed_tests"] = failed;
    verdict.report["scripts_did_not_load"] = did_not_load;
    verdict.report["addon_scripts_did_not_load"] = addon_did_not_load;
    verdict.report["truncated"] = truncated || facts.output_truncated;
    if (verdict.verdict != "pass") verdict.report["output_tail"] = tail(facts.output, 40);
    return verdict;
}

}  // namespace didi::offline
