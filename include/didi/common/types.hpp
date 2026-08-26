#pragma once

#include <string>
#include <string_view>
#include <vector>
#include <optional>
#include <variant>
#include <functional>
#include <sstream>
#include <algorithm>
#include <cstdint>
#include <memory>
#include "json.hpp"

namespace didi {

using json = nlohmann::json;

struct Error {
    int code{0};
    std::string message;
    json data;

    Error() = default;
    Error(int c, std::string msg, json d = nullptr)
        : code(c), message(std::move(msg)), data(std::move(d)) {}

    static Error notFound(std::string msg) { return Error(404, std::move(msg)); }
    static Error invalidArgument(std::string msg) { return Error(400, std::move(msg)); }
    static Error internal(std::string msg) { return Error(500, std::move(msg)); }
    static Error notConnected(std::string msg = "Godot Editor GDExtension is not connected") {
        return Error(503, std::move(msg));
    }
};

template <typename T>
class Result {
public:
    Result(T value) : m_data(std::move(value)) {}
    Result(Error err) : m_data(std::move(err)) {}

    bool isOk() const { return std::holds_alternative<T>(m_data); }
    bool isErr() const { return std::holds_alternative<Error>(m_data); }

    const T& value() const { return std::get<T>(m_data); }
    T& value() { return std::get<T>(m_data); }

    const Error& error() const { return std::get<Error>(m_data); }
    Error& error() { return std::get<Error>(m_data); }

    T valueOr(T default_val) const {
        return isOk() ? std::get<T>(m_data) : default_val;
    }

private:
    std::variant<T, Error> m_data;
};

// Void specialization for Result
template <>
class Result<void> {
public:
    Result() : m_error(std::nullopt) {}
    Result(Error err) : m_error(std::move(err)) {}

    bool isOk() const { return !m_error.has_value(); }
    bool isErr() const { return m_error.has_value(); }

    const Error& error() const { return *m_error; }

    static Result<void> ok() { return Result<void>(); }

private:
    std::optional<Error> m_error;
};

// Math & Spatial structures
struct Vector2D {
    double x{0.0};
    double y{0.0};

    json toJson() const { return {{"x", x}, {"y", y}}; }
    static Vector2D fromJson(const json& j) {
        Vector2D v;
        if (j.is_object()) {
            v.x = j.value("x", 0.0);
            v.y = j.value("y", 0.0);
        } else if (j.is_array() && j.size() >= 2) {
            v.x = j[0].get<double>();
            v.y = j[1].get<double>();
        }
        return v;
    }
};

struct Vector3D {
    double x{0.0};
    double y{0.0};
    double z{0.0};

    json toJson() const { return {{"x", x}, {"y", y}, {"z", z}}; }
    static Vector3D fromJson(const json& j) {
        Vector3D v;
        if (j.is_object()) {
            v.x = j.value("x", 0.0);
            v.y = j.value("y", 0.0);
            v.z = j.value("z", 0.0);
        } else if (j.is_array() && j.size() >= 3) {
            v.x = j[0].get<double>();
            v.y = j[1].get<double>();
            v.z = j[2].get<double>();
        }
        return v;
    }
};

struct ColorRGBA {
    float r{1.0f};
    float g{1.0f};
    float b{1.0f};
    float a{1.0f};

    json toJson() const { return {{"r", r}, {"g", g}, {"b", b}, {"a", a}}; }
    static ColorRGBA fromJson(const json& j) {
        ColorRGBA c;
        if (j.is_object()) {
            c.r = j.value("r", 1.0f);
            c.g = j.value("g", 1.0f);
            c.b = j.value("b", 1.0f);
            c.a = j.value("a", 1.0f);
        }
        return c;
    }
};

struct Transform3DData {
    Vector3D position;
    Vector3D rotation; // Euler angles in degrees
    Vector3D scale{1.0, 1.0, 1.0};

    json toJson() const {
        return {
            {"position", position.toJson()},
            {"rotation", rotation.toJson()},
            {"scale", scale.toJson()}
        };
    }

    static Transform3DData fromJson(const json& j) {
        Transform3DData t;
        if (j.contains("position")) t.position = Vector3D::fromJson(j["position"]);
        if (j.contains("rotation")) t.rotation = Vector3D::fromJson(j["rotation"]);
        if (j.contains("scale")) t.scale = Vector3D::fromJson(j["scale"]);
        return t;
    }
};

// String helper utilities
namespace strings {
    inline std::string trim(std::string_view s) {
        auto start = s.find_first_not_of(" \t\n\r");
        if (start == std::string_view::npos) return "";
        auto end = s.find_last_not_of(" \t\n\r");
        return std::string(s.substr(start, end - start + 1));
    }

    inline bool startsWith(std::string_view str, std::string_view prefix) {
        return str.size() >= prefix.size() && str.substr(0, prefix.size()) == prefix;
    }

    inline bool endsWith(std::string_view str, std::string_view suffix) {
        return str.size() >= suffix.size() && str.substr(str.size() - suffix.size()) == suffix;
    }

    inline std::vector<std::string> split(std::string_view str, char delim) {
        std::vector<std::string> result;
        size_t start = 0;
        size_t end = str.find(delim);
        while (end != std::string_view::npos) {
            result.emplace_back(str.substr(start, end - start));
            start = end + 1;
            end = str.find(delim, start);
        }
        result.emplace_back(str.substr(start));
        return result;
    }

    inline std::string replaceAll(std::string str, const std::string& from, const std::string& to) {
        if (from.empty()) return str;
        size_t start_pos = 0;
        while ((start_pos = str.find(from, start_pos)) != std::string::npos) {
            str.replace(start_pos, from.length(), to);
            start_pos += to.length();
        }
        return str;
    }
}

} // namespace didi
