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

    // Fade the sighted weapon toward see-through, approximating what the off
    // eye gives a shooter at cheek weld. Opt-in: a solid weapon is the normal
    // presentation, and a player who has not asked for this should never have
    // their gun turn translucent on them.
    bool seeThroughWeaponWhenAiming = false;

    // How far the fade is taken. 1 is the tuned look; lower values keep more
    // of the weapon, so a player who finds it distracting can dial it back
    // without giving up the clearer sight picture entirely.
    float seeThroughWeaponStrength = 1.0f;

    // Show the full load trace -- stage timings, upload counts, DX12 resource
    // state -- on the loading screen. Off is the shipping presentation: a
    // player waiting for a level wants to know it is loading, not read a
    // renderer diagnostic. On is what is needed when a load stalls, and those
    // numbers are useless if turning them on means a recompile.
    bool debugLoadingScreen = false;

    // Aiming model. Classic is the shipping one: the camera and the muzzle are
    // the same ray, so where you look is where you shoot. Realistic decouples
    // them -- the gun leads and the camera follows on a spring, so a fast flick
    // swings the body rather than teleporting the muzzle. It changes how the
    // weapon handles, not just how it looks, so it belongs to the player and
    // survives a restart rather than living on a dev key.
    bool realisticAiming = false;

    // Hip-fire crosshair. On is the shipping presentation. Off is for players
    // who want the weapon's own sights to be the only aiming reference; the
    // optic dot and scope reticles are unaffected, since those are the sight
    // picture rather than a HUD overlay.
    bool showCrosshair = true;

    // Audio mix, 0..1 each. These mirror the submix graph in GunAudio.h: one
    // master trim and one fader per bus. Stored here rather than left on the
    // AudioDevice because the device's values are runtime-only -- the editor
    // panel has always written straight through to the live voices, so a mix
    // set there was gone on the next launch. A player who turns the music down
    // expects it to stay down.
    //
    // Defaults match AudioDevice's own, so a missing file and a fresh device
    // agree the same way the sensitivity above does.
    float masterVolume = 1.0f;
    float weaponsVolume = 1.0f;
    float voicesVolume = 1.0f;
    float ambienceVolume = 1.0f;
    float uiVolume = 1.0f;
    // Music sits under the rest by default, matching AudioDevice's own table:
    // a score that competes with gunfire and callouts is a score the player
    // turns off. Deliberately not 1.0 -- defaulting it flat here would quietly
    // undo that mix for everyone who never opens this menu.
    float musicVolume = 0.55f;

    static constexpr float kDefaultMasterVolume = 1.0f;
    static constexpr float kDefaultBusVolume = 1.0f;
    static constexpr float kDefaultMusicVolume = 0.55f;

    static constexpr bool  kDefaultRealisticAiming = false;
    static constexpr bool  kDefaultShowCrosshair = true;
    static constexpr bool  kDefaultDebugLoadingScreen = false;
    static constexpr bool  kDefaultSeeThroughWeapon = false;
    static constexpr float kDefaultSeeThroughStrength = 1.0f;
    static constexpr float kMinSeeThroughStrength = 0.0f;
    static constexpr float kMaxSeeThroughStrength = 1.0f;

    void Clamp() {
        mouseSensitivity = (std::max)(kMinSensitivity,
                           (std::min)(kMaxSensitivity, mouseSensitivity));
        seeThroughWeaponStrength =
            (std::max)(kMinSeeThroughStrength,
            (std::min)(kMaxSeeThroughStrength, seeThroughWeaponStrength));
        // A hand-edited or corrupt file must not be able to drive a voice above
        // unity, which clips, or below zero, which XAudio2 rejects outright.
        // NaN fails both comparisons and falls through to 0, which is silent
        // rather than undefined -- the same posture the sensitivity takes.
        const auto volume = [](float& value) {
            value = value > 0.0f ? (value < 1.0f ? value : 1.0f) : 0.0f;
        };
        volume(masterVolume);
        volume(weaponsVolume);
        volume(voicesVolume);
        volume(ambienceVolume);
        volume(uiVolume);
        volume(musicVolume);
    }

    void ResetToDefaults() {
        mouseSensitivity = kDefaultSensitivity;
        seeThroughWeaponWhenAiming = kDefaultSeeThroughWeapon;
        seeThroughWeaponStrength = kDefaultSeeThroughStrength;
        realisticAiming = kDefaultRealisticAiming;
        showCrosshair = kDefaultShowCrosshair;
        debugLoadingScreen = kDefaultDebugLoadingScreen;
        masterVolume = kDefaultMasterVolume;
        weaponsVolume = kDefaultBusVolume;
        voicesVolume = kDefaultBusVolume;
        ambienceVolume = kDefaultBusVolume;
        uiVolume = kDefaultBusVolume;
        musicVolume = kDefaultMusicVolume;
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
        else if (key == "SeeThroughWeaponWhenAiming") {
            // Several spellings accepted so a hand-edited file does not hinge
            // on guessing which one the writer used. Anything else is off,
            // which is also what a missing key gives.
            out.seeThroughWeaponWhenAiming =
                value == "1" || value == "true" || value == "yes";
        }
        else if (key == "SeeThroughWeaponStrength") {
            out.seeThroughWeaponStrength = std::strtof(value.c_str(), nullptr);
        }
        else if (key == "RealisticAiming") {
            out.realisticAiming =
                value == "1" || value == "true" || value == "yes";
        }
        else if (key == "ShowCrosshair") {
            // Unlike the others this one defaults on, so the test is for the
            // off spellings. A missing key never reaches here and keeps the
            // default; anything unrecognised reads as on, which is the
            // presentation a player who never touched the setting expects.
            out.showCrosshair =
                !(value == "0" || value == "false" || value == "no");
        }
        else if (key == "DebugLoadingScreen") {
            out.debugLoadingScreen =
                value == "1" || value == "true" || value == "yes";
        }
        // strtof for the same reason as the sensitivity: a malformed value
        // yields 0 and the clamp turns it into silence rather than throwing
        // out of a file read.
        else if (key == "MasterVolume") {
            out.masterVolume = std::strtof(value.c_str(), nullptr);
        }
        else if (key == "WeaponsVolume") {
            out.weaponsVolume = std::strtof(value.c_str(), nullptr);
        }
        else if (key == "VoicesVolume") {
            out.voicesVolume = std::strtof(value.c_str(), nullptr);
        }
        else if (key == "AmbienceVolume") {
            out.ambienceVolume = std::strtof(value.c_str(), nullptr);
        }
        else if (key == "UIVolume") {
            out.uiVolume = std::strtof(value.c_str(), nullptr);
        }
        else if (key == "MusicVolume") {
            out.musicVolume = std::strtof(value.c_str(), nullptr);
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
         << "MouseSensitivity=" << settings.mouseSensitivity << "\n"
         << "[Gameplay]\n"
         << "SeeThroughWeaponWhenAiming="
         << (settings.seeThroughWeaponWhenAiming ? 1 : 0) << "\n"
         << "SeeThroughWeaponStrength="
         << settings.seeThroughWeaponStrength << "\n"
         << "RealisticAiming="
         << (settings.realisticAiming ? 1 : 0) << "\n"
         << "[HUD]\n"
         << "ShowCrosshair="
         << (settings.showCrosshair ? 1 : 0) << "\n"
         << "[Audio]\n"
         << "MasterVolume=" << settings.masterVolume << "\n"
         << "WeaponsVolume=" << settings.weaponsVolume << "\n"
         << "VoicesVolume=" << settings.voicesVolume << "\n"
         << "AmbienceVolume=" << settings.ambienceVolume << "\n"
         << "UIVolume=" << settings.uiVolume << "\n"
         << "MusicVolume=" << settings.musicVolume << "\n"
         << "[Debug]\n"
         << "DebugLoadingScreen="
         << (settings.debugLoadingScreen ? 1 : 0) << "\n";
    return file.good();
}
