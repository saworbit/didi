#include "didi/common/config_file_syntax.hpp"

#include <cstdint>
#include <cstdlib>

#include <cctype>

namespace didi::config_file {
namespace {

bool isSpace(char character) {
    return std::isspace(static_cast<unsigned char>(character)) != 0;
}

int hexValue(char character) {
    if (character >= '0' && character <= '9') return character - '0';
    if (character >= 'a' && character <= 'f') return character - 'a' + 10;
    if (character >= 'A' && character <= 'F') return character - 'A' + 10;
    return -1;
}

// The UTF-8 decode Godot runs on a string when its closing quote is read. A
// byte that does not begin a well-formed sequence becomes U+FFFD and the decode
// carries on from the byte after it, rather than from the end of the sequence
// it started: an overlong pair is two replacement characters, the three bytes
// of a UTF-16 surrogate are three, and a four-byte sequence cut short after its
// third byte is three. A byte order mark is dropped only where it leads the
// string. Measured on 4.5.1, 4.6.2 and 4.7.2 with each byte written as an
// escape, which is the one way a file can put an arbitrary byte in a string.
std::string decodeUtf8TheWayGodotDoes(const std::string& bytes) {
    static constexpr char kReplacement[] = "\xEF\xBF\xBD";
    std::string out;
    out.reserve(bytes.size());
    size_t at = 0;
    if (bytes.size() >= 3 && bytes.compare(0, 3, "\xEF\xBB\xBF") == 0) at = 3;
    while (at < bytes.size()) {
        const auto lead = static_cast<unsigned char>(bytes[at]);
        if (lead < 0x80) {
            out += bytes[at++];
            continue;
        }
        size_t length = 0;
        uint32_t code = 0;
        if ((lead & 0xE0) == 0xC0) {
            length = 2;
            code = lead & 0x1F;
        } else if ((lead & 0xF0) == 0xE0) {
            length = 3;
            code = lead & 0x0F;
        } else if ((lead & 0xF8) == 0xF0) {
            length = 4;
            code = lead & 0x07;
        }
        bool valid = length != 0 && at + length <= bytes.size();
        for (size_t i = 1; valid && i < length; ++i) {
            const auto next = static_cast<unsigned char>(bytes[at + i]);
            if ((next & 0xC0) != 0x80) {
                valid = false;
            } else {
                code = (code << 6) | (next & 0x3F);
            }
        }
        if (valid) {
            static constexpr uint32_t kSmallest[] = {0, 0, 0x80, 0x800, 0x10000};
            valid = code >= kSmallest[length] && code <= 0x10FFFF &&
                    !(code >= 0xD800 && code <= 0xDFFF);
        }
        if (valid) {
            out.append(bytes, at, length);
            at += length;
        } else {
            out += kReplacement;
            ++at;
        }
    }
    return out;
}

// One quoted string, read from just past its opening quote. See stringValue in
// the header for every rule, each measured on the three lines.
struct DecodedString {
    std::string text;
    std::string problem;
    size_t end{0};        // just past the closing quote, or the end of the text
    bool closed{false};
};

DecodedString decodeString(std::string_view text, size_t at) {
    DecodedString decoded;
    const auto refuse = [&decoded](const char* reason) {
        if (decoded.problem.empty()) decoded.problem = reason;
    };
    constexpr const char* kUnpairedLead =
        "the string has a UTF-16 lead surrogate escape with no trail surrogate escape after it";
    // The file's bytes and the escaped units, before the decode that runs when
    // the string closes.
    std::string bytes;
    uint32_t lead = 0;
    while (at < text.size()) {
        const char character = text[at++];
        if (character == '"') {
            decoded.closed = true;
            break;
        }
        if (character != '\\') {
            if (lead != 0) refuse(kUnpairedLead);
            bytes += character;
            continue;
        }
        // A backslash at the very end is an unterminated value, which is
        // Scan::complete's answer rather than this one.
        if (at >= text.size()) break;
        const char next = text[at++];
        uint32_t unit = static_cast<unsigned char>(next);
        switch (next) {
            case 'b': unit = 0x08; break;
            case 't': unit = 0x09; break;
            case 'n': unit = 0x0A; break;
            case 'f': unit = 0x0C; break;
            case 'r': unit = 0x0D; break;
            case 'u':
            case 'U': {
                const int digits = next == 'U' ? 6 : 4;
                unit = 0;
                for (int i = 0; i < digits && at < text.size(); ++i) {
                    const int value = hexValue(text[at]);
                    if (value < 0) {
                        // Not consumed, so a closing quote that came too soon
                        // still closes the string.
                        refuse(next == 'U'
                                   ? "the string has a \\U escape without six hex digits after it"
                                   : "the string has a \\u escape without four hex digits after it");
                        break;
                    }
                    unit = (unit << 4) | static_cast<uint32_t>(value);
                    ++at;
                }
                break;
            }
            default:
                // Any other character is itself: \" \\ \' and \/, and \q, \0,
                // \a and \v with it. A multi-byte character after the
                // backslash contributes its first byte here and the rest as
                // ordinary bytes, which is how the parser reads it too.
                break;
        }
        if ((unit & 0xFFFFFC00u) == 0xD800u) {
            if (lead != 0) refuse(kUnpairedLead);
            lead = unit;
            continue;
        }
        if ((unit & 0xFFFFFC00u) == 0xDC00u) {
            if (lead == 0) {
                refuse("the string has a UTF-16 trail surrogate escape with no lead surrogate "
                       "escape before it");
                continue;
            }
            unit = (lead << 10) + unit - ((0xD800u << 10) + 0xDC00u - 0x10000u);
            lead = 0;
        }
        if (lead != 0) refuse(kUnpairedLead);
        // An escape is one byte of what the closing decode reads. Anything
        // that does not fit in a byte becomes a space, and so does zero.
        bytes += (unit == 0 || unit > 0xFF) ? ' ' : static_cast<char>(unit);
    }
    if (decoded.closed && lead != 0) refuse(kUnpairedLead);
    decoded.end = at;
    if (decoded.problem.empty()) decoded.text = decodeUtf8TheWayGodotDoes(bytes);
    return decoded;
}

bool isIdentifierStart(char character) {
    return std::isalpha(static_cast<unsigned char>(character)) != 0 || character == '_';
}

bool isIdentifierChar(char character) {
    return std::isalnum(static_cast<unsigned char>(character)) != 0 || character == '_';
}

// The only identifiers that are a value on their own. Everything else the
// parser reads as an identifier has to be followed by a `(` or a `[`. The
// match is exact: `nil` is null's other spelling, and `True`, `False` and
// `NAN` are err 43 on 4.5.1, 4.6.2 and 4.7.2.
bool isValueKeyword(std::string_view word) {
    return word == "true" || word == "false" || word == "null" || word == "nil" ||
           word == "nan" || word == "inf" || word == "inf_neg";
}

size_t nextNonSpace(std::string_view text, size_t from) {
    while (from < text.size() && isSpace(text[from])) ++from;
    return from;
}

// How far through a value the walk is. A value is finished when it is not
// inside a string, every bracket it opened is closed, it holds something, and
// it is not an identifier still waiting to see whether it names a call.
struct ValueState {
    int depth{0};
    bool in_string{false};
    bool seen{false};
    // The run being read at depth 0 is an identifier. An identifier is a value
    // on its own only for the seven keywords; otherwise the parser skips
    // whitespace to find the `(` or `[` after it.
    bool identifier{false};
    // An identifier, or the `[type]` after one, has ended at depth 0 and a `(`
    // or `[` may still follow -- possibly on the next line. Until that is
    // settled the value is not over.
    bool awaiting_call{false};
    // The brackets being counted are the `[int]` of `Array[int]([1, 2])`, so
    // closing them returns to waiting for the call rather than ending the
    // value.
    bool annotation{false};
};

// Walks one line of value text. `end` is set to the offset just past what this
// line contributes to the value, which is where the engine carries on reading.
// Returns true when the value ends on this line.
bool advanceValue(std::string_view text, ValueState& state, size_t& end) {
    size_t index = 0;
    // Where the identifier being read starts, so the decision below can ask
    // what it says. A keyword is a whole value and the parser returns on it
    // without looking further: `a=true` with `[t]` under it is the boolean and
    // then a section, where `a=Vector2` with `(1, 2)` under it is one Vector2.
    size_t word_start = std::string_view::npos;
    const auto readingKeyword = [&](size_t word_end) {
        return word_start != std::string_view::npos &&
               isValueKeyword(text.substr(word_start, word_end - word_start));
    };
    if (state.awaiting_call) {
        // The previous line ended after an identifier, so whether the value is
        // over is decided by what comes next. `Vector2` with `(1, 2)` below it
        // is one Vector2; `true` with `b=3` below it is the boolean and then a
        // key.
        index = nextNonSpace(text, 0);
        if (index >= text.size()) {
            end = text.size();
            return false;
        }
        if (text[index] != '(' && text[index] != '[') {
            // The value ended on an earlier line. This line contributes
            // nothing to it and is read from its start as key text.
            end = 0;
            return true;
        }
        state.awaiting_call = false;
    }
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
            state.identifier = false;
            continue;
        }
        if (character == '[' || character == '{' || character == '(') {
            if (state.depth == 0) {
                // A `[` straight after an identifier is the type of a typed
                // container, not an array: `Array[int]([1, 2])` carries on to
                // the call after the `]`.
                state.annotation = character == '[' && (state.identifier || state.awaiting_call);
                state.awaiting_call = false;
            }
            ++state.depth;
            state.seen = true;
            state.identifier = false;
            continue;
        }
        if (character == ']' || character == '}' || character == ')') {
            if (state.depth > 0) --state.depth;
            state.seen = true;
            state.identifier = false;
            if (state.depth == 0) {
                if (state.annotation) {
                    state.annotation = false;
                    state.awaiting_call = true;
                    continue;
                }
                ++index;
                break;
            }
            continue;
        }
        if (isSpace(character)) {
            // A bare token ends at the first space after it. `a=1 b=2` is two
            // settings on one line, and `name="a" note` is the value a
            // followed by key text. An identifier is the exception: the parser
            // skips whitespace to find the `(` or `[` that follows it.
            if (state.depth == 0 && state.seen) {
                if ((state.identifier && !readingKeyword(index)) || state.awaiting_call) {
                    const size_t look = nextNonSpace(text, index);
                    if (look >= text.size()) {
                        state.identifier = false;
                        state.awaiting_call = true;
                        end = text.size();
                        return false;
                    }
                    if (text[look] == '(' || text[look] == '[') {
                        state.identifier = false;
                        state.awaiting_call = true;
                        index = look - 1;
                        continue;
                    }
                }
                break;
            }
            continue;
        }
        if (state.depth == 0) {
            if (state.seen) {
                state.identifier = state.identifier && isIdentifierChar(character);
            } else {
                state.identifier = isIdentifierStart(character);
                if (state.identifier) word_start = index;
            }
        }
        state.seen = true;
    }
    if (index >= text.size() && state.depth == 0 && !state.in_string && state.identifier &&
        !readingKeyword(index)) {
        // The identifier runs to the end of the line with no space after it.
        // The line below decides whether it was the whole value. Only when the
        // walk reached the end: a value that stopped at a space or a `;` has
        // already had its answer.
        state.identifier = false;
        state.awaiting_call = true;
    }
    end = index;
    return !state.in_string && state.depth == 0 && state.seen && !state.awaiting_call;
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

// Walks one value the way the parser walks it, far enough to find the failures
// a bracket count cannot see. Every rule here is a shape the engine answers
// with err 43 on 4.5.1, 4.6.2 and 4.7.2; anything it is not sure about, it
// accepts. See valueProblem's header comment for what that leaves out.
struct ValueWalk {
    std::string_view text;
    size_t at{0};
    std::string problem;

    bool done() const { return at >= text.size(); }
    char peek() const { return at < text.size() ? text[at] : '\0'; }
    void skipSpace() { at = nextNonSpace(text, at); }

    void refuse(std::string reason) {
        if (problem.empty()) problem = std::move(reason);
    }

    // `at` is on the opening quote. The parser reads a string's escapes as it
    // goes, so one it cannot read is err 43 for the whole file wherever the
    // string is, a constructor's arguments included.
    void skipString() {
        const auto decoded = decodeString(text, at + 1);
        if (!decoded.problem.empty()) refuse(decoded.problem);
        at = decoded.end;
    }

    // `at` is on the opening bracket. Strings inside are skipped whole so a
    // bracket in one does not count.
    void skipBalanced() {
        int depth = 0;
        while (at < text.size()) {
            const char character = text[at];
            if (character == '"') {
                skipString();
                continue;
            }
            if (character == '[' || character == '{' || character == '(') ++depth;
            if (character == ']' || character == '}' || character == ')') {
                --depth;
                if (depth <= 0) {
                    ++at;
                    return;
                }
            }
            ++at;
        }
    }

    void readValue(int depth) {
        if (!problem.empty()) return;
        if (depth > 32) {
            // Deeper than anything the engine writes. Nothing below here is
            // proven either way, so stop rather than guess.
            at = text.size();
            return;
        }
        skipSpace();
        if (done()) {
            refuse("the value is empty");
            return;
        }
        const char character = peek();
        if (character == ')' || character == ']' || character == '}' || character == ',' ||
            character == ':' || character == '(') {
            refuse(std::string("a value cannot begin with '") + character + "'");
            return;
        }
        if (character == '"') {
            skipString();
            return;
        }
        if (character == '&' || character == '@') {
            ++at;
            if (peek() != '"') {
                refuse(std::string("'") + character +
                       "' begins a StringName and has to be followed straight away by a quoted "
                       "string");
                return;
            }
            skipString();
            return;
        }
        if (isIdentifierStart(character)) {
            const size_t start = at;
            while (at < text.size() && isIdentifierChar(text[at])) ++at;
            const auto word = text.substr(start, at - start);
            const size_t after_word = at;
            skipSpace();
            if (peek() == '[') {
                skipBalanced();
                skipSpace();
                if (peek() == '(') skipBalanced();
                return;
            }
            if (peek() == '(') {
                skipBalanced();
                return;
            }
            at = after_word;
            if (isValueKeyword(word)) return;
            refuse("Godot has no value named '" + std::string(word) + "'");
            return;
        }
        if (character == '[') {
            readArray(depth);
            return;
        }
        if (character == '{') {
            readDictionary(depth);
            return;
        }
        // A number, a `#rrggbb` colour, or anything else the parser reads as
        // one token. Whether it is well formed needs the engine's own number
        // grammar, so this only finds where it ends.
        while (at < text.size() && !isSpace(text[at]) &&
               std::string_view(",:)]}").find(text[at]) == std::string_view::npos) {
            ++at;
        }
    }

    void readArray(int depth) {
        ++at;
        while (true) {
            skipSpace();
            // An unterminated value is Scan::complete's answer, not this one.
            if (done()) return;
            if (peek() == ']') {
                ++at;
                return;
            }
            readValue(depth + 1);
            if (!problem.empty()) return;
            skipSpace();
            if (done()) return;
            if (peek() == ',') {
                // A trailing comma is allowed: `[1,]` loads.
                ++at;
                continue;
            }
            if (peek() == ']') {
                ++at;
                return;
            }
            refuse(std::string("a ',' or a ']' has to follow an array element, not '") + peek() +
                   "'");
            return;
        }
    }

    void readDictionary(int depth) {
        ++at;
        while (true) {
            skipSpace();
            if (done()) return;
            if (peek() == '}') {
                ++at;
                return;
            }
            readValue(depth + 1);
            if (!problem.empty()) return;
            skipSpace();
            if (done()) return;
            if (peek() != ':') {
                refuse(std::string("a ':' has to follow a dictionary key, not '") + peek() + "'");
                return;
            }
            ++at;
            readValue(depth + 1);
            if (!problem.empty()) return;
            skipSpace();
            if (done()) return;
            if (peek() == ',') {
                ++at;
                continue;
            }
            if (peek() == '}') {
                ++at;
                return;
            }
            refuse(std::string("a ',' or a '}' has to follow a dictionary value, not '") + peek() +
                   "'");
            return;
        }
    }
};

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
            // A line the value did not reach into is not part of it. An
            // identifier waiting to see whether a call follows it consumes
            // nothing from the line that turns out to hold the next key, and
            // counting that line would put it inside the span a rewrite
            // replaces.
            if (!result.entries.empty() && end > 0) {
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
    //
    // An identifier still waiting for a call is not a value left open: the
    // file ended, so nothing follows it, and the identifier was the whole
    // value. `enabled=true` as the last line is a complete file.
    result.complete = !in_value || value.awaiting_call;
    result.trailing_key = !pending.empty();
    return result;
}

std::string valueProblem(std::string_view value_text) {
    ValueWalk walk;
    walk.text = value_text;
    walk.readValue(0);
    // Text after the value is not this value's problem. The scan hands each
    // entry only the span its own value covers, and reads whatever follows as
    // the next key (#816).
    return walk.problem;
}

std::optional<LoadFailure> loadFailure(const Scan& scanned) {
    // A file that ends part-way through a *key* is not this. The engine drops
    // that text and load() returns OK, which is the ordinary trailing note, so
    // trailing_key is not consulted.
    if (!scanned.complete) {
        LoadFailure failure;
        failure.unterminated = true;
        // The last key read is the one whose value never closed, which is the
        // line to repair. Naming it is the difference between a verdict and a
        // remedy.
        if (!scanned.entries.empty()) {
            failure.section = scanned.entries.back().section;
            failure.key = scanned.entries.back().key;
            failure.line = scanned.entries.back().line;
        }
        return failure;
    }
    // Balanced is not loadable. `config/broken=)` closes every bracket it opens
    // and is still err 43, so a reader that stopped at `complete` answered out
    // of a file that does not load (#820).
    for (const auto& entry : scanned.entries) {
        auto problem = valueProblem(entry.value_text);
        if (problem.empty()) continue;
        LoadFailure failure;
        failure.section = entry.section;
        failure.key = entry.key;
        failure.line = entry.line;
        failure.value_reason = std::move(problem);
        return failure;
    }
    return std::nullopt;
}

bool booleanize(std::string_view value_text) {
    const auto text = strings::trim(value_text);
    if (text == "false" || text == "null" || text == "nil") return false;
    if (text.empty()) return false;
    // A number decides on being zero. strtod is safe here: the locale is never
    // changed, so the decimal point is always `.`.
    char* end = nullptr;
    const std::string owned(text);
    const double number = std::strtod(owned.c_str(), &end);
    if (end != owned.c_str() && *end == '\0') return number != 0.0;
    return true;
}

namespace {

// The longest decimal number at the start of text: a sign, digits with at most
// one point, and an exponent only when digits follow it. Nothing else counts,
// so hex, inf, nan and digit separators stop it, the way they stop Godot.
double decimalPrefix(std::string_view text) {
    size_t at = 0;
    if (at < text.size() && (text[at] == '+' || text[at] == '-')) ++at;
    bool any_digit = false;
    while (at < text.size() && std::isdigit(static_cast<unsigned char>(text[at]))) {
        ++at;
        any_digit = true;
    }
    if (at < text.size() && text[at] == '.') {
        ++at;
        while (at < text.size() && std::isdigit(static_cast<unsigned char>(text[at]))) {
            ++at;
            any_digit = true;
        }
    }
    if (!any_digit) return 0.0;
    size_t end = at;
    if (at < text.size() && (text[at] == 'e' || text[at] == 'E')) {
        size_t exponent = at + 1;
        if (exponent < text.size() && (text[exponent] == '+' || text[exponent] == '-')) ++exponent;
        if (exponent < text.size() && std::isdigit(static_cast<unsigned char>(text[exponent]))) {
            while (exponent < text.size() &&
                   std::isdigit(static_cast<unsigned char>(text[exponent]))) {
                ++exponent;
            }
            end = exponent;
        }
    }
    // strtod over exactly the prefix found, so its reading of the digits is the
    // only thing used. The locale is never changed, so the point is `.`.
    const std::string owned(text.substr(0, end));
    const double value = std::strtod(owned.c_str(), nullptr);
    // -0.0 reads back from the engine as 0.0.
    return value == 0.0 ? 0.0 : value;
}

} // namespace

double floatize(std::string_view value_text) {
    const auto text = strings::trim(value_text);
    if (text == "true") return 1.0;
    if (text.empty() || text.front() == '&' || text.front() == '@') return 0.0;
    if (text.front() == '"') {
        const auto decoded = stringValue(text);
        if (!decoded) return 0.0;
        const std::string_view body = decoded->text;
        size_t start = 0;
        while (start < body.size() && std::isspace(static_cast<unsigned char>(body[start]))) ++start;
        return decimalPrefix(body.substr(start));
    }
    return decimalPrefix(text);
}

std::optional<StringValue> stringValue(std::string_view value_text) {
    const auto text = trimmedView(value_text);
    size_t quote = 0;
    if (!text.empty() && (text.front() == '&' || text.front() == '@')) quote = 1;
    if (quote >= text.size() || text[quote] != '"') return std::nullopt;
    auto decoded = decodeString(text, quote + 1);
    // One string and nothing after it. `"a" "b"` is not a value this answers
    // for, and neither is a string the text ends inside.
    if (!decoded.closed || decoded.end != text.size()) return std::nullopt;
    return StringValue{std::move(decoded.text), std::move(decoded.problem)};
}

} // namespace didi::config_file
