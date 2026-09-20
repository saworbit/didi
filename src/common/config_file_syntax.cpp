#include "didi/common/config_file_syntax.hpp"

#include <cctype>

namespace didi::config_file {
namespace {

bool isSpace(char character) {
    return std::isspace(static_cast<unsigned char>(character)) != 0;
}

// How far through a value the walk is. A value is finished when it is not
// inside a string, every bracket it opened is closed, and it holds something.
struct ValueState {
    int depth{0};
    bool in_string{false};
    bool seen{false};
};

// Walks one line of value text. `end` is set to the offset just past what this
// line contributes to the value, which is where the engine carries on reading.
// Returns true when the value ends on this line.
bool advanceValue(std::string_view text, ValueState& state, size_t& end) {
    size_t index = 0;
    for (; index < text.size(); ++index) {
        const char character = text[index];
        if (state.in_string) {
            if (character == '\\') {
                ++index;
                continue;
            }
            if (character == '"') {
                state.in_string = false;
                // A bare string is the whole value, so the closing quote ends
                // it. Inside brackets it is one element and the value carries
                // on.
                if (state.depth == 0) {
                    ++index;
                    break;
                }
            }
            continue;
        }
        // `;` comments out the rest of the line wherever it is, not only
        // where a value could end. `name="a" ; note` is the string a, a `;`
        // before the value has started is a comment the value carries on
        // past, and a `;` inside a dictionary is dropped with the `}` on a
        // later line still closing it. Checked on 4.5.1, 4.6.2 and 4.7.2.
        if (character == ';') break;
        if (character == '"') {
            state.in_string = true;
            state.seen = true;
            continue;
        }
        if (character == '[' || character == '{' || character == '(') {
            ++state.depth;
            state.seen = true;
            continue;
        }
        if (character == ']' || character == '}' || character == ')') {
            if (state.depth > 0) --state.depth;
            state.seen = true;
            if (state.depth == 0) {
                ++index;
                break;
            }
            continue;
        }
        if (isSpace(character)) {
            // A bare token ends at the first space after it. `a=1 b=2` is two
            // settings on one line, and `name="a" note` is the value a
            // followed by key text.
            if (state.depth == 0 && state.seen) break;
            continue;
        }
        state.seen = true;
    }
    end = index;
    return !state.in_string && state.depth == 0 && state.seen;
}

// Appends one line's key tokens to `pending` and returns the offset of the `=`
// that closes the key, or npos when this line does not hold one.
size_t appendKeyTokens(std::string_view text, std::string& pending) {
    size_t index = 0;
    while (index < text.size()) {
        const char character = text[index];
        if (character == '=') return index;
        if (character == '"') {
            std::string content;
            ++index;
            bool closed = false;
            while (index < text.size()) {
                if (text[index] == '\\' && index + 1 < text.size()) {
                    content += text[index + 1];
                    index += 2;
                    continue;
                }
                if (text[index] == '"') {
                    closed = true;
                    ++index;
                    break;
                }
                content += text[index];
                ++index;
            }
            // A quoted token becomes the key so far rather than joining onto
            // it: `# note "a=b"` above `name="a"` registers `a=bname`, not
            // `#notea=bname`. Checked on 4.5.1, 4.6.2 and 4.7.2.
            pending = content;
            if (!closed) return std::string::npos;
            continue;
        }
        if (!isSpace(character)) pending += character;
        ++index;
    }
    return std::string::npos;
}

std::string_view trimmedView(std::string_view text) {
    size_t first = 0;
    while (first < text.size() && isSpace(text[first])) ++first;
    size_t last = text.size();
    while (last > first && isSpace(text[last - 1])) --last;
    return text.substr(first, last - first);
}

// Adds one line's contribution to a value that may span several.
//
// Godot writes a dictionary over four lines and reads them as one value, so
// the text of that value is those lines joined rather than the first of them.
// The pieces are trimmed and joined with a newline, which is the file's own
// text for anything the engine wrote.
void appendValueText(std::string& value_text, std::string_view piece) {
    const auto trimmed = trimmedView(piece);
    if (trimmed.empty()) return;
    if (!value_text.empty()) value_text += '\n';
    value_text.append(trimmed);
}

} // namespace

Scan scan(std::string_view text) {
    Scan result;
    std::string pending;
    int pending_line = 0;
    std::string section;
    ValueState value;
    bool in_value = false;

    size_t cursor = 0;
    int number = 0;
    while (cursor <= text.size()) {
        const auto newline = text.find('\n', cursor);
        auto raw = text.substr(cursor, newline == std::string_view::npos ? std::string_view::npos
                                                                         : newline - cursor);
        if (!raw.empty() && raw.back() == '\r') raw.remove_suffix(1);
        ++number;

        // Where on this line the engine is reading. A value that ends part-way
        // through a line does not end the line: the text after it is read the
        // same way the text on a line of its own would be. `name="a" # note`
        // registers `#noteplatform` for the `platform=` line below it, and
        // `name="a" [t]` opens the section `t`. Checked on 4.5.1, 4.6.2 and
        // 4.7.2.
        size_t offset = 0;
        if (in_value) {
            size_t end = 0;
            const bool finished = advanceValue(raw, value, end);
            if (!result.entries.empty()) {
                result.entries.back().value_end_line = number;
                appendValueText(result.entries.back().value_text, raw.substr(0, end));
            }
            if (!finished) {
                if (newline == std::string_view::npos) break;
                cursor = newline + 1;
                continue;
            }
            in_value = false;
            offset = end;
        }

        while (offset <= raw.size()) {
            const auto line = trimmedView(raw.substr(offset));
            if (line.empty() || line.front() == ';') {
                // A `;` line is a comment wherever a key could start, including
                // partway through one: `# note`, `; real`, `name="a"` registers
                // `#notename`.
                break;
            }
            if (pending.empty() && line.size() >= 2 && line.front() == '[' && line.back() == ']') {
                section = std::string(trimmedView(line.substr(1, line.size() - 2)));
                result.headers.push_back({section, number});
                break;
            }
            if (pending.empty()) pending_line = number;
            std::string on_this_line;
            appendKeyTokens(line, on_this_line);
            const auto equals = appendKeyTokens(line, pending);
            if (equals == std::string_view::npos) break;

            Entry entry;
            entry.section = section;
            entry.key = pending;
            entry.key_on_line = on_this_line;
            entry.key_line = pending_line;
            entry.line = number;
            entry.value_end_line = number;
            entry.joined = pending_line != number;
            result.entries.push_back(std::move(entry));
            pending.clear();
            value = ValueState{};

            // `line` is a view into `raw`, so the value starts this far along
            // the raw line and the walk carries on from wherever it ends.
            const auto value_begin =
                static_cast<size_t>(line.data() - raw.data()) + equals + 1;
            size_t end = 0;
            const bool finished = advanceValue(raw.substr(value_begin), value, end);
            appendValueText(result.entries.back().value_text,
                            raw.substr(value_begin, end));
            if (!finished) {
                in_value = true;
                break;
            }
            offset = value_begin + end;
        }

        if (newline == std::string_view::npos) break;
        cursor = newline + 1;
    }

    // A key left open at the end of the file is dropped and load() still
    // returns OK, so a trailing note is not a broken file. A value left open is
    // ERR_PARSE_ERROR and nothing in the file loads.
    result.complete = !in_value;
    result.trailing_key = !pending.empty();
    return result;
}

} // namespace didi::config_file
