#include "didi/gdextension/expression_sandbox.hpp"

#include "didi/gdextension/gdextension_api.hpp"

#include <array>
#include <chrono>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <string>
#include <unordered_set>
#include <utility>
#include <vector>

namespace didi {
namespace godot {
namespace {

constexpr size_t kOpaqueBytes = 64;
constexpr size_t kMaxSourceBytes = 2048;
constexpr size_t kMaxContextPathBytes = 1024;
constexpr size_t kMaxResultBytes = 256 * 1024;
constexpr size_t kMaxContainerElements = 4096;
constexpr size_t kMaxClassNameBytes = 256;
constexpr size_t kResponseTimingReserve = 64;
constexpr int kMaxResultDepth = 16;
using Opaque = std::array<std::byte, kOpaqueBytes>;

json errorJson(int code, const std::string& message) {
    return {{"error", {{"code", code}, {"message", message}}}};
}

bool isContinuation(unsigned char value) {
    return (value & 0xC0u) == 0x80u;
}

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
    DictionaryLiteral
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
            if (!method_call) {
                return Error::invalidArgument(
                    "Object member/property reads are forbidden in read-only expressions");
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
        if (token.kind == TokenKind::Identifier && forbiddenIdentifiers().count(token.text) != 0) {
            return Error::invalidArgument("Expression contains a forbidden identifier");
        }
        if (token.kind == TokenKind::Identifier && token.text.rfind("__didi_", 0) == 0) {
            return Error::invalidArgument("Expression contains a reserved sandbox identifier");
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

struct DirectPropertyRead {
    size_t start;
    size_t end;
    std::string property;
};

Result<std::vector<DirectPropertyRead>> directPropertyReads(const std::vector<Token>& tokens) {
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

class NativeValue {
public:
    explicit NativeValue(GDExtensionVariantType type) : m_type(type) {}
    NativeValue(const NativeValue&) = delete;
    NativeValue& operator=(const NativeValue&) = delete;
    ~NativeValue() {
        if (m_initialized) {
            auto destructor = GodotApi::instance().variant_get_ptr_destructor(m_type);
            if (destructor) destructor(m_storage.data());
        }
    }
    void* ptr() { return m_storage.data(); }
    const void* ptr() const { return m_storage.data(); }
    void markInitialized() { m_initialized = true; }

private:
    alignas(16) Opaque m_storage{};
    GDExtensionVariantType m_type;
    bool m_initialized{false};
};

class VariantValue {
public:
    struct Uninitialized {};

    VariantValue() {
        if (GodotApi::instance().variant_new_nil) {
            GodotApi::instance().variant_new_nil(m_storage.data());
            m_initialized = true;
        }
    }
    explicit VariantValue(Uninitialized) {}
    VariantValue(const VariantValue&) = delete;
    VariantValue& operator=(const VariantValue&) = delete;
    VariantValue(VariantValue&& other) noexcept
        : m_storage(other.m_storage), m_initialized(other.m_initialized) {
        other.m_initialized = false;
    }
    VariantValue& operator=(VariantValue&& other) noexcept {
        if (this == &other) return *this;
        destroy();
        m_storage = other.m_storage;
        m_initialized = other.m_initialized;
        other.m_initialized = false;
        return *this;
    }
    ~VariantValue() { destroy(); }
    void* ptr() { return m_storage.data(); }
    const void* ptr() const { return m_storage.data(); }
    void markInitialized() { m_initialized = true; }

private:
    void destroy() {
        if (m_initialized && GodotApi::instance().variant_destroy) {
            GodotApi::instance().variant_destroy(m_storage.data());
        }
        m_initialized = false;
    }
    alignas(16) Opaque m_storage{};
    bool m_initialized{false};
};

class NativeName {
public:
    explicit NativeName(const std::string& value) {
        if (GodotApi::instance().string_name_new_with_utf8_chars) {
            GodotApi::instance().string_name_new_with_utf8_chars(m_storage.data(), value.c_str());
            m_initialized = true;
        }
    }
    NativeName(const NativeName&) = delete;
    NativeName& operator=(const NativeName&) = delete;
    ~NativeName() {
        if (m_initialized) {
            auto destructor = GodotApi::instance().variant_get_ptr_destructor(
                GDEXTENSION_VARIANT_TYPE_STRING_NAME);
            if (destructor) destructor(m_storage.data());
        }
    }
    const void* ptr() const { return m_storage.data(); }
    bool valid() const { return m_initialized; }

private:
    alignas(16) Opaque m_storage{};
    bool m_initialized{false};
};

class OwnedObject {
public:
    explicit OwnedObject(GDExtensionObjectPtr object) : m_object(object) {}
    OwnedObject(const OwnedObject&) = delete;
    OwnedObject& operator=(const OwnedObject&) = delete;
    ~OwnedObject() {
        if (m_object && GodotApi::instance().object_destroy) {
            GodotApi::instance().object_destroy(m_object);
        }
    }
    GDExtensionObjectPtr get() const { return m_object; }

private:
    GDExtensionObjectPtr m_object{nullptr};
};

Result<VariantValue> variantFromNative(GDExtensionVariantType type, void* native) {
    auto constructor = GodotApi::instance().get_variant_from_type_constructor(type);
    if (!constructor) return Error::internal("Missing Godot Variant constructor");
    VariantValue result(VariantValue::Uninitialized{});
    constructor(result.ptr(), native);
    result.markInitialized();
    return std::move(result);
}

template <typename T>
Result<VariantValue> makeScalar(GDExtensionVariantType type, T value) {
    return variantFromNative(type, &value);
}

Result<VariantValue> makeString(const std::string& text) {
    auto& api = GodotApi::instance();
    if (!api.string_new_with_utf8_chars) return Error::internal("Godot String constructor is unavailable");
    NativeValue native(GDEXTENSION_VARIANT_TYPE_STRING);
    api.string_new_with_utf8_chars(native.ptr(), text.c_str());
    native.markInitialized();
    return variantFromNative(GDEXTENSION_VARIANT_TYPE_STRING, native.ptr());
}

Result<VariantValue> makeStringName(const std::string& text) {
    NativeName native(text);
    if (!native.valid()) return Error::internal("Godot StringName constructor is unavailable");
    return variantFromNative(GDEXTENSION_VARIANT_TYPE_STRING_NAME,
                             const_cast<void*>(native.ptr()));
}

Result<VariantValue> makeObject(GDExtensionObjectPtr object) {
    return variantFromNative(GDEXTENSION_VARIANT_TYPE_OBJECT, &object);
}

Result<VariantValue> makeContainer(GDExtensionVariantType type) {
    auto constructor = GodotApi::instance().variant_get_ptr_constructor(type, 0);
    if (!constructor) return Error::internal("Godot container constructor is unavailable");
    NativeValue native(type);
    constructor(native.ptr(), nullptr);
    native.markInitialized();
    return variantFromNative(type, native.ptr());
}

Result<VariantValue> callVariant(VariantValue& target, const std::string& method_name,
                                 const std::vector<const VariantValue*>& arguments = {}) {
    auto& api = GodotApi::instance();
    NativeName method(method_name);
    if (!method.valid() || !api.variant_call) return Error::internal("Godot Variant call API is unavailable");
    std::vector<const void*> raw_arguments;
    raw_arguments.reserve(arguments.size());
    for (const auto* argument : arguments) raw_arguments.push_back(argument->ptr());
    VariantValue result(VariantValue::Uninitialized{});
    GDExtensionCallError call_error{};
    api.variant_call(target.ptr(), method.ptr(), raw_arguments.empty() ? nullptr : raw_arguments.data(),
                     static_cast<GDExtensionInt>(raw_arguments.size()), result.ptr(), &call_error);
    result.markInitialized();
    if (call_error.error != GDEXTENSION_CALL_OK) {
        return Error::internal("Godot Variant." + method_name + " failed");
    }
    return std::move(result);
}

Result<VariantValue> callObject(GDExtensionObjectPtr object, const char* class_name,
                                const char* method_name, int64_t hash,
                                const std::vector<const VariantValue*>& arguments = {}) {
    if (!object) return Error::notFound(std::string("Cannot call ") + method_name + " on a null Godot object");
    auto& api = GodotApi::instance();
    NativeName klass(class_name);
    NativeName method(method_name);
    if (!klass.valid() || !method.valid() || !api.classdb_get_method_bind || !api.object_method_bind_call) {
        return Error::internal("Godot method binding API is unavailable");
    }
    auto binding = api.classdb_get_method_bind(klass.ptr(), method.ptr(), hash);
    if (!binding) {
        return Error::internal(std::string("Godot method binding unavailable: ") +
                               class_name + "." + method_name);
    }
    std::vector<const void*> raw_arguments;
    raw_arguments.reserve(arguments.size());
    for (const auto* argument : arguments) raw_arguments.push_back(argument->ptr());
    VariantValue result(VariantValue::Uninitialized{});
    GDExtensionCallError call_error{};
    api.object_method_bind_call(binding, object,
                                raw_arguments.empty() ? nullptr : raw_arguments.data(),
                                static_cast<GDExtensionInt>(raw_arguments.size()),
                                result.ptr(), &call_error);
    result.markInitialized();
    if (call_error.error != GDEXTENSION_CALL_OK) {
        return Error::internal(std::string("Godot call failed: ") + class_name + "." +
                               method_name + " (call error " +
                               std::to_string(call_error.error) + ")");
    }
    return std::move(result);
}

template <typename T>
Result<T> scalarFromVariant(VariantValue& value, GDExtensionVariantType type) {
    auto constructor = GodotApi::instance().get_variant_to_type_constructor(type);
    if (!constructor) return Error::internal("Missing Godot Variant-to-native constructor");
    T result{};
    constructor(&result, value.ptr());
    return result;
}

Result<GDExtensionObjectPtr> objectFromVariant(VariantValue& value) {
    return scalarFromVariant<GDExtensionObjectPtr>(value, GDEXTENSION_VARIANT_TYPE_OBJECT);
}

Result<std::string> nativeStringToUtf8(
    const void* native_string,
    size_t max_bytes = std::numeric_limits<size_t>::max()) {
    auto& api = GodotApi::instance();
    const auto length = api.string_to_utf8_chars(native_string, nullptr, 0);
    if (length < 0) return Error::internal("Godot String UTF-8 conversion failed");
    if (static_cast<uint64_t>(length) > max_bytes) {
        return Error(413, "Expression string exceeds the 256 KiB serialized limit");
    }
    std::string result(static_cast<size_t>(length), '\0');
    if (length > 0) api.string_to_utf8_chars(native_string, result.data(), length);
    return result;
}

Result<std::string> stringFromVariant(
    VariantValue& value,
    GDExtensionVariantType type,
    size_t max_bytes = std::numeric_limits<size_t>::max()) {
    auto& api = GodotApi::instance();
    auto to_native = api.get_variant_to_type_constructor(type);
    if (!to_native) return Error::internal("Missing Godot string conversion");
    NativeValue native(type);
    to_native(native.ptr(), value.ptr());
    native.markInitialized();
    if (type == GDEXTENSION_VARIANT_TYPE_STRING) {
        return nativeStringToUtf8(native.ptr(), max_bytes);
    }
    const int constructor_index = type == GDEXTENSION_VARIANT_TYPE_STRING_NAME ? 2 : 3;
    auto string_constructor = api.variant_get_ptr_constructor(GDEXTENSION_VARIANT_TYPE_STRING,
                                                               constructor_index);
    if (!string_constructor) return Error::internal("Missing Godot string-like conversion");
    NativeValue text(GDEXTENSION_VARIANT_TYPE_STRING);
    const void* arguments[] = {native.ptr()};
    string_constructor(text.ptr(), arguments);
    text.markInitialized();
    return nativeStringToUtf8(text.ptr(), max_bytes);
}

Result<size_t> stringVariantUtf8Length(VariantValue& value,
                                       GDExtensionVariantType type) {
    auto& api = GodotApi::instance();
    auto to_native = api.get_variant_to_type_constructor(type);
    if (!to_native) return Error::internal("Missing Godot string conversion");
    NativeValue native(type);
    to_native(native.ptr(), value.ptr());
    native.markInitialized();
    if (type == GDEXTENSION_VARIANT_TYPE_STRING) {
        const auto length = api.string_to_utf8_chars(native.ptr(), nullptr, 0);
        if (length < 0) return Error::internal("Godot String UTF-8 length query failed");
        return static_cast<size_t>(length);
    }
    const int constructor_index = type == GDEXTENSION_VARIANT_TYPE_STRING_NAME ? 2 : 3;
    auto string_constructor = api.variant_get_ptr_constructor(GDEXTENSION_VARIANT_TYPE_STRING,
                                                               constructor_index);
    if (!string_constructor) return Error::internal("Missing Godot string-like conversion");
    NativeValue text(GDEXTENSION_VARIANT_TYPE_STRING);
    const void* arguments[] = {native.ptr()};
    string_constructor(text.ptr(), arguments);
    text.markInitialized();
    const auto length = api.string_to_utf8_chars(text.ptr(), nullptr, 0);
    if (length < 0) return Error::internal("Godot String UTF-8 length query failed");
    return static_cast<size_t>(length);
}

Result<GDExtensionObjectPtr> singleton(const std::string& name) {
    NativeName native_name(name);
    if (!native_name.valid() || !GodotApi::instance().global_get_singleton) {
        return Error::internal("Godot singleton API is unavailable");
    }
    auto result = GodotApi::instance().global_get_singleton(native_name.ptr());
    if (!result) return Error::notConnected("Godot singleton is unavailable: " + name);
    return result;
}

Result<GDExtensionObjectPtr> activeSceneTree() {
    auto engine = singleton("Engine");
    if (engine.isErr()) return engine.error();
    auto main_loop = callObject(engine.value(), "Engine", "get_main_loop", 1016888095LL);
    if (main_loop.isErr()) return main_loop.error();
    auto object = objectFromVariant(main_loop.value());
    if (object.isErr() || !object.value()) return Error::notConnected("Godot Engine has no active main loop");
    auto scene_tree_name = makeString("SceneTree");
    if (scene_tree_name.isErr()) return scene_tree_name.error();
    auto is_scene_tree = callObject(object.value(), "Object", "is_class", 3927539163LL,
                                    {&scene_tree_name.value()});
    if (is_scene_tree.isErr()) return is_scene_tree.error();
    auto matches = scalarFromVariant<GDExtensionBool>(is_scene_tree.value(), GDEXTENSION_VARIANT_TYPE_BOOL);
    if (matches.isErr() || matches.value() == 0) {
        return Error::notConnected("Godot active main loop is not a SceneTree");
    }
    return object.value();
}

Result<std::string> nodeString(GDExtensionObjectPtr node, const char* owner,
                               const char* method, int64_t hash,
                               GDExtensionVariantType expected_type,
                               size_t max_bytes = std::numeric_limits<size_t>::max()) {
    auto result = callObject(node, owner, method, hash);
    if (result.isErr()) return result.error();
    return stringFromVariant(result.value(), expected_type, max_bytes);
}

bool isSafePreboundPropertyType(GDExtensionVariantType type) {
    switch (type) {
        case GDEXTENSION_VARIANT_TYPE_NIL:
        case GDEXTENSION_VARIANT_TYPE_BOOL:
        case GDEXTENSION_VARIANT_TYPE_INT:
        case GDEXTENSION_VARIANT_TYPE_FLOAT:
        case GDEXTENSION_VARIANT_TYPE_STRING:
        case GDEXTENSION_VARIANT_TYPE_STRING_NAME:
        case GDEXTENSION_VARIANT_TYPE_NODE_PATH:
        case GDEXTENSION_VARIANT_TYPE_VECTOR2:
        case GDEXTENSION_VARIANT_TYPE_VECTOR3:
        case GDEXTENSION_VARIANT_TYPE_COLOR:
            return true;
        default:
            return false;
    }
}

Result<void> prepareNativePropertyReads(const std::string& source,
                                        GDExtensionObjectPtr context_node,
                                        std::string& executable_source,
                                        std::vector<std::string>& input_names,
                                        std::vector<VariantValue>& input_values) {
    auto scanned = scanExpression(source);
    if (scanned.isErr()) return scanned.error();
    auto reads = directPropertyReads(scanned.value());
    if (reads.isErr()) return reads.error();
    executable_source = source;
    if (reads.value().empty()) return Result<void>::ok();

    auto class_db = singleton("ClassDB");
    if (class_db.isErr()) return class_db.error();
    auto class_text = nodeString(context_node, "Object", "get_class", 201670096LL,
                                 GDEXTENSION_VARIANT_TYPE_STRING,
                                 kMaxClassNameBytes);
    if (class_text.isErr()) return class_text.error();
    auto class_name = makeStringName(class_text.value());
    auto object_value = makeObject(context_node);
    if (class_name.isErr()) return class_name.error();
    if (object_value.isErr()) return object_value.error();

    input_names.reserve(reads.value().size());
    input_values.reserve(reads.value().size());
    for (size_t index = 0; index < reads.value().size(); ++index) {
        const auto& read = reads.value()[index];
        auto property_name = makeStringName(read.property);
        if (property_name.isErr()) return property_name.error();
        auto getter_value = callObject(class_db.value(), "ClassDB",
                                       "class_get_property_getter", 3770832642LL,
                                       {&class_name.value(), &property_name.value()});
        if (getter_value.isErr()) return getter_value.error();
        auto getter = stringFromVariant(getter_value.value(),
                                        GDEXTENSION_VARIANT_TYPE_STRING_NAME,
                                        kMaxClassNameBytes);
        if (getter.isErr()) return getter.error();
        if (getter.value().empty()) {
            return Error(403,
                         "node.get is limited to native ClassDB-defined properties");
        }
        auto property_value = callObject(class_db.value(), "ClassDB",
                                         "class_get_property", 2498641674LL,
                                         {&object_value.value(), &property_name.value()});
        if (property_value.isErr()) return property_value.error();
        const auto type = GodotApi::instance().variant_get_type(property_value.value().ptr());
        if (!isSafePreboundPropertyType(type)) {
            return Error(415,
                         "node.get native property has an unsupported prebound type");
        }
        if (type == GDEXTENSION_VARIANT_TYPE_STRING ||
            type == GDEXTENSION_VARIANT_TYPE_STRING_NAME ||
            type == GDEXTENSION_VARIANT_TYPE_NODE_PATH) {
            auto length = stringVariantUtf8Length(property_value.value(), type);
            if (length.isErr()) return length.error();
            if (length.value() > (kMaxResultBytes - 2) / 6) {
                return Error(413,
                             "node.get native string property exceeds the bounded result budget");
            }
        }
        input_names.push_back("__didi_property_" + std::to_string(index));
        input_values.push_back(std::move(property_value.value()));
    }

    for (size_t index = reads.value().size(); index-- > 0;) {
        const auto& read = reads.value()[index];
        executable_source.replace(read.start, read.end - read.start,
                                  input_names[index]);
    }
    return Result<void>::ok();
}

Result<void> validateContextPath(std::string_view path) {
    if (path.empty() || path.size() > kMaxContextPathBytes || !isValidUtf8(path) ||
        path.find('\0') != std::string_view::npos) {
        return Error::invalidArgument("context_node must be a non-empty UTF-8 path of at most 1024 bytes");
    }
    if (path != "/root" && path.rfind("/root/", 0) != 0) {
        return Error::invalidArgument("context_node must be a canonical absolute path beneath /root");
    }
    if (path.back() == '/' || path.find("//") != std::string_view::npos ||
        path.find('\\') != std::string_view::npos || path.find(':') != std::string_view::npos) {
        return Error::invalidArgument("context_node must be a canonical absolute NodePath");
    }
    size_t start = 1;
    while (start <= path.size()) {
        const auto end = path.find('/', start);
        const auto segment = path.substr(start, end == std::string_view::npos
                                                   ? std::string_view::npos
                                                   : end - start);
        if (segment.empty() || segment == "." || segment == ".." || segment.front() == '%') {
            return Error::invalidArgument("context_node may not contain aliases or relative segments");
        }
        if (end == std::string_view::npos) break;
        start = end + 1;
    }
    return Result<void>::ok();
}

Result<VariantValue> makeNodePath(const std::string& path) {
    NativeValue native_string(GDEXTENSION_VARIANT_TYPE_STRING);
    GodotApi::instance().string_new_with_utf8_chars(native_string.ptr(), path.c_str());
    native_string.markInitialized();
    auto constructor = GodotApi::instance().variant_get_ptr_constructor(
        GDEXTENSION_VARIANT_TYPE_NODE_PATH, 2);
    if (!constructor) return Error::internal("Godot NodePath constructor is unavailable");
    NativeValue native_path(GDEXTENSION_VARIANT_TYPE_NODE_PATH);
    const void* arguments[] = {native_string.ptr()};
    constructor(native_path.ptr(), arguments);
    native_path.markInitialized();
    return variantFromNative(GDEXTENSION_VARIANT_TYPE_NODE_PATH, native_path.ptr());
}

Result<GDExtensionObjectPtr> nodeFromVariant(VariantValue& value,
                                             const std::string& missing_message) {
    auto node = objectFromVariant(value);
    if (node.isErr()) return node.error();
    if (!node.value()) return Error::notFound(missing_message);
    return node.value();
}

Result<GDExtensionObjectPtr> resolveBelowRoot(GDExtensionObjectPtr root,
                                              const std::string& root_path,
                                              const std::string& requested_path) {
    if (requested_path == root_path) return root;
    if (requested_path.rfind(root_path + "/", 0) != 0) {
        return Error::invalidArgument("context_node escapes the active scene subtree");
    }
    const auto relative = requested_path.substr(root_path.size() + 1);
    auto node_path = makeNodePath(relative);
    if (node_path.isErr()) return node_path.error();
    auto result = callObject(root, "Node", "get_node_or_null", 2734337346LL,
                             {&node_path.value()});
    if (result.isErr()) return result.error();
    return nodeFromVariant(result.value(), "Expression context node was not found: " + requested_path);
}

struct ExpressionContext {
    GDExtensionObjectPtr tree{nullptr};
    GDExtensionObjectPtr node{nullptr};
    std::string canonical_path;
    GDExtensionObjectPtr scope_root{nullptr};
    std::string scope_root_path;
};

Result<ExpressionContext> resolveContext(const json& params, const std::string& session_kind) {
    auto tree = activeSceneTree();
    if (tree.isErr()) return tree.error();

    GDExtensionObjectPtr context_root = nullptr;
    GDExtensionObjectPtr scope_root = nullptr;
    std::string scope_root_path;
    if (session_kind == "editor") {
        auto editor = singleton("EditorInterface");
        if (editor.isErr()) return editor.error();
        auto root_value = callObject(editor.value(), "EditorInterface", "get_edited_scene_root",
                                     3160264692LL);
        if (root_value.isErr()) return root_value.error();
        auto root = nodeFromVariant(root_value.value(), "No edited scene is open in Godot");
        if (root.isErr()) return root.error();
        context_root = root.value();
        scope_root = root.value();
        auto root_name = nodeString(root.value(), "Node", "get_name", 2002593661LL,
                                    GDEXTENSION_VARIANT_TYPE_STRING_NAME,
                                    kMaxContextPathBytes - 6);
        if (root_name.isErr()) return root_name.error();
        scope_root_path = "/root/" + root_name.value();
        auto valid_scope = validateContextPath(scope_root_path);
        if (valid_scope.isErr()) return valid_scope.error();
    } else {
        auto current_value = callObject(tree.value(), "SceneTree", "get_current_scene", 3160264692LL);
        if (current_value.isErr()) return current_value.error();
        auto current = nodeFromVariant(current_value.value(), "Godot SceneTree has no current scene");
        if (current.isErr()) return current.error();
        context_root = current.value();
        auto tree_root_value = callObject(tree.value(), "SceneTree", "get_root", 1757182445LL);
        if (tree_root_value.isErr()) return tree_root_value.error();
        auto tree_root = nodeFromVariant(tree_root_value.value(), "Godot SceneTree has no root Window");
        if (tree_root.isErr()) return tree_root.error();
        scope_root = tree_root.value();
        scope_root_path = "/root";
    }

    GDExtensionObjectPtr context_node = context_root;
    std::string canonical_path;
    if (params.contains("context_node")) {
        const auto requested_path = params["context_node"].get<std::string>();
        auto valid = validateContextPath(requested_path);
        if (valid.isErr()) return valid.error();
        if (session_kind == "editor") {
            auto resolved = resolveBelowRoot(context_root, scope_root_path, requested_path);
            if (resolved.isErr()) return resolved.error();
            context_node = resolved.value();
        } else {
            auto resolved = resolveBelowRoot(scope_root, scope_root_path, requested_path);
            if (resolved.isErr()) return resolved.error();
            context_node = resolved.value();
        }
        canonical_path = requested_path;
    } else if (session_kind == "editor") {
        canonical_path = scope_root_path;
    } else {
        auto native_path = nodeString(context_node, "Node", "get_path", 4075236667LL,
                                      GDEXTENSION_VARIANT_TYPE_NODE_PATH,
                                      kMaxContextPathBytes);
        if (native_path.isErr()) return native_path.error();
        auto valid_path = validateContextPath(native_path.value());
        if (valid_path.isErr()) return valid_path.error();
        canonical_path = native_path.value();
    }
    return ExpressionContext{tree.value(), context_node, canonical_path,
                             scope_root, scope_root_path};
}

Result<std::string> resultNodePath(GDExtensionObjectPtr node,
                                   const ExpressionContext& context) {
    if (node == context.scope_root) return context.scope_root_path;
    auto target_value = makeObject(node);
    auto use_unique_path = makeScalar(GDEXTENSION_VARIANT_TYPE_BOOL,
                                      static_cast<GDExtensionBool>(0));
    if (target_value.isErr()) return target_value.error();
    if (use_unique_path.isErr()) return use_unique_path.error();
    auto relative_value = callObject(context.scope_root, "Node", "get_path_to", 498846349LL,
                                     {&target_value.value(), &use_unique_path.value()});
    if (relative_value.isErr()) return relative_value.error();
    auto relative = stringFromVariant(relative_value.value(),
                                      GDEXTENSION_VARIANT_TYPE_NODE_PATH,
                                      kMaxContextPathBytes);
    if (relative.isErr()) return relative.error();
    if (relative.value() == ".." || relative.value().rfind("../", 0) == 0 ||
        relative.value().empty() || relative.value().front() == '/') {
        return Error(415, "Expression returned a Node outside the active session subtree");
    }
    if (relative.value().size() > kMaxContextPathBytes -
                                    std::min(context.scope_root_path.size() + 1,
                                             kMaxContextPathBytes)) {
        return Error(413, "Expression Node path exceeds the 1024-byte context limit");
    }
    const auto path = context.scope_root_path + "/" + relative.value();
    auto valid_path = validateContextPath(path);
    if (valid_path.isErr()) return Error(415, "Expression returned a non-canonical Node path");
    return path;
}

struct ConversionBudget {
    size_t bytes{0};

    size_t remaining() const {
        return kMaxResultBytes - std::min(bytes, kMaxResultBytes);
    }

    Result<void> add(size_t amount) {
        if (amount > kMaxResultBytes - std::min(bytes, kMaxResultBytes)) {
            return Error(413, "Expression result exceeds the 256 KiB serialized limit");
        }
        bytes += amount;
        return Result<void>::ok();
    }
};

struct ConvertedValue {
    json value;
    std::string type;
};

const char* variantTypeName(GDExtensionVariantType type) {
    switch (type) {
        case GDEXTENSION_VARIANT_TYPE_NIL: return "null";
        case GDEXTENSION_VARIANT_TYPE_BOOL: return "bool";
        case GDEXTENSION_VARIANT_TYPE_INT: return "int";
        case GDEXTENSION_VARIANT_TYPE_FLOAT: return "float";
        case GDEXTENSION_VARIANT_TYPE_STRING: return "String";
        case GDEXTENSION_VARIANT_TYPE_STRING_NAME: return "StringName";
        case GDEXTENSION_VARIANT_TYPE_NODE_PATH: return "NodePath";
        case GDEXTENSION_VARIANT_TYPE_ARRAY: return "Array";
        case GDEXTENSION_VARIANT_TYPE_DICTIONARY: return "Dictionary";
        case GDEXTENSION_VARIANT_TYPE_VECTOR2: return "Vector2";
        case GDEXTENSION_VARIANT_TYPE_VECTOR3: return "Vector3";
        case GDEXTENSION_VARIANT_TYPE_COLOR: return "Color";
        case GDEXTENSION_VARIANT_TYPE_OBJECT: return "Object";
        default: return "unsupported";
    }
}

double elapsedMilliseconds(std::chrono::steady_clock::time_point started);
Result<void> requireWithinTimeout(std::chrono::steady_clock::time_point started,
                                  int64_t timeout_ms);

Result<ConvertedValue> convertVariant(VariantValue& value, ConversionBudget& budget,
                                      const ExpressionContext& context,
                                      std::chrono::steady_clock::time_point started,
                                      int64_t timeout_ms,
                                      int depth = 0) {
    auto within_timeout = requireWithinTimeout(started, timeout_ms);
    if (within_timeout.isErr()) return within_timeout.error();
    if (depth > kMaxResultDepth) {
        return Error(413, "Expression result exceeds the maximum nesting depth of 16");
    }
    const auto type = GodotApi::instance().variant_get_type(value.ptr());
    if (type == GDEXTENSION_VARIANT_TYPE_NIL) {
        auto bounded = budget.add(4);
        if (bounded.isErr()) return bounded.error();
        return ConvertedValue{nullptr, variantTypeName(type)};
    }
    if (type == GDEXTENSION_VARIANT_TYPE_BOOL) {
        auto result = scalarFromVariant<GDExtensionBool>(value, type);
        if (result.isErr()) return result.error();
        auto bounded = budget.add(result.value() ? 4 : 5);
        if (bounded.isErr()) return bounded.error();
        return ConvertedValue{result.value() != 0, variantTypeName(type)};
    }
    if (type == GDEXTENSION_VARIANT_TYPE_INT) {
        auto result = scalarFromVariant<int64_t>(value, type);
        if (result.isErr()) return result.error();
        auto bounded = budget.add(24);
        if (bounded.isErr()) return bounded.error();
        return ConvertedValue{result.value(), variantTypeName(type)};
    }
    if (type == GDEXTENSION_VARIANT_TYPE_FLOAT) {
        auto result = scalarFromVariant<double>(value, type);
        if (result.isErr()) return result.error();
        if (!std::isfinite(result.value())) {
            return Error(415, "Expression returned a non-finite float");
        }
        auto bounded = budget.add(32);
        if (bounded.isErr()) return bounded.error();
        return ConvertedValue{result.value(), variantTypeName(type)};
    }
    if (type == GDEXTENSION_VARIANT_TYPE_STRING ||
        type == GDEXTENSION_VARIANT_TYPE_STRING_NAME ||
        type == GDEXTENSION_VARIANT_TYPE_NODE_PATH) {
        const size_t remaining = budget.remaining();
        const size_t max_string_bytes = remaining > 2 ? (remaining - 2) / 6 : 0;
        auto result = stringFromVariant(value, type, max_string_bytes);
        if (result.isErr()) return result.error();
        within_timeout = requireWithinTimeout(started, timeout_ms);
        if (within_timeout.isErr()) return within_timeout.error();
        const size_t escaped_upper_bound = result.value().size() > (kMaxResultBytes - 2) / 6
            ? kMaxResultBytes + 1
            : result.value().size() * 6 + 2;
        auto bounded = budget.add(escaped_upper_bound);
        if (bounded.isErr()) return bounded.error();
        return ConvertedValue{result.value(), variantTypeName(type)};
    }
    if (type == GDEXTENSION_VARIANT_TYPE_VECTOR2 ||
        type == GDEXTENSION_VARIANT_TYPE_VECTOR3 ||
        type == GDEXTENSION_VARIANT_TYPE_COLOR) {
        struct Vector2Native { float x; float y; };
        struct Vector3Native { float x; float y; float z; };
        struct ColorNative { float r; float g; float b; float a; };
        json output;
        if (type == GDEXTENSION_VARIANT_TYPE_VECTOR2) {
            auto native = scalarFromVariant<Vector2Native>(value, type);
            if (native.isErr()) return native.error();
            if (!std::isfinite(native.value().x) || !std::isfinite(native.value().y)) {
                return Error(415, "Expression returned a Vector2 with non-finite components");
            }
            output = {{"type", "Vector2"}, {"x", native.value().x}, {"y", native.value().y}};
        } else if (type == GDEXTENSION_VARIANT_TYPE_VECTOR3) {
            auto native = scalarFromVariant<Vector3Native>(value, type);
            if (native.isErr()) return native.error();
            if (!std::isfinite(native.value().x) || !std::isfinite(native.value().y) ||
                !std::isfinite(native.value().z)) {
                return Error(415, "Expression returned a Vector3 with non-finite components");
            }
            output = {{"type", "Vector3"}, {"x", native.value().x}, {"y", native.value().y},
                      {"z", native.value().z}};
        } else {
            auto native = scalarFromVariant<ColorNative>(value, type);
            if (native.isErr()) return native.error();
            if (!std::isfinite(native.value().r) || !std::isfinite(native.value().g) ||
                !std::isfinite(native.value().b) || !std::isfinite(native.value().a)) {
                return Error(415, "Expression returned a Color with non-finite components");
            }
            output = {{"type", "Color"}, {"r", native.value().r}, {"g", native.value().g},
                      {"b", native.value().b}, {"a", native.value().a}};
        }
        auto bounded = budget.add(output.dump().size());
        if (bounded.isErr()) return bounded.error();
        within_timeout = requireWithinTimeout(started, timeout_ms);
        if (within_timeout.isErr()) return within_timeout.error();
        return ConvertedValue{std::move(output), variantTypeName(type)};
    }
    if (type == GDEXTENSION_VARIANT_TYPE_ARRAY) {
        auto size_value = callVariant(value, "size");
        if (size_value.isErr()) return size_value.error();
        auto size = scalarFromVariant<int64_t>(size_value.value(), GDEXTENSION_VARIANT_TYPE_INT);
        if (size.isErr()) return size.error();
        if (size.value() < 0 ||
            static_cast<uint64_t>(size.value()) > kMaxContainerElements) {
            return Error(413, "Expression array exceeds the serialized result limit");
        }
        auto bounded = budget.add(2 + static_cast<size_t>(size.value()));
        if (bounded.isErr()) return bounded.error();
        json output = json::array();
        for (int64_t index = 0; index < size.value(); ++index) {
            auto index_value = makeScalar(GDEXTENSION_VARIANT_TYPE_INT, index);
            if (index_value.isErr()) return index_value.error();
            auto item = callVariant(value, "get", {&index_value.value()});
            if (item.isErr()) return item.error();
            auto converted = convertVariant(item.value(), budget, context,
                                            started, timeout_ms, depth + 1);
            if (converted.isErr()) return converted.error();
            output.push_back(std::move(converted.value().value));
        }
        return ConvertedValue{std::move(output), variantTypeName(type)};
    }
    if (type == GDEXTENSION_VARIANT_TYPE_DICTIONARY) {
        auto size_value = callVariant(value, "size");
        if (size_value.isErr()) return size_value.error();
        auto size = scalarFromVariant<int64_t>(size_value.value(), GDEXTENSION_VARIANT_TYPE_INT);
        if (size.isErr()) return size.error();
        if (size.value() < 0 ||
            static_cast<uint64_t>(size.value()) > kMaxContainerElements) {
            return Error(413, "Expression dictionary exceeds the serialized result limit");
        }
        auto bounded = budget.add(2 + static_cast<size_t>(size.value()));
        if (bounded.isErr()) return bounded.error();
        within_timeout = requireWithinTimeout(started, timeout_ms);
        if (within_timeout.isErr()) return within_timeout.error();
        auto keys = callVariant(value, "keys");
        if (keys.isErr()) return keys.error();
        within_timeout = requireWithinTimeout(started, timeout_ms);
        if (within_timeout.isErr()) return within_timeout.error();
        json output = json::object();
        for (int64_t index = 0; index < size.value(); ++index) {
            auto index_value = makeScalar(GDEXTENSION_VARIANT_TYPE_INT, index);
            if (index_value.isErr()) return index_value.error();
            auto key = callVariant(keys.value(), "get", {&index_value.value()});
            if (key.isErr()) return key.error();
            const auto key_type = GodotApi::instance().variant_get_type(key.value().ptr());
            if (key_type != GDEXTENSION_VARIANT_TYPE_STRING &&
                key_type != GDEXTENSION_VARIANT_TYPE_STRING_NAME) {
                return Error(415, "Expression dictionary contains a non-string key");
            }
            const size_t remaining = budget.remaining();
            const size_t max_key_bytes = remaining > 3 ? (remaining - 3) / 6 : 0;
            auto key_text = stringFromVariant(key.value(), key_type, max_key_bytes);
            if (key_text.isErr()) return key_text.error();
            within_timeout = requireWithinTimeout(started, timeout_ms);
            if (within_timeout.isErr()) return within_timeout.error();
            auto key_bounded = budget.add(key_text.value().size() * 6 + 3);
            if (key_bounded.isErr()) return key_bounded.error();
            auto item = callVariant(value, "get", {&key.value()});
            if (item.isErr()) return item.error();
            auto converted = convertVariant(item.value(), budget, context,
                                            started, timeout_ms, depth + 1);
            if (converted.isErr()) return converted.error();
            output[key_text.value()] = std::move(converted.value().value);
        }
        return ConvertedValue{std::move(output), variantTypeName(type)};
    }
    if (type == GDEXTENSION_VARIANT_TYPE_OBJECT) {
        auto object = objectFromVariant(value);
        if (object.isErr()) return object.error();
        if (!object.value()) {
            auto bounded = budget.add(4);
            if (bounded.isErr()) return bounded.error();
            return ConvertedValue{nullptr, "null"};
        }
        auto node_name = makeString("Node");
        if (node_name.isErr()) return node_name.error();
        auto is_node_value = callObject(object.value(), "Object", "is_class", 3927539163LL,
                                        {&node_name.value()});
        if (is_node_value.isErr()) return is_node_value.error();
        auto is_node = scalarFromVariant<GDExtensionBool>(is_node_value.value(),
                                                          GDEXTENSION_VARIANT_TYPE_BOOL);
        if (is_node.isErr()) return is_node.error();
        if (is_node.value() == 0) {
            return Error(415, "Expression returned an unsupported non-Node Object");
        }
        auto path = resultNodePath(object.value(), context);
        auto class_name = nodeString(object.value(), "Object", "get_class", 201670096LL,
                                     GDEXTENSION_VARIANT_TYPE_STRING,
                                     kMaxClassNameBytes);
        auto instance_value = callObject(object.value(), "Object", "get_instance_id", 3905245786LL);
        if (class_name.isErr()) return class_name.error();
        if (path.isErr()) return path.error();
        if (instance_value.isErr()) return instance_value.error();
        auto instance_id = scalarFromVariant<int64_t>(instance_value.value(), GDEXTENSION_VARIANT_TYPE_INT);
        if (instance_id.isErr()) return instance_id.error();
        json output = {{"type", "Node"}, {"class", class_name.value()},
                       {"path", path.value()}, {"instance_id", instance_id.value()}};
        auto bounded = budget.add(output.dump().size());
        if (bounded.isErr()) return bounded.error();
        within_timeout = requireWithinTimeout(started, timeout_ms);
        if (within_timeout.isErr()) return within_timeout.error();
        return ConvertedValue{std::move(output), "Node"};
    }
    return Error(415, "Expression returned unsupported Variant type " + std::to_string(type));
}

double elapsedMilliseconds(std::chrono::steady_clock::time_point started) {
    return std::chrono::duration<double, std::milli>(
        std::chrono::steady_clock::now() - started).count();
}

Result<void> requireWithinTimeout(std::chrono::steady_clock::time_point started,
                                  int64_t timeout_ms) {
    if (elapsedMilliseconds(started) >= static_cast<double>(timeout_ms)) {
        return Error(408, "Expression evaluation exceeded timeout_ms");
    }
    return Result<void>::ok();
}

Result<std::string> expressionErrorText(GDExtensionObjectPtr expression) {
    auto error = callObject(expression, "Expression", "get_error_text", 201670096LL);
    if (error.isErr()) return error.error();
    return stringFromVariant(error.value(), GDEXTENSION_VARIANT_TYPE_STRING);
}

} // namespace

Result<void> ExpressionPolicy::validate(std::string_view source) {
    if (source.empty() || source.size() > kMaxSourceBytes ||
        source.find('\0') != std::string_view::npos || !isValidUtf8(source)) {
        return Error::invalidArgument("expression must be 1..2048 bytes of valid UTF-8 without NUL");
    }
    auto scanned = scanExpression(source);
    if (scanned.isErr()) return scanned.error();
    auto valid = validateTokens(scanned.value());
    if (valid.isErr()) return valid.error();
    auto reads = directPropertyReads(scanned.value());
    if (reads.isErr()) return reads.error();
    return Result<void>::ok();
}

json executeExpression(const json& params, const std::string& session_kind) {
    const auto started = std::chrono::steady_clock::now();
    if (!params.is_object()) return errorJson(400, "Expression params must be an object");
    if (session_kind != "editor" && session_kind != "game") {
        return errorJson(500, "Runtime session kind is unavailable");
    }
    if (!params.contains("expression") || !params["expression"].is_string()) {
        return errorJson(400, "expression is required and must be a string");
    }
    if (params.contains("context_node") && !params["context_node"].is_string()) {
        return errorJson(400, "context_node must be a string");
    }
    if (params.contains("timeout_ms") &&
        !params["timeout_ms"].is_number_integer() && !params["timeout_ms"].is_number_unsigned()) {
        return errorJson(400, "timeout_ms must be an integer from 1 to 5000");
    }
    int64_t timeout_ms = 1000;
    if (params.contains("timeout_ms")) {
        if (params["timeout_ms"].is_number_integer()) {
            timeout_ms = params["timeout_ms"].get<int64_t>();
        } else {
            const auto value = params["timeout_ms"].get<uint64_t>();
            if (value > static_cast<uint64_t>(std::numeric_limits<int64_t>::max())) {
                return errorJson(400, "timeout_ms must be an integer from 1 to 5000");
            }
            timeout_ms = static_cast<int64_t>(value);
        }
    }
    if (timeout_ms < 1 || timeout_ms > 5000) {
        return errorJson(400, "timeout_ms must be an integer from 1 to 5000");
    }

    const auto source = params["expression"].get<std::string>();
    auto policy = ExpressionPolicy::validate(source);
    if (policy.isErr()) return errorJson(403, "Unsafe expression: " + policy.error().message);
    if (params.contains("context_node")) {
        auto valid_path = validateContextPath(params["context_node"].get<std::string>());
        if (valid_path.isErr()) return errorJson(valid_path.error().code, valid_path.error().message);
    }
    auto within_timeout = requireWithinTimeout(started, timeout_ms);
    if (within_timeout.isErr()) return errorJson(within_timeout.error().code,
                                                  within_timeout.error().message);

    auto context = resolveContext(params, session_kind);
    if (context.isErr()) return errorJson(context.error().code, context.error().message);
    within_timeout = requireWithinTimeout(started, timeout_ms);
    if (within_timeout.isErr()) return errorJson(within_timeout.error().code,
                                                  within_timeout.error().message);

    std::string executable_source;
    std::vector<std::string> prebound_names;
    std::vector<VariantValue> prebound_values;
    auto prepared = prepareNativePropertyReads(source, context.value().node,
                                               executable_source, prebound_names,
                                               prebound_values);
    if (prepared.isErr()) return errorJson(prepared.error().code, prepared.error().message);
    within_timeout = requireWithinTimeout(started, timeout_ms);
    if (within_timeout.isErr()) return errorJson(within_timeout.error().code,
                                                  within_timeout.error().message);

    NativeName expression_class("Expression");
    if (!expression_class.valid() || !GodotApi::instance().classdb_construct_object) {
        return errorJson(500, "Godot Expression construction API is unavailable");
    }
    OwnedObject expression(GodotApi::instance().classdb_construct_object(expression_class.ptr()));
    if (!expression.get()) return errorJson(500, "Godot could not construct Expression");

    auto source_value = makeString(executable_source);
    auto input_names = makeContainer(GDEXTENSION_VARIANT_TYPE_PACKED_STRING_ARRAY);
    if (source_value.isErr()) return errorJson(source_value.error().code, source_value.error().message);
    if (input_names.isErr()) return errorJson(input_names.error().code, input_names.error().message);
    for (const auto* name : {"node", "tree"}) {
        auto name_value = makeString(name);
        if (name_value.isErr()) return errorJson(name_value.error().code, name_value.error().message);
        auto appended = callVariant(input_names.value(), "append", {&name_value.value()});
        if (appended.isErr()) return errorJson(appended.error().code, appended.error().message);
    }
    for (const auto& name : prebound_names) {
        auto name_value = makeString(name);
        if (name_value.isErr()) return errorJson(name_value.error().code,
                                                 name_value.error().message);
        auto appended = callVariant(input_names.value(), "append", {&name_value.value()});
        if (appended.isErr()) return errorJson(appended.error().code,
                                               appended.error().message);
    }

    auto parsed = callObject(expression.get(), "Expression", "parse", 3069722906LL,
                             {&source_value.value(), &input_names.value()});
    if (parsed.isErr()) return errorJson(parsed.error().code, parsed.error().message);
    auto parse_code = scalarFromVariant<int64_t>(parsed.value(), GDEXTENSION_VARIANT_TYPE_INT);
    if (parse_code.isErr()) return errorJson(parse_code.error().code, parse_code.error().message);
    if (parse_code.value() != 0) {
        auto detail = expressionErrorText(expression.get());
        return errorJson(422, detail.isOk() && !detail.value().empty()
                                  ? "Expression parse failed: " + detail.value()
                                  : "Expression parse failed");
    }
    within_timeout = requireWithinTimeout(started, timeout_ms);
    if (within_timeout.isErr()) return errorJson(within_timeout.error().code,
                                                  within_timeout.error().message);

    auto inputs = makeContainer(GDEXTENSION_VARIANT_TYPE_ARRAY);
    auto node_value = makeObject(context.value().node);
    auto tree_value = makeObject(context.value().tree);
    if (inputs.isErr()) return errorJson(inputs.error().code, inputs.error().message);
    if (node_value.isErr()) return errorJson(node_value.error().code, node_value.error().message);
    if (tree_value.isErr()) return errorJson(tree_value.error().code, tree_value.error().message);
    for (auto* input : {&node_value.value(), &tree_value.value()}) {
        auto appended = callVariant(inputs.value(), "append", {input});
        if (appended.isErr()) return errorJson(appended.error().code, appended.error().message);
    }
    for (auto& input : prebound_values) {
        auto appended = callVariant(inputs.value(), "append", {&input});
        if (appended.isErr()) return errorJson(appended.error().code,
                                               appended.error().message);
    }
    VariantValue base_instance;
    auto show_error = makeScalar(GDEXTENSION_VARIANT_TYPE_BOOL, static_cast<GDExtensionBool>(0));
    auto const_calls_only = makeScalar(GDEXTENSION_VARIANT_TYPE_BOOL, static_cast<GDExtensionBool>(1));
    if (show_error.isErr()) return errorJson(show_error.error().code, show_error.error().message);
    if (const_calls_only.isErr()) return errorJson(const_calls_only.error().code,
                                                   const_calls_only.error().message);

    auto result = callObject(expression.get(), "Expression", "execute", 3712471238LL,
                             {&inputs.value(), &base_instance, &show_error.value(),
                              &const_calls_only.value()});
    if (result.isErr()) return errorJson(result.error().code, result.error().message);
    auto failed_value = callObject(expression.get(), "Expression", "has_execute_failed", 36873697LL);
    if (failed_value.isErr()) return errorJson(failed_value.error().code, failed_value.error().message);
    auto failed = scalarFromVariant<GDExtensionBool>(failed_value.value(), GDEXTENSION_VARIANT_TYPE_BOOL);
    if (failed.isErr()) return errorJson(failed.error().code, failed.error().message);
    if (failed.value() != 0) {
        auto detail = expressionErrorText(expression.get());
        return errorJson(422, detail.isOk() && !detail.value().empty()
                                  ? "Expression execution failed: " + detail.value()
                                  : "Expression execution failed");
    }
    within_timeout = requireWithinTimeout(started, timeout_ms);
    if (within_timeout.isErr()) return errorJson(within_timeout.error().code,
                                                  within_timeout.error().message);

    ConversionBudget budget;
    auto converted = convertVariant(result.value(), budget, context.value(),
                                    started, timeout_ms);
    if (converted.isErr()) return errorJson(converted.error().code, converted.error().message);
    within_timeout = requireWithinTimeout(started, timeout_ms);
    if (within_timeout.isErr()) return errorJson(within_timeout.error().code,
                                                  within_timeout.error().message);
    json response = {
        {"context_node", context.value().canonical_path},
        {"value", std::move(converted.value().value)},
        {"value_type", converted.value().type},
        {"timeout_ms", timeout_ms},
        {"read_only", true},
        {"sandbox_profile", "expression_const_v1"},
        {"execution_mode", "live"},
        {"is_live_engine", true},
        {"session_kind", session_kind}
    };
    const auto response_size_without_timing = response.dump().size();
    if (response_size_without_timing > kMaxResultBytes - kResponseTimingReserve) {
        return errorJson(413, "Expression response exceeds the 256 KiB serialized limit");
    }
    within_timeout = requireWithinTimeout(started, timeout_ms);
    if (within_timeout.isErr()) return errorJson(within_timeout.error().code,
                                                  within_timeout.error().message);
    response["elapsed_ms"] = elapsedMilliseconds(started);
    return response;
}

} // namespace godot
} // namespace didi
