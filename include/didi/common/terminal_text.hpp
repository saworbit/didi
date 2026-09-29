#pragma once

#include <string>

namespace didi {

// Text with the terminal control sequences taken out: carriage returns, and
// escape sequences, CSI (colour, cursor movement) and two-character alike.
//
// Godot colours its console output, and Didi hands some of it to agents: an
// export failure used to arrive as four kilobytes of escapes and progress bars
// with the one actionable line sixty lines down (#651), and runtime_read_output
// relayed the colouring on 40 of a fresh headless editor's first 41 records
// (#1028). A client that renders either into a terminal would execute them,
// and a match against the visible text fails.
inline std::string withoutTerminalEscapes(const std::string& text) {
    std::string out;
    out.reserve(text.size());
    for (size_t index = 0; index < text.size(); ++index) {
        if (text[index] == '\r') continue;
        if (text[index] != '\x1b') {
            out += text[index];
            continue;
        }
        // CSI and the two-character sequences alike: skip to the byte that ends
        // the sequence rather than trying to understand it.
        ++index;
        if (index < text.size() && text[index] == '[') {
            ++index;
            while (index < text.size() && !(text[index] >= '@' && text[index] <= '~')) ++index;
        }
    }
    return out;
}

} // namespace didi
