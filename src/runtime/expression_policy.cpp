#include "didi/runtime/expression_policy.hpp"

#include <cstddef>
#include <limits>
#include <string>
#include <unordered_set>
#include <utility>
#include <vector>

namespace didi::runtime {
namespace {

constexpr size_t kMaxSourceBytes = 2048;

bool isContinuation(unsigned char value) {
    return (value & 0xC0u) == 0x80u;
}

bool isHexDigit(char value) {
    return (value >= '0' && value <= '9') ||
           (value >= 'a' && value <= 'f') ||
           (value >= 'A' && value <= 'F');
}

bool isIdentifierStart(unsigned char value) {
    return (value >= 'a' && value <= 'z') ||
           (value >= 'A' && value <= 'Z') || value == '_';
}

bool isIdentifierContinue(unsigned char value) {
    return isIdentifierStart(value) || (value >= '0' && value <= '9');
}

enum class TokenKind {
    Identifier,
    Number,
    String,
    Punctuation,
    Operator
};

struct Token {
    TokenKind kind;
    std::string text;
    size_t start;
    size_t end;
};

Result<std::vector<Token>> scanExpression(std::string_view source) {
    std::vector<Token> tokens;
    int brace_depth = 0;
    size_t index = 0;
    while (index < source.size()) {
        const auto byte = static_cast<unsigned char>(source[index]);
        if (byte == ' ' || byte == '\t') {
            ++index;
            continue;
        }
        if (byte == '\n' || byte == '\r' || byte == '\0' || byte < 0x20u || byte >= 0x80u) {
            return Error::invalidArgument("Expression contains a forbidden control or non-ASCII token outside a string");
        }
        if (isIdentifierStart(byte)) {
            const size_t start = index++;
            while (index < source.size() &&
                   isIdentifierContinue(static_cast<unsigned char>(source[index]))) ++index;
            tokens.push_back({TokenKind::Identifier, std::string(source.substr(start, index - start)),
                              start, index});
            continue;
        }
        if (byte >= '0' && byte <= '9') {
            const size_t start = index++;
            while (index < source.size()) {
                const auto current = static_cast<unsigned char>(source[index]);
                if ((current >= '0' && current <= '9') || current == '_' || current == '.') {
                    ++index;
                    continue;
                }
                if ((current == 'e' || current == 'E') && index + 1 < source.size()) {
                    ++index;
                    if (source[index] == '+' || source[index] == '-') ++index;
                    continue;
                }
                break;
            }
            tokens.push_back({TokenKind::Number, std::string(source.substr(start, index - start)),
                              start, index});
            continue;
        }
        if (byte == '\'' || byte == '"') {
            const char quote = static_cast<char>(byte);
            const size_t start = index++;
            bool terminated = false;
            while (index < source.size()) {
                const char current = source[index++];
                if (current == quote) {
                    terminated = true;
                    break;
                }
                if (current == '\n' || current == '\r' || current == '\0') {
                    return Error::invalidArgument("Expression string contains a forbidden control character");
                }
                if (current != '\\') continue;
                if (index >= source.size()) {
                    return Error::invalidArgument("Expression string ends with an incomplete escape");
                }
                const char escaped = source[index++];
                if (escaped == 'x' || escaped == 'u' || escaped == 'U') {
                    const size_t digits = escaped == 'x' ? 2u : (escaped == 'u' ? 4u : 8u);
                    if (index + digits > source.size()) {
                        return Error::invalidArgument("Expression string contains an incomplete hexadecimal escape");
                    }
                    for (size_t offset = 0; offset < digits; ++offset) {
                        if (!isHexDigit(source[index + offset])) {
                            return Error::invalidArgument("Expression string contains an invalid hexadecimal escape");
                        }
                    }
                    index += digits;
                    continue;
                }
                static const std::string allowed_escapes = "abefnrtv0\\\'\"";
                if (allowed_escapes.find(escaped) == std::string::npos) {
                    return Error::invalidArgument("Expression string contains an unsupported escape");
                }
            }
            if (!terminated) return Error::invalidArgument("Expression contains an unterminated string");
            tokens.push_back({TokenKind::String, std::string(source.substr(start, index - start)),
                              start, index});
            continue;
        }

        const char character = static_cast<char>(byte);
        if (character == '{') {
            ++brace_depth;
            tokens.push_back({TokenKind::Punctuation, "{", index, index + 1});
            ++index;
            continue;
        }
        if (character == '}') {
            if (brace_depth <= 0) return Error::invalidArgument("Expression contains an unmatched closing brace");
            --brace_depth;
            tokens.push_back({TokenKind::Punctuation, "}", index, index + 1});
            ++index;
            continue;
        }
        if (character == ':') {
            if (brace_depth <= 0) return Error::invalidArgument("Statement separators and type annotations are forbidden");
            tokens.push_back({TokenKind::Punctuation, ":", index, index + 1});
            ++index;
            continue;
        }
        if (character == '(' || character == ')' || character == '[' || character == ']' ||
            character == ',' || character == '.') {
            tokens.push_back({TokenKind::Punctuation, std::string(1, character), index, index + 1});
            ++index;
            continue;
        }

        if (character == '=') {
            if (index + 1 >= source.size() || source[index + 1] != '=') {
                return Error::invalidArgument("Assignments are forbidden in read-only expressions");
            }
            tokens.push_back({TokenKind::Operator, "==", index, index + 2});
            index += 2;
            continue;
        }
        if ((character == '!' || character == '<' || character == '>') &&
            index + 1 < source.size() && source[index + 1] == '=') {
            tokens.push_back({TokenKind::Operator, std::string(source.substr(index, 2)),
                              index, index + 2});
            index += 2;
            continue;
        }
        if ((character == '&' || character == '|' || character == '<' || character == '>' || character == '*') &&
            index + 1 < source.size() && source[index + 1] == character) {
            tokens.push_back({TokenKind::Operator, std::string(source.substr(index, 2)),
                              index, index + 2});
            index += 2;
            continue;
        }
        if (character == '+' || character == '-' || character == '*' || character == '/' ||
            character == '!' || character == '<' || character == '>' ||
            character == '&' || character == '|' || character == '^' || character == '~') {
            tokens.push_back({TokenKind::Operator, std::string(1, character), index, index + 1});
            ++index;
            continue;
        }
        return Error::invalidArgument("Expression contains forbidden statement or dispatch punctuation");
    }
    if (brace_depth != 0) return Error::invalidArgument("Expression contains an unmatched opening brace");
    if (tokens.empty()) return Error::invalidArgument("Expression must contain a value");
    return tokens;
}

const std::unordered_set<std::string>& allowedGlobalCallables() {
    static const std::unordered_set<std::string> values = {
        "min", "max", "abs", "clamp", "snapped", "Vector2", "Vector3", "Color"
    };
    return values;
}

const std::unordered_set<std::string>& forbiddenIdentifiers() {
    static const std::unordered_set<std::string> values = {
        "OS", "FileAccess", "DirAccess", "ResourceLoader", "ResourceSaver", "ProjectSettings",
        "Engine", "ClassDB", "JavaScriptBridge", "IP", "HTTPClient", "HTTPRequest", "TCPServer",
        "StreamPeerTCP", "PacketPeerUDP", "TLSOptions", "ZIPReader", "ZIPPacker", "WorkerThreadPool",
        "load", "preload", "execute", "open", "remove", "rename", "store", "str",
        "get_property_list", "set", "set_deferred",
        "set_indexed", "set_meta", "remove_meta", "call", "callv", "call_deferred", "rpc", "rpc_id",
        "emit_signal", "queue_free", "free", "quit", "change_scene", "change_scene_to_file",
        "change_scene_to_packed", "reload_current_scene", "add_child", "remove_child", "reparent",
        "set_owner", "set_script", "get_script", "get_source_code", "connect", "disconnect",
        "while", "for", "in", "match", "func", "class", "class_name", "extends", "signal", "var", "const",
        "enum", "return", "break", "continue", "pass", "await", "yield", "assert", "static", "lambda"
    };
    return values;
}

enum class ReceiverKind {
    None,
    Node,
    StringLiteral,
    ArrayLiteral,
    DictionaryLiteral,
    // The result of a Vector2, Vector3 or Color constructor written out in this
    // expression. There is no object to reach through: the value was built from
    // source-local numbers a few tokens earlier.
    MathLiteral
};

bool isLiteralIdentifier(const Token& token) {
    return token.kind == TokenKind::Identifier &&
           (token.text == "true" || token.text == "false" || token.text == "null");
}

bool isSourceLocalContainer(const std::vector<Token>& tokens, size_t receiver_end,
                            const std::string& opening, const std::string& closing) {
    int depth = 0;
    size_t opening_index = receiver_end;
    for (size_t index = receiver_end + 1; index-- > 0;) {
        if (tokens[index].text == closing) {
            ++depth;
        } else if (tokens[index].text == opening) {
            if (--depth == 0) {
                opening_index = index;
                break;
            }
        }
        if (index == 0) break;
    }
    if (opening_index == receiver_end || tokens[opening_index].text != opening) return false;
    for (size_t index = opening_index + 1; index < receiver_end; ++index) {
        const auto& token = tokens[index];
        if (token.kind == TokenKind::String || token.kind == TokenKind::Number ||
            isLiteralIdentifier(token) || token.text == "," || token.text == ":" ||
            token.text == "[" || token.text == "]" || token.text == "{" ||
            token.text == "}" || token.text == "+" || token.text == "-") {
            continue;
        }
        return false;
    }
    return true;
}

// Names the math constructor whose closing parenthesis sits at receiver_end, or
// "" when that token does not close one. The constructor's own arguments were
// already forced to be source-local numerics by the global-callable rule when
// the loop reached its identifier, so nothing dynamic can hide inside it.
std::string mathConstructorAt(const std::vector<Token>& tokens, size_t receiver_end) {
    static const std::unordered_set<std::string> constructors = {"Vector2", "Vector3", "Color"};
    if (receiver_end >= tokens.size() || tokens[receiver_end].text != ")") return {};
    int depth = 0;
    for (size_t index = receiver_end + 1; index-- > 0;) {
        if (tokens[index].text == ")") {
            ++depth;
        } else if (tokens[index].text == "(") {
            if (--depth == 0) {
                if (index == 0) return {};
                const auto& name = tokens[index - 1];
                if (name.kind == TokenKind::Identifier && constructors.count(name.text) != 0) {
                    return name.text;
                }
                return {};
            }
        }
        if (index == 0) break;
    }
    return {};
}

// Whether the token before `receiver_end` closes a direct node.get("name")
// property read.
//
// The read is replaced with a prebound input before the expression is parsed,
// so by execution time there is no object left to reach through: the value is
// already a Variant of one of the types isSafePreboundPropertyType allows, none
// of which is an Object, an Array or a Dictionary. Reading a component off it
// is the same act as reading one off Vector2(10, 20), which is already allowed.
//
// It exists because runtime_explore_scene had no way to sample a number.
// position.x is forbidden, node.get("position") is a Vector2 and a probe needs
// a scalar, so every probe in field trial 03 read nothing at all and the run
// still reported a clean twelve seconds.
bool isPreboundPropertyRead(const std::vector<Token>& tokens, size_t receiver_end) {
    if (receiver_end < 5 || tokens[receiver_end].text != ")") return false;
    return tokens[receiver_end - 1].kind == TokenKind::String &&
           tokens[receiver_end - 2].text == "(" &&
           tokens[receiver_end - 3].kind == TokenKind::Identifier &&
           tokens[receiver_end - 3].text == "get" && tokens[receiver_end - 4].text == "." &&
           tokens[receiver_end - 5].kind == TokenKind::Identifier &&
           tokens[receiver_end - 5].text == "node";
}

// The components a prebound property can carry. The allowed prebound types are
// Vector2, Vector3 and Color plus scalars, so this is their members and
// nothing else; anything else fails at execution rather than reading something
// unintended.
bool isPreboundComponent(const std::string& component) {
    return component == "x" || component == "y" || component == "z" || component == "r" ||
           component == "g" || component == "b" || component == "a";
}

bool isMathComponent(const std::string& constructor, const std::string& component) {
    if (constructor == "Vector2") return component == "x" || component == "y";
    if (constructor == "Vector3") {
        return component == "x" || component == "y" || component == "z";
    }
    if (constructor == "Color") {
        return component == "r" || component == "g" || component == "b" || component == "a";
    }
    return false;
}

ReceiverKind receiverKind(const std::vector<Token>& tokens, size_t call_index) {
    if (call_index < 2 || tokens[call_index - 1].text != ".") return ReceiverKind::None;
    const size_t receiver_end = call_index - 2;
    const auto& receiver = tokens[receiver_end];
    if (receiver.kind == TokenKind::Identifier && receiver.text == "node") {
        return ReceiverKind::Node;
    }
    if (receiver.kind == TokenKind::String) return ReceiverKind::StringLiteral;
    if (receiver.text == "]" &&
        isSourceLocalContainer(tokens, receiver_end, "[", "]")) {
        return ReceiverKind::ArrayLiteral;
    }
    if (receiver.text == "}" &&
        isSourceLocalContainer(tokens, receiver_end, "{", "}")) {
        return ReceiverKind::DictionaryLiteral;
    }
    if (!mathConstructorAt(tokens, receiver_end).empty()) {
        return ReceiverKind::MathLiteral;
    }
    return ReceiverKind::None;
}

bool hasNoArguments(const std::vector<Token>& tokens, size_t call_index) {
    return call_index + 2 < tokens.size() && tokens[call_index + 2].text == ")";
}

bool hasOneStringLiteralArgument(const std::vector<Token>& tokens, size_t call_index) {
    return call_index + 3 < tokens.size() &&
           tokens[call_index + 2].kind == TokenKind::String &&
           tokens[call_index + 3].text == ")";
}

bool hasOneScalarLiteralArgument(const std::vector<Token>& tokens, size_t call_index) {
    return call_index + 3 < tokens.size() &&
           (tokens[call_index + 2].kind == TokenKind::String ||
            tokens[call_index + 2].kind == TokenKind::Number ||
            isLiteralIdentifier(tokens[call_index + 2])) &&
           tokens[call_index + 3].text == ")";
}

// Exactly one argument, itself a math constructor written out here, as in
// Vector2(0, 0).distance_to(Vector2(3, 4)).
bool hasOneMathConstructorArgument(const std::vector<Token>& tokens, size_t call_index) {
    // call_index names the method, call_index + 1 is its opening parenthesis, so
    // the argument starts at call_index + 2 and must read Name ( ... ) ).
    const size_t argument_start = call_index + 2;
    if (argument_start + 1 >= tokens.size()) return false;
    if (tokens[argument_start].kind != TokenKind::Identifier) return false;
    if (tokens[argument_start + 1].text != "(") return false;

    int depth = 0;
    for (size_t index = argument_start + 1; index < tokens.size(); ++index) {
        if (tokens[index].text == "(") {
            ++depth;
        } else if (tokens[index].text == ")" && --depth == 0) {
            // The argument closed here. It has to be a constructor, and it has
            // to be the only argument.
            return !mathConstructorAt(tokens, index).empty() &&
                   index + 1 < tokens.size() && tokens[index + 1].text == ")";
        }
    }
    return false;
}

bool hasOnlySourceLocalNumericArguments(const std::vector<Token>& tokens,
                                        size_t call_index) {
    for (size_t index = call_index + 2; index < tokens.size(); ++index) {
        const auto& token = tokens[index];
        if (token.text == ")") return true;
        if (token.kind == TokenKind::Number || token.text == "," ||
            token.text == "+" || token.text == "-") {
            continue;
        }
        if (token.kind == TokenKind::Identifier &&
            (token.text == "INF" || token.text == "NAN" ||
             token.text == "PI" || token.text == "TAU")) {
            continue;
        }
        return false;
    }
    return false;
}

Result<void> validateTokens(const std::vector<Token>& tokens) {
    for (size_t index = 0; index < tokens.size(); ++index) {
        const auto& token = tokens[index];
        if (token.text == ".") {
            const bool method_call = index + 2 < tokens.size() &&
                                     tokens[index + 1].kind == TokenKind::Identifier &&
                                     tokens[index + 2].text == "(";
            // Vector2(10, 20).x reads a scalar off a value this expression just
            // built from source-local numbers. There is no object to reach
            // through, so the rule against object property reads does not apply.
            const bool component_read =
                !method_call && index > 0 && index + 1 < tokens.size() &&
                tokens[index + 1].kind == TokenKind::Identifier &&
                isMathComponent(mathConstructorAt(tokens, index - 1), tokens[index + 1].text);
            // node.get("position").x. The read in front of the dot is prebound
            // as a value before the expression is parsed, so this reads a
            // component off a value rather than through an object.
            const bool prebound_component_read =
                !method_call && index > 0 && index + 1 < tokens.size() &&
                tokens[index + 1].kind == TokenKind::Identifier &&
                isPreboundComponent(tokens[index + 1].text) &&
                isPreboundPropertyRead(tokens, index - 1);
            if (!method_call && !component_read && !prebound_component_read) {
                // The rule was stated and the way through was not, so the
                // natural next call after being refused was another refusal
                // (#488). A property read through an object can run a script
                // getter, which is project code; node.get(...) cannot, because
                // the value is prebound from the native getter before the
                // expression is parsed.
                return Error::invalidArgument(
                    "Object member/property reads are forbidden in read-only expressions, "
                    "because reading through an object can run a script getter. Read a "
                    "native property of the context node with node.get(\"position\"), or "
                    "use scene_get_property.");
            }
        }
        if (token.text == "[" && index > 0) {
            const auto& previous = tokens[index - 1];
            const bool starts_literal = previous.kind == TokenKind::Operator ||
                                        previous.text == "(" || previous.text == "[" ||
                                        previous.text == "{" || previous.text == "," ||
                                        previous.text == ":" || previous.text == "and" ||
                                        previous.text == "or" || previous.text == "not" ||
                                        previous.text == "in" || previous.text == "if" ||
                                        previous.text == "else";
            if (!starts_literal) {
                return Error::invalidArgument(
                    "Dynamic indexed reads are forbidden in read-only expressions");
            }
        }
        // `self` parsed and reached Godot, which answered "self can't be used
        // because instance is null". That reads like a fault in the caller's
        // expression rather than a fact about the sandbox, and it buried the
        // name that is bound (#488).
        if (token.kind == TokenKind::Identifier && token.text == "self") {
            return Error::invalidArgument(
                "self is not bound in a read-only expression: there is no script instance "
                "for it to be. The context node is bound as node, so read a native property "
                "with node.get(\"position\") or call one of the allowed node methods.");
        }
        if (token.kind == TokenKind::Identifier && forbiddenIdentifiers().count(token.text) != 0) {
            return Error::invalidArgument("Expression contains a forbidden identifier");
        }
        if (token.kind == TokenKind::Identifier && token.text.rfind("__didi_", 0) == 0) {
            return Error::invalidArgument("Expression contains a reserved sandbox identifier");
        }
        // #488 with a wider aperture. Every unbound name, not only `self`,
        // reached Godot and came back as "self can't be used because instance
        // is null (not passed)" -- a sentence about a word the expression did
        // not contain. The common case is not `self`: it is a typo in `node`,
        // or a singleton a caller reasonably expected to be there (#712).
        //
        // Only a bare name gets here, and every other position keeps the
        // sentence it already had. A name after a `.` is a member read and a
        // name before one is the receiver of that read, both of which the rule
        // at the top of this loop answers, and it answers them better: it names
        // node.get(...) as the way through for exactly that shape. A name
        // before a `(` is a call and the rules below answer it. A forbidden or
        // reserved name was answered two checks up.
        if (token.kind == TokenKind::Identifier &&
            !(index > 0 && tokens[index - 1].text == ".") &&
            !(index + 1 < tokens.size() &&
              (tokens[index + 1].text == "." || tokens[index + 1].text == "("))) {
            // Everything a bare word is allowed to be: the two names Expression
            // is given as inputs, the literals, the operators GDScript spells
            // with letters, and the numeric constants Expression resolves for
            // itself. `tree` is bound and is refused later, on the ground that
            // returning it is an unsupported non-Node Object, which is a
            // different and true sentence.
            static const std::unordered_set<std::string> bound_bare_identifiers = {
                "node", "tree",
                "true", "false", "null",
                "and", "or", "not", "in", "if", "else",
                "INF", "NAN", "PI", "TAU"
            };
            if (bound_bare_identifiers.count(token.text) == 0) {
                return Error::invalidArgument(
                    "'" + token.text + "' is not bound in a read-only expression. The context "
                    "node is bound as node, and that is the only name: read a native property "
                    "with node.get(\"position\"), call one of the allowed node methods, or use "
                    "scene_get_property. Engine singletons and project globals are not reachable "
                    "here.");
            }
        }
        if (token.kind == TokenKind::Identifier && index + 1 < tokens.size() &&
            tokens[index + 1].text == "(") {
            const auto receiver = receiverKind(tokens, index);
            if (receiver == ReceiverKind::None) {
                const bool has_method_receiver = index > 0 && tokens[index - 1].text == ".";
                if (has_method_receiver || allowedGlobalCallables().count(token.text) == 0 ||
                    !hasOnlySourceLocalNumericArguments(tokens, index)) {
                    return Error::invalidArgument(
                        "Global calls require source-local numeric arguments and no receiver");
                }
                continue;
            }
            if (receiver == ReceiverKind::Node) {
                if (token.text == "get") {
                    if (!hasOneStringLiteralArgument(tokens, index)) {
                        return Error::invalidArgument(
                            "get is restricted to direct node.get(<string literal>) property reads");
                    }
                    continue;
                }
                static const std::unordered_set<std::string> zero_argument_methods = {
                    "get_child_count", "get_path", "get_class"
                };
                static const std::unordered_set<std::string> string_argument_methods = {
                    "is_class", "is_in_group", "has_method", "has_meta"
                };
                if ((zero_argument_methods.count(token.text) != 0 &&
                     hasNoArguments(tokens, index)) ||
                    (string_argument_methods.count(token.text) != 0 &&
                     hasOneStringLiteralArgument(tokens, index))) {
                    continue;
                }
                return Error::invalidArgument(
                    "Only exact direct constant-space native scalar Node calls are permitted");
            }
            if (receiver == ReceiverKind::StringLiteral && token.text == "repeat") {
                const bool single_number_argument = index + 3 < tokens.size() &&
                                                    tokens[index + 2].kind == TokenKind::Number &&
                                                    tokens[index + 3].text == ")";
                if (!single_number_argument) {
                    return Error::invalidArgument(
                        "repeat is restricted to a bounded string literal and integer literal");
                }
            uint64_t repeat_count = 0;
            for (const auto character : tokens[index + 2].text) {
                if (character < '0' || character > '9' ||
                    repeat_count > (std::numeric_limits<uint64_t>::max() - 9) / 10) {
                    return Error::invalidArgument("repeat count must be a bounded integer literal");
                }
                repeat_count = repeat_count * 10 + static_cast<uint64_t>(character - '0');
            }
            const size_t literal_bytes = tokens[index - 2].text.size() - 2;
            if (repeat_count > 512 * 1024 ||
                (literal_bytes != 0 && repeat_count > (512 * 1024) / literal_bytes)) {
                return Error::invalidArgument("repeat output is limited to 512 KiB");
            }
                continue;
            }
            if (receiver == ReceiverKind::StringLiteral &&
                ((token.text == "size" || token.text == "is_empty") &&
                 hasNoArguments(tokens, index))) {
                continue;
            }
            if (receiver == ReceiverKind::StringLiteral &&
                (token.text == "find" || token.text == "count") &&
                hasOneStringLiteralArgument(tokens, index)) {
                continue;
            }
            if (receiver == ReceiverKind::ArrayLiteral &&
                ((token.text == "size" || token.text == "is_empty") &&
                 hasNoArguments(tokens, index))) {
                continue;
            }
            if (receiver == ReceiverKind::ArrayLiteral &&
                (token.text == "find" || token.text == "count" || token.text == "has") &&
                hasOneScalarLiteralArgument(tokens, index)) {
                continue;
            }
            if (receiver == ReceiverKind::DictionaryLiteral &&
                ((token.text == "size" || token.text == "is_empty") &&
                 hasNoArguments(tokens, index))) {
                continue;
            }
            if (receiver == ReceiverKind::DictionaryLiteral && token.text == "has" &&
                hasOneStringLiteralArgument(tokens, index)) {
                continue;
            }
            if (receiver == ReceiverKind::MathLiteral) {
                // Pure geometry on a value built from source-local numbers.
                // Every one of these returns a scalar or a vector of fixed size,
                // so the result stays in constant space, and none of them can
                // reach an object, the filesystem or the scene tree.
                static const std::unordered_set<std::string> zero_argument_methods = {
                    "length", "length_squared", "normalized", "abs", "sign",
                    "floor", "ceil", "round", "angle", "aspect", "is_normalized",
                    "is_finite", "min_axis_index", "max_axis_index"
                };
                static const std::unordered_set<std::string> vector_argument_methods = {
                    "dot", "cross", "distance_to", "distance_squared_to",
                    "angle_to", "angle_to_point"
                };
                if (zero_argument_methods.count(token.text) != 0 &&
                    hasNoArguments(tokens, index)) {
                    continue;
                }
                if (vector_argument_methods.count(token.text) != 0 &&
                    hasOneMathConstructorArgument(tokens, index)) {
                    continue;
                }
                return Error::invalidArgument(
                    "Only pure geometry calls on source-local Vector or Color literals are permitted");
            }
            return Error::invalidArgument(
                "Method calls require an exact direct Node receiver or a source-local literal receiver");
        }
        if (token.text == "(" && index > 0) {
            const auto& previous = tokens[index - 1];
            if (previous.kind != TokenKind::Identifier &&
                (previous.text == "]" || previous.text == ")" ||
                 previous.kind == TokenKind::String || previous.kind == TokenKind::Number)) {
                return Error::invalidArgument("Dynamic callable dispatch is forbidden");
            }
        }
    }
    return Result<void>::ok();
}

Result<std::vector<DirectPropertyRead>> readsInTokens(const std::vector<Token>& tokens) {
    std::vector<DirectPropertyRead> reads;
    for (size_t index = 0; index < tokens.size(); ++index) {
        if (tokens[index].kind != TokenKind::Identifier || tokens[index].text != "get" ||
            index + 1 >= tokens.size() || tokens[index + 1].text != "(") continue;
        const auto& literal = tokens[index + 2].text;
        if (literal.size() < 2 || literal.front() != literal.back()) {
            return Error::invalidArgument("node.get property name must be a quoted string literal");
        }
        std::string property = literal.substr(1, literal.size() - 2);
        if (property.empty()) {
            return Error::invalidArgument("node.get property name may not be empty");
        }
        for (const auto character : property) {
            const auto byte = static_cast<unsigned char>(character);
            if (!isIdentifierContinue(byte)) {
                return Error::invalidArgument(
                    "node.get property name must be an unescaped ASCII property identifier");
            }
        }
        reads.push_back({tokens[index - 2].start, tokens[index + 3].end,
                         std::move(property)});
    }
    return reads;
}

} // namespace

bool isValidUtf8(std::string_view text) {
    size_t index = 0;
    while (index < text.size()) {
        const auto first = static_cast<unsigned char>(text[index]);
        if (first <= 0x7Fu) {
            ++index;
            continue;
        }
        if (first >= 0xC2u && first <= 0xDFu) {
            if (index + 1 >= text.size() ||
                !isContinuation(static_cast<unsigned char>(text[index + 1]))) return false;
            index += 2;
            continue;
        }
        if (first >= 0xE0u && first <= 0xEFu) {
            if (index + 2 >= text.size()) return false;
            const auto second = static_cast<unsigned char>(text[index + 1]);
            const auto third = static_cast<unsigned char>(text[index + 2]);
            if (!isContinuation(second) || !isContinuation(third) ||
                (first == 0xE0u && second < 0xA0u) ||
                (first == 0xEDu && second >= 0xA0u)) return false;
            index += 3;
            continue;
        }
        if (first >= 0xF0u && first <= 0xF4u) {
            if (index + 3 >= text.size()) return false;
            const auto second = static_cast<unsigned char>(text[index + 1]);
            const auto third = static_cast<unsigned char>(text[index + 2]);
            const auto fourth = static_cast<unsigned char>(text[index + 3]);
            if (!isContinuation(second) || !isContinuation(third) || !isContinuation(fourth) ||
                (first == 0xF0u && second < 0x90u) ||
                (first == 0xF4u && second >= 0x90u)) return false;
            index += 4;
            continue;
        }
        return false;
    }
    return true;
}

Result<void> ExpressionPolicy::validate(std::string_view source) {
    if (source.empty() || source.size() > kMaxSourceBytes ||
        source.find('\0') != std::string_view::npos || !isValidUtf8(source)) {
        return Error::invalidArgument("expression must be 1..2048 bytes of valid UTF-8 without NUL");
    }
    auto scanned = scanExpression(source);
    if (scanned.isErr()) return scanned.error();
    auto valid = validateTokens(scanned.value());
    if (valid.isErr()) return valid.error();
    auto reads = readsInTokens(scanned.value());
    if (reads.isErr()) return reads.error();
    return Result<void>::ok();
}

Result<std::vector<DirectPropertyRead>> directPropertyReads(std::string_view source) {
    auto scanned = scanExpression(source);
    if (scanned.isErr()) return scanned.error();
    return readsInTokens(scanned.value());
}

} // namespace didi::runtime
