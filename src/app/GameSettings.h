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
#include <array>
#include <cstdio>
#include <cstdlib>
#include <fstream>
#include <sstream>
#include <string>
#include "../gameplay/weapons/WeaponCustomization.h"

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
    float weaponsVolume = kDefaultWeaponsVolume;
    float voicesVolume = 1.0f;
    float ambienceVolume = 1.0f;
    float uiVolume = 1.0f;
    // Music sits under the rest by default, matching AudioDevice's own table:
    // a score that competes with gunfire and callouts is a score the player
    // turns off. Deliberately not 1.0 -- defaulting it flat here would quietly
    // undo that mix for everyone who never opens this menu.
    float musicVolume = 0.55f;

    // Display settings. These control window mode and camera presentation, and
    // are resolved in ApplyGameSettings() which runs at boot and after any
    // level load or editor entry.

    // Vertical sync. On maps to scene.vsyncInterval = 1 (every vblank); the
    // editor panel still offers 2..4. Off by default because Scene's own
    // default is 0 -- the shipping frame pacing does not change for a player
    // who never opens this menu.
    bool vsync = false;

    // DLSS reconstructs a smaller visibility render into the display target;
    // at 100% it runs native DLAA. Keep it opt-in until measured.
    bool dlssEnabled = false;
    // Ray-traced quality tier. Ultra traces every pixel (no confidence
    // classification) and denoises with DLSS Ray Reconstruction at native
    // resolution. Off leaves ray tracing to the editor's own toggles.
    int rayTracingQuality = 0;
    // Lumen global illumination: ray-traced diffuse bounce lighting.
    bool lumenGI = false;
    // Separate diffuse GI backend; never add two estimates of the same bounce.
    bool radianceCascadesGI = false;
    // Opt-in sample sharing; full-resolution GI remains the parity reference.
    bool lumenGIHalfResolution = false;
    // Ultra: Ray Reconstruction upscales from the DLSS screen percentage
    // instead of running at native resolution. Measured at 1080p, 50%:
    // RR 9.5 -> 2.9 ms, GPU frame 23.2 -> 10.5 ms.
    bool rayReconstructionUpscale = true;
    // Upscaling RR: rebuild RR's guides and motion for forward-drawn pixels
    // (grass, palms, props) instead of reusing the surface behind them.
    bool rrForwardGuides = true;
    // DLSS Super Resolution: real motion vectors for grass and forward props
    // (the grass composite otherwise hands DLSS a reactive marker).
    bool dlssForwardMotion = true;
    // Surfaces rougher than this skip ray-traced reflections and use the
    // reflection probe. Applies while Ray Reconstruction or Lumen GI is on;
    // 1.0 traces every surface (the Ultra behaviour before this setting).
    float rtReflectionRoughnessCutoff = 1.0f;
    static constexpr float kMinRTReflectionRoughnessCutoff = 0.05f;
    static constexpr float kMaxRTReflectionRoughnessCutoff = 1.0f;
    static constexpr int kRayTracingOff = 0;
    static constexpr int kRayTracingUltra = 1;
    static constexpr int kMinRayTracingQuality = kRayTracingOff;
    static constexpr int kMaxRayTracingQuality = kRayTracingUltra;
    // Streamline DLSS preset: K=0, L=1, M=2. L is the SDK default.
    int dlssPreset = 1;
    static constexpr int kMinDLSSPreset = 0;
    static constexpr int kMaxDLSSPreset = 2;
    static constexpr int kDefaultDLSSPreset = 1;
    bool extensionMotionVectors = false;
    float dlssScreenPercentage = 75.0f;
    static constexpr float kMinDLSSScreenPercentage = 33.0f;
    static constexpr float kMaxDLSSScreenPercentage = 100.0f;
    static constexpr float kDefaultDLSSScreenPercentage = 75.0f;

    // Borderless fullscreen, the mode the game has always booted into. Read at
    // window creation; afterwards the main loop moves the window to match it,
    // and F11 writes it back, so the menu and the key agree.
    bool fullscreen = true;

    // Hip-fire field of view in vertical degrees. The camera and ADS blend
    // from this. Clamped to a usable range so a corrupt hand-edited file
    // cannot produce an unusable view.
    float fieldOfView = 60.0f;
    static constexpr float kMinFieldOfView = 50.0f;
    static constexpr float kMaxFieldOfView = 100.0f;

    // Inverts the Y axis in ProcessMouseMovement so the camera pans down when
    // the mouse moves down (and vice versa). Intended for players who grew up
    // on flight sims rather than FPS games.
    bool invertMouseY = false;

    // Walking/running camera bob multiplier. 0 is off, 1 is the stock bob.
    float cameraBob = 1.0f;
    static constexpr float kMinCameraBob = 0.0f;
    static constexpr float kMaxCameraBob = 3.0f;
    static constexpr float kDefaultCameraBob = 1.0f;

    // Visual firing jolt for each weapon ID. Aim recoil and blast shake are
    // independent, so zero removes the camera jolt without changing accuracy.
    static constexpr float kDefaultWeaponCameraShake = 1.0f;
    static constexpr float kMaxWeaponCameraShake = 30.0f;
    std::array<float, SGE::WeaponCustomizationSystem::kWeaponCount>
        weaponCameraShake = [] {
            std::array<float, SGE::WeaponCustomizationSystem::kWeaponCount> values{};
            values.fill(kDefaultWeaponCameraShake);
            return values;
        }();

    static constexpr float kDefaultMasterVolume = 1.0f;
    static constexpr float kDefaultBusVolume = 1.0f;
    static constexpr float kDefaultWeaponsVolume = 0.9f;
    static constexpr float kDefaultMusicVolume = 0.55f;

    static constexpr bool  kDefaultRealisticAiming = false;
    static constexpr bool  kDefaultShowCrosshair = true;
    static constexpr bool  kDefaultDebugLoadingScreen = false;
    static constexpr bool  kDefaultSeeThroughWeapon = false;
    static constexpr float kDefaultSeeThroughStrength = 1.0f;
    static constexpr float kMinSeeThroughStrength = 0.0f;
    static constexpr float kMaxSeeThroughStrength = 1.0f;

    static constexpr bool  kDefaultVsync = false;
    static constexpr bool  kDefaultDLSS = false;
    static constexpr int   kDefaultRayTracingQuality = kRayTracingOff;
    static constexpr bool  kDefaultLumenGI = false;
    static constexpr bool  kDefaultRadianceCascadesGI = false;
    static constexpr bool  kDefaultLumenGIHalfResolution = false;
    static constexpr bool  kDefaultRayReconstructionUpscale = true;
    static constexpr bool  kDefaultRRForwardGuides = true;
    static constexpr bool  kDefaultDLSSForwardMotion = true;
    static constexpr float kDefaultRTReflectionRoughnessCutoff = 1.0f;
    static constexpr bool  kDefaultExtensionMotionVectors = false;
    static constexpr bool  kDefaultFullscreen = true;
    static constexpr float kDefaultFieldOfView = 60.0f;
    static constexpr bool  kDefaultInvertMouseY = false;

    void Clamp() {
        if (radianceCascadesGI) lumenGI = false;
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
        // FOV must stay within view angles a player can handle, whatever a
        // hand-edited file says.
        cameraBob = (std::max)(kMinCameraBob,
                    (std::min)(kMaxCameraBob, cameraBob));
        fieldOfView = (std::max)(kMinFieldOfView,
                      (std::min)(kMaxFieldOfView, fieldOfView));
        dlssPreset = (std::max)(kMinDLSSPreset,
                     (std::min)(kMaxDLSSPreset, dlssPreset));
        rayTracingQuality = (std::max)(kMinRayTracingQuality,
                            (std::min)(kMaxRayTracingQuality, rayTracingQuality));
        rtReflectionRoughnessCutoff = (std::max)(
            kMinRTReflectionRoughnessCutoff,
            (std::min)(kMaxRTReflectionRoughnessCutoff,
                       rtReflectionRoughnessCutoff));
        dlssScreenPercentage = dlssScreenPercentage >=
            kMinDLSSScreenPercentage
            ? (std::min)(kMaxDLSSScreenPercentage, dlssScreenPercentage)
            : kMinDLSSScreenPercentage;
        for (float& shake : weaponCameraShake)
            shake = shake > 0.0f ? (std::min)(kMaxWeaponCameraShake, shake)
                                 : 0.0f;
    }

    void ResetToDefaults() {
        mouseSensitivity = kDefaultSensitivity;
        seeThroughWeaponWhenAiming = kDefaultSeeThroughWeapon;
        seeThroughWeaponStrength = kDefaultSeeThroughStrength;
        realisticAiming = kDefaultRealisticAiming;
        showCrosshair = kDefaultShowCrosshair;
        debugLoadingScreen = kDefaultDebugLoadingScreen;
        masterVolume = kDefaultMasterVolume;
        weaponsVolume = kDefaultWeaponsVolume;
        voicesVolume = kDefaultBusVolume;
        ambienceVolume = kDefaultBusVolume;
        uiVolume = kDefaultBusVolume;
        musicVolume = kDefaultMusicVolume;
        vsync = kDefaultVsync;
        dlssEnabled = kDefaultDLSS;
        rayTracingQuality = kDefaultRayTracingQuality;
        lumenGI = kDefaultLumenGI;
        radianceCascadesGI = kDefaultRadianceCascadesGI;
        lumenGIHalfResolution = kDefaultLumenGIHalfResolution;
        rayReconstructionUpscale = kDefaultRayReconstructionUpscale;
        rrForwardGuides = kDefaultRRForwardGuides;
        dlssForwardMotion = kDefaultDLSSForwardMotion;
        rtReflectionRoughnessCutoff = kDefaultRTReflectionRoughnessCutoff;
        dlssPreset = kDefaultDLSSPreset;
        extensionMotionVectors = kDefaultExtensionMotionVectors;
        dlssScreenPercentage = kDefaultDLSSScreenPercentage;
        fullscreen = kDefaultFullscreen;
        fieldOfView = kDefaultFieldOfView;
        cameraBob = kDefaultCameraBob;
        invertMouseY = kDefaultInvertMouseY;
        weaponCameraShake.fill(kDefaultWeaponCameraShake);
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
        else if (key == "VSync") {
            out.vsync =
                value == "1" || value == "true" || value == "yes";
        }
        else if (key == "DLSS") {
            out.dlssEnabled =
                value == "1" || value == "true" || value == "yes";
        }
        else if (key == "RayTracingQuality") {
            out.rayTracingQuality = static_cast<int>(std::strtol(value.c_str(), nullptr, 10));
        }
        else if (key == "LumenGI") {
            out.lumenGI =
                value == "1" || value == "true" || value == "yes";
        }
        else if (key == "LumenGIHalfResolution") {
            out.lumenGIHalfResolution =
                value == "1" || value == "true" || value == "yes";
        }
        else if (key == "RadianceCascadesGI") {
            out.radianceCascadesGI =
                value == "1" || value == "true" || value == "yes";
        }
        else if (key == "RayReconstructionUpscale") {
            out.rayReconstructionUpscale =
                value == "1" || value == "true" || value == "yes";
        }
        else if (key == "RRForwardGuides") {
            out.rrForwardGuides =
                value == "1" || value == "true" || value == "yes";
        }
        else if (key == "DLSSForwardMotion") {
            out.dlssForwardMotion =
                value == "1" || value == "true" || value == "yes";
        }
        else if (key == "RTReflectionRoughnessCutoff") {
            out.rtReflectionRoughnessCutoff =
                std::strtof(value.c_str(), nullptr);
        }
        else if (key == "DLSSRayReconstruction") {
            // Legacy key from before the quality tier; RR alone now means Ultra.
            const bool enabled = value == "1" || value == "true" || value == "yes";
            if (enabled) out.rayTracingQuality = GameSettings::kRayTracingUltra;
        }
        else if (key == "DLSSPreset") {
            out.dlssPreset = static_cast<int>(std::strtol(value.c_str(), nullptr, 10));
        }
        else if (key == "ExtensionMotionVectors") {
            out.extensionMotionVectors =
                value == "1" || value == "true" || value == "yes";
        }
        else if (key == "DLSSScreenPercentage") {
            out.dlssScreenPercentage = std::strtof(value.c_str(), nullptr);
        }
        else if (key == "Fullscreen") {
            out.fullscreen =
                value == "1" || value == "true" || value == "yes";
        }
        else if (key == "FieldOfView") {
            out.fieldOfView = std::strtof(value.c_str(), nullptr);
        }
        else if (key == "CameraBob") {
            out.cameraBob = std::strtof(value.c_str(), nullptr);
        }
        else if (key == "InvertMouseY") {
            out.invertMouseY =
                value == "1" || value == "true" || value == "yes";
        }
        else if (key.rfind("WeaponCameraShake", 0) == 0) {
            const std::string suffix = key.substr(sizeof("WeaponCameraShake") - 1);
            char* end = nullptr;
            const long weapon = std::strtol(suffix.c_str(), &end, 10);
            if (!suffix.empty() && *end == '\0' && weapon >= 0 &&
                weapon < static_cast<long>(out.weaponCameraShake.size()))
                out.weaponCameraShake[static_cast<size_t>(weapon)] =
                    std::strtof(value.c_str(), nullptr);
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
         << "InvertMouseY="
         << (settings.invertMouseY ? 1 : 0) << "\n"
         << "[Gameplay]\n"
         << "SeeThroughWeaponWhenAiming="
         << (settings.seeThroughWeaponWhenAiming ? 1 : 0) << "\n"
         << "SeeThroughWeaponStrength="
         << settings.seeThroughWeaponStrength << "\n"
         << "RealisticAiming="
         << (settings.realisticAiming ? 1 : 0) << "\n"
         << "[Display]\n"
         << "VSync="
         << (settings.vsync ? 1 : 0) << "\n"
         << "DLSS="
         << (settings.dlssEnabled ? 1 : 0) << "\n"
         << "RayTracingQuality="
         << settings.rayTracingQuality << "\n"
         << "LumenGI="
         << (settings.lumenGI ? 1 : 0) << "\n"
         << "RadianceCascadesGI="
         << (settings.radianceCascadesGI ? 1 : 0) << "\n"
         << "LumenGIHalfResolution="
         << (settings.lumenGIHalfResolution ? 1 : 0) << "\n"
         << "RayReconstructionUpscale="
         << (settings.rayReconstructionUpscale ? 1 : 0) << "\n"
         << "RRForwardGuides="
         << (settings.rrForwardGuides ? 1 : 0) << "\n"
         << "DLSSForwardMotion="
         << (settings.dlssForwardMotion ? 1 : 0) << "\n"
         << "RTReflectionRoughnessCutoff="
         << settings.rtReflectionRoughnessCutoff << "\n"
         << "DLSSPreset="
         << settings.dlssPreset << "\n"
         << "ExtensionMotionVectors="
         << (settings.extensionMotionVectors ? 1 : 0) << "\n"
         << "DLSSScreenPercentage="
         << settings.dlssScreenPercentage << "\n"
         << "Fullscreen="
         << (settings.fullscreen ? 1 : 0) << "\n"
         << "FieldOfView=" << settings.fieldOfView << "\n"
         << "CameraBob=" << settings.cameraBob << "\n"
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
    file << "[WeaponCameraShake]\n";
    for (size_t weapon = 0; weapon < settings.weaponCameraShake.size(); ++weapon)
        file << "WeaponCameraShake" << weapon << '='
             << settings.weaponCameraShake[weapon] << '\n';
    return file.good();
}
