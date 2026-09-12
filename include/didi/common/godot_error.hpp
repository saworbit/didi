#pragma once

#include <string>

namespace didi::godot {

// The name behind a Godot `Error` value.
//
// The engine hands back an integer. Printing it and stopping there gave a
// caller "ResourceSaver.save failed with Error 19" and nothing that says the
// file could not be opened (#535). The table is Godot's own `Error` enum as
// published in extension_api.json; the values have been stable across 4.x, and
// an unknown one falls through to the number rather than to a guess.
inline std::string godotErrorName(long long value) {
    switch (value) {
        case 0: return "OK";
        case 1: return "FAILED";
        case 2: return "ERR_UNAVAILABLE";
        case 3: return "ERR_UNCONFIGURED";
        case 4: return "ERR_UNAUTHORIZED";
        case 5: return "ERR_PARAMETER_RANGE_ERROR";
        case 6: return "ERR_OUT_OF_MEMORY";
        case 7: return "ERR_FILE_NOT_FOUND";
        case 8: return "ERR_FILE_BAD_DRIVE";
        case 9: return "ERR_FILE_BAD_PATH";
        case 10: return "ERR_FILE_NO_PERMISSION";
        case 11: return "ERR_FILE_ALREADY_IN_USE";
        case 12: return "ERR_FILE_CANT_OPEN";
        case 13: return "ERR_FILE_CANT_WRITE";
        case 14: return "ERR_FILE_CANT_READ";
        case 15: return "ERR_FILE_UNRECOGNIZED";
        case 16: return "ERR_FILE_CORRUPT";
        case 17: return "ERR_FILE_MISSING_DEPENDENCIES";
        case 18: return "ERR_FILE_EOF";
        case 19: return "ERR_CANT_OPEN";
        case 20: return "ERR_CANT_CREATE";
        case 21: return "ERR_QUERY_FAILED";
        case 22: return "ERR_ALREADY_IN_USE";
        case 23: return "ERR_LOCKED";
        case 24: return "ERR_TIMEOUT";
        case 25: return "ERR_CANT_CONNECT";
        case 26: return "ERR_CANT_RESOLVE";
        case 27: return "ERR_CONNECTION_ERROR";
        case 28: return "ERR_CANT_ACQUIRE_RESOURCE";
        case 29: return "ERR_CANT_FORK";
        case 30: return "ERR_INVALID_DATA";
        case 31: return "ERR_INVALID_PARAMETER";
        case 32: return "ERR_ALREADY_EXISTS";
        case 33: return "ERR_DOES_NOT_EXIST";
        case 34: return "ERR_DATABASE_CANT_READ";
        case 35: return "ERR_DATABASE_CANT_WRITE";
        case 36: return "ERR_COMPILATION_FAILED";
        case 37: return "ERR_METHOD_NOT_FOUND";
        case 38: return "ERR_LINK_FAILED";
        case 39: return "ERR_SCRIPT_FAILED";
        case 40: return "ERR_CYCLIC_LINK";
        case 41: return "ERR_INVALID_DECLARATION";
        case 42: return "ERR_DUPLICATE_SYMBOL";
        case 43: return "ERR_PARSE_ERROR";
        case 44: return "ERR_BUSY";
        case 45: return "ERR_SKIP";
        case 46: return "ERR_HELP";
        case 47: return "ERR_BUG";
        case 48: return "ERR_PRINTER_ON_FIRE";
        default: break;
    }
    return "Error " + std::to_string(value);
}

// The same value with its number kept, for a message a human also reads.
inline std::string describeGodotError(long long value) {
    const auto name = godotErrorName(value);
    if (name.rfind("Error ", 0) == 0) return name;
    return name + " (" + std::to_string(value) + ")";
}

// Whether a Godot `Error` says the caller's path was the problem rather than
// the server. These are the file-and-path family: the fix is to send a
// different path, so the answer is a 400 and not a 500.
inline bool isGodotPathError(long long value) {
    switch (value) {
        case 7:   // ERR_FILE_NOT_FOUND
        case 8:   // ERR_FILE_BAD_DRIVE
        case 9:   // ERR_FILE_BAD_PATH
        case 12:  // ERR_FILE_CANT_OPEN
        case 13:  // ERR_FILE_CANT_WRITE
        case 19:  // ERR_CANT_OPEN
        case 20:  // ERR_CANT_CREATE
            return true;
        default: break;
    }
    return false;
}

} // namespace didi::godot
