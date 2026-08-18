#pragma once

// Player-facing settings that survive a restart.
//
// Everything the player could change previously lived only in memory, so every
// preference reset on launch. This owns the small set of values that belong to
// the player rather than to a level, and reads/writes them as a plain INI next
// to the executable.
//
// INI rather than JSON: the file is meant to be hand-editable when a setting
// puts the game in a state the menu cannot undo (an unreadable resolution, an
// unusable sensitivity), and it avoids pulling a parser into the startup path.
// Unknown keys are ignored rather than dropped, so a file written by a newer
// build still loads on an older one.

#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <fstream>
#include <sstream>
#include <string>

struct GameSettings {
    // Multiplies raw mouse delta in Camera::ProcessMouseMovement. The camera's
    // own constructor default is 0.1f; keep these in step so a missing INI and
    // a fresh camera agree.
    float mouseSensitivity = 0.1f;

    // Bounds for the slider and for clamping whatever the file contains. The
    // low end still turns, and the high end is fast rather than unusable --
    // a corrupt or hand-edited file must never produce an uncontrollable or
    // completely frozen camera.
    static constexpr float kMinSensitivity = 0.01f;
    static constexpr float kMaxSensitivity = 1.0f;

    static constexpr float kDefaultSensitivity = 0.1f;

    void Clamp() {
        mouseSensitivity = (std::max)(kMinSensitivity,
                           (std::min)(kMaxSensitivity, mouseSensitivity));
    }

    void ResetToDefaults() {
        mouseSensitivity = kDefaultSensitivity;
    }
};

// Path is relative to the working directory, which is where the engine already
// looks for shaders/, prefabs/ and Content/ -- so the file lands beside the exe
// in both a dev run and a packaged build.
inline const char* GameSettingsPath() { return "settings.ini"; }

// Missing file is not a failure: it is the first run. The caller keeps the
// defaults already in `out` and carries on.
inline bool LoadGameSettings(GameSettings& out) {
    std::ifstream file(GameSettingsPath());
    if (!file) return false;

    std::string line;
    while (std::getline(file, line)) {
        // Strip comments and section headers. Sections are accepted and ignored
        // so the file can grow into [Graphics]/[Audio] groups later without an
        // older build choking on them.
        const size_t comment = line.find_first_of(";#");
        if (comment != std::string::npos) line.erase(comment);
        const size_t equals = line.find('=');
        if (equals == std::string::npos) continue;

        auto trim = [](std::string s) {
            const size_t first = s.find_first_not_of(" \t\r\n");
            if (first == std::string::npos) return std::string();
            const size_t last = s.find_last_not_of(" \t\r\n");
            return s.substr(first, last - first + 1);
        };
        const std::string key = trim(line.substr(0, equals));
        const std::string value = trim(line.substr(equals + 1));
        if (key.empty() || value.empty()) continue;

        if (key == "MouseSensitivity") {
            // strtof over stof: a malformed value yields 0 and sets no
            // exception, and the clamp below turns that into the minimum
            // rather than throwing out of a file read.
            out.mouseSensitivity = std::strtof(value.c_str(), nullptr);
        }
    }

    // Whatever the file said, the result has to be usable.
    out.Clamp();
    return true;
}

inline bool SaveGameSettings(const GameSettings& settings) {
    std::ofstream file(GameSettingsPath(), std::ios::trunc);
    if (!file) return false;
    file << "; Smallest Graphics Engine settings.\n"
         << "; Delete this file to restore defaults.\n"
         << "[Input]\n"
         << "MouseSensitivity=" << settings.mouseSensitivity << "\n";
    return file.good();
}
