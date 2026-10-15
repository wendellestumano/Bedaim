#pragma once

#include <charconv>
#include <cctype>
#include <optional>
#include <string>
#include <string_view>

namespace bedaim::autohitkey {

// Converts user-friendly names to Android key codes used by the BedrockTools
// launcher input bridge. A missing value means the input is incomplete/unknown;
// zero means explicit automatic mode ("auto", "off", or "none").
inline std::optional<int> parseMouseButton(std::string_view raw) {
    std::string key;
    key.reserve(raw.size());
    for (const unsigned char ch : raw) {
        if (std::isspace(ch) || ch == '-' || ch == '_') continue;
        key.push_back(static_cast<char>(std::tolower(ch)));
    }
    if (key.rfind("mouse", 0) == 0) key.erase(0, 5);
    else if (key.rfind("button", 0) == 0) key.erase(0, 6);
    else return std::nullopt;

    int button = 0;
    const auto parsed = std::from_chars(key.data(), key.data() + key.size(), button);
    if (key.empty() || parsed.ec != std::errc{} ||
        parsed.ptr != key.data() + key.size() || button < 1 || button > 16) {
        return std::nullopt;
    }
    return button;
}

inline std::optional<int> parse(std::string_view raw) {
    std::string key;
    key.reserve(raw.size());
    for (const unsigned char ch : raw) {
        if (std::isspace(ch) || ch == '-' || ch == '_') continue;
        key.push_back(static_cast<char>(std::tolower(ch)));
    }

    if (key.empty() || key == "auto" || key == "none" || key == "off") return 0;

    if (key.size() == 1 && key[0] >= '0' && key[0] <= '9')
        return 7 + (key[0] - '0'); // Android KEYCODE_0..KEYCODE_9.
    if (key.size() == 1 && key[0] >= 'a' && key[0] <= 'z')
        return 29 + (key[0] - 'a'); // Android KEYCODE_A..KEYCODE_Z.

    if (key == "shift" || key == "shiftleft") return 59;
    if (key == "shiftright") return 60;
    if (key == "alt" || key == "altleft") return 57;
    if (key == "altright") return 58;
    if (key == "ctrl" || key == "control" || key == "ctrlleft" || key == "controlleft") return 113;
    if (key == "ctrlright" || key == "controlright") return 114;
    if (key == "space" || key == "spacebar") return 62;
    if (key == "enter" || key == "return") return 66;
    if (key == "tab") return 61;
    if (key == "esc" || key == "escape") return 111;
    if (key == "backspace") return 67;
    if (key == "delete" || key == "del") return 112;

    if (key.rfind("button", 0) == 0 || key.rfind("mouse", 0) == 0)
        return std::nullopt; // mouse aliases are handled by parseMouseButton().

    std::string_view digits(key);
    if (digits.rfind("keycode:", 0) == 0) digits.remove_prefix(8);
    else if (digits.rfind("keycode", 0) == 0) digits.remove_prefix(7);
    if (!digits.empty()) {
        int code = 0;
        const auto parsed = std::from_chars(digits.data(), digits.data() + digits.size(), code);
        if (parsed.ec == std::errc{} && parsed.ptr == digits.data() + digits.size() &&
            code >= 1 && code <= 288) {
            return code;
        }
    }
    return std::nullopt;
}

inline std::string format(int code) {
    if (code <= 0) return "auto";
    if (code >= 7 && code <= 16) return std::string(1, static_cast<char>('0' + code - 7));
    if (code >= 29 && code <= 54) return std::string(1, static_cast<char>('a' + code - 29));
    switch (code) {
        case 57: return "alt";
        case 58: return "alt right";
        case 59: return "shift";
        case 60: return "shift right";
        case 61: return "tab";
        case 62: return "space";
        case 66: return "enter";
        case 67: return "backspace";
        case 111: return "esc";
        case 112: return "delete";
        case 113: return "ctrl";
        case 114: return "ctrl right";
        default: break;
    }
    return "keycode:" + std::to_string(code);
}

} // namespace bedaim::autohitkey
