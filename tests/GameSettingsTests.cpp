#include "GameSettings.h"

#include <cmath>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <iostream>

static int failures = 0;
#define CHECK(value) do { if (!(value)) { \
    std::cerr << __FILE__ << ':' << __LINE__ << " CHECK failed: " #value "\n"; \
    ++failures; } } while (false)

// The settings file is read from the working directory, so each case runs in a
// scratch directory of its own rather than writing next to the test binary.
static void WriteSettingsFile(const std::string& body) {
    std::ofstream file(GameSettingsPath(), std::ios::trunc);
    file << body;
}

int main() {
    const std::filesystem::path scratch =
        std::filesystem::temp_directory_path() / "sge_game_settings_tests";
    std::filesystem::create_directories(scratch);
    std::filesystem::current_path(scratch);
    std::filesystem::remove(GameSettingsPath());

    // A missing file is the first run, not a failure: the caller keeps its
    // defaults and the see-through effect stays off until it is asked for.
    {
        GameSettings settings;
        CHECK(!LoadGameSettings(settings));
        CHECK(!settings.seeThroughWeaponWhenAiming);
        CHECK(settings.seeThroughWeaponStrength ==
              GameSettings::kDefaultSeeThroughStrength);
    }

    // A file written by an older build has no see-through keys at all. It must
    // still load, and must leave the effect off rather than inventing a value:
    // this is opt-in, so silence means "no".
    {
        WriteSettingsFile("[Input]\nMouseSensitivity=0.25\n");
        GameSettings settings;
        CHECK(LoadGameSettings(settings));
        CHECK(settings.mouseSensitivity == 0.25f);
        CHECK(!settings.seeThroughWeaponWhenAiming);
    }

    // Round trip: what Save writes, Load must read back unchanged.
    {
        GameSettings written;
        written.mouseSensitivity = 0.42f;
        written.seeThroughWeaponWhenAiming = true;
        written.seeThroughWeaponStrength = 0.6f;
        CHECK(SaveGameSettings(written));

        GameSettings read;
        CHECK(LoadGameSettings(read));
        CHECK(read.seeThroughWeaponWhenAiming);
        CHECK(std::abs(read.seeThroughWeaponStrength - 0.6f) < 1e-4f);
        CHECK(std::abs(read.mouseSensitivity - 0.42f) < 1e-4f);
    }

    // The toggle is hand-editable, so the spellings a person would reach for
    // all have to work -- and anything else has to read as off.
    {
        for (const char* on : {"1", "true", "yes"}) {
            WriteSettingsFile(std::string("SeeThroughWeaponWhenAiming=") + on + "\n");
            GameSettings settings;
            CHECK(LoadGameSettings(settings));
            CHECK(settings.seeThroughWeaponWhenAiming);
        }
        for (const char* off : {"0", "false", "no", "banana"}) {
            WriteSettingsFile(std::string("SeeThroughWeaponWhenAiming=") + off + "\n");
            GameSettings settings;
            CHECK(LoadGameSettings(settings));
            CHECK(!settings.seeThroughWeaponWhenAiming);
        }
    }

    // A hand-edited or corrupt strength must never leave the weapon in a state
    // the menu cannot undo, so the clamp has to survive the file.
    {
        WriteSettingsFile("SeeThroughWeaponStrength=9.5\n");
        GameSettings settings;
        CHECK(LoadGameSettings(settings));
        CHECK(settings.seeThroughWeaponStrength ==
              GameSettings::kMaxSeeThroughStrength);

        WriteSettingsFile("SeeThroughWeaponStrength=-3\n");
        GameSettings negative;
        CHECK(LoadGameSettings(negative));
        CHECK(negative.seeThroughWeaponStrength ==
              GameSettings::kMinSeeThroughStrength);
    }

    // Resetting has to turn the effect back off, not just restore the slider:
    // the reset button is the way out for a player who cannot read the screen.
    {
        GameSettings settings;
        settings.seeThroughWeaponWhenAiming = true;
        settings.seeThroughWeaponStrength = 0.3f;
        settings.ResetToDefaults();
        CHECK(!settings.seeThroughWeaponWhenAiming);
        CHECK(settings.seeThroughWeaponStrength ==
              GameSettings::kDefaultSeeThroughStrength);
    }

    // Experimental ray budgets stay opt-in for old files and remain bounded
    // even when a hand-edited INI contains NaN, infinity, or a negative number.
    {
        WriteSettingsFile("[Video]\nLumenGI=1\n");
        GameSettings defaults;
        CHECK(LoadGameSettings(defaults));
        CHECK(!defaults.lumenVariableRateGI);
        CHECK(!defaults.emissiveReSTIRCompatibility);
        CHECK(defaults.lumenVariableRateBudget == 0.5f);
        GameSettings written;
        written.lumenVariableRateGI = true;
        written.emissiveReSTIRCompatibility = true;
        written.lumenVariableRateBudget = 0.25f;
        CHECK(SaveGameSettings(written));
        GameSettings read;
        CHECK(LoadGameSettings(read));
        CHECK(read.lumenVariableRateGI && read.emissiveReSTIRCompatibility);
        CHECK(read.lumenVariableRateBudget == 0.25f);
        for (const char* invalid : {"nan", "inf", "-1", "0", "5"}) {
            WriteSettingsFile(std::string("LumenVariableRateBudget=") + invalid + "\n");
            CHECK(LoadGameSettings(read));
            CHECK(std::isfinite(read.lumenVariableRateBudget));
            CHECK(read.lumenVariableRateBudget >= 0.125f && read.lumenVariableRateBudget <= 1.0f);
        }
        read.ResetToDefaults();
        CHECK(!read.lumenVariableRateGI && !read.emissiveReSTIRCompatibility);
        CHECK(read.lumenVariableRateBudget == 0.5f);
    }

    // VSync round-trip and default.
    {
        GameSettings written;
        written.vsync = true;
        CHECK(SaveGameSettings(written));

        GameSettings read;
        CHECK(LoadGameSettings(read));
        CHECK(read.vsync);

        GameSettings defaults;
        CHECK(defaults.vsync == GameSettings::kDefaultVsync);
    }

    // Scene-lit fog mode round-trips and falls back to Level default when out of range.
    {
        GameSettings written;
        written.sceneLitFog = GameSettings::kSceneLitFogAllLevels;
        CHECK(SaveGameSettings(written));
        GameSettings read;
        CHECK(LoadGameSettings(read));
        CHECK(read.sceneLitFog == GameSettings::kSceneLitFogAllLevels);
        WriteSettingsFile("SceneLitFog=7\n");
        CHECK(LoadGameSettings(read));
        CHECK(read.sceneLitFog == GameSettings::kDefaultSceneLitFog);
        CHECK(GameSettings{}.sceneLitFog == GameSettings::kSceneLitFogLevelDefault);
    }

    // DLSS choice, preset and render percentage survive restarts and reject bad INI values.
    {
        GameSettings written;
        written.dlssEnabled = true;
        written.dlssFrameGeneration = true;
        written.nvidiaReflex = 2;
        written.rayTracingQuality = GameSettings::kRayTracingUltra;
        written.dlssPreset = 2;
        written.extensionMotionVectors = true;
        written.dlssScreenPercentage = 67.0f;
        CHECK(SaveGameSettings(written));

        GameSettings read;
        CHECK(LoadGameSettings(read));
        CHECK(read.dlssEnabled);
        CHECK(read.dlssFrameGeneration);
        CHECK(read.nvidiaReflex == 2);
        CHECK(read.rayTracingQuality == GameSettings::kRayTracingUltra);
        CHECK(read.dlssPreset == 2);
        CHECK(read.extensionMotionVectors);
        CHECK(read.dlssScreenPercentage == 67.0f);

        WriteSettingsFile("DLSSPreset=-1\n");
        CHECK(LoadGameSettings(read));
        CHECK(read.dlssPreset == GameSettings::kMinDLSSPreset);
        WriteSettingsFile("DLSSPreset=9\n");
        CHECK(LoadGameSettings(read));
        CHECK(read.dlssPreset == GameSettings::kMaxDLSSPreset);

        WriteSettingsFile("DLSSScreenPercentage=0\n");
        CHECK(LoadGameSettings(read));
        CHECK(read.dlssScreenPercentage ==
              GameSettings::kMinDLSSScreenPercentage);
        WriteSettingsFile("DLSSScreenPercentage=200\n");
        CHECK(LoadGameSettings(read));
        CHECK(read.dlssScreenPercentage ==
              GameSettings::kMaxDLSSScreenPercentage);

        GameSettings defaults;
        CHECK(!defaults.dlssEnabled);
        CHECK(!defaults.dlssFrameGeneration);
        CHECK(defaults.nvidiaReflex == 0);
        WriteSettingsFile("NVIDIAReflex=-2\n");
        CHECK(LoadGameSettings(read));
        CHECK(read.nvidiaReflex == 0);
        WriteSettingsFile("NVIDIAReflex=5\n");
        CHECK(LoadGameSettings(read));
        CHECK(read.nvidiaReflex == 2);
        read.dlssFrameGeneration = true;
        read.nvidiaReflex = 2;
        read.ResetToDefaults();
        CHECK(!read.dlssFrameGeneration);
        CHECK(read.nvidiaReflex == 0);

        CHECK(!defaults.maxFidelityLighting);
        GameSettings lighting;
        lighting.maxFidelityLighting = true;
        CHECK(SaveGameSettings(lighting));
        CHECK(LoadGameSettings(read));
        CHECK(read.maxFidelityLighting);
        read.ResetToDefaults();
        CHECK(!read.maxFidelityLighting);
        CHECK(defaults.rayTracingQuality == GameSettings::kRayTracingOff);
        CHECK(defaults.dlssPreset == GameSettings::kDefaultDLSSPreset);
        CHECK(defaults.extensionMotionVectors ==
              GameSettings::kDefaultExtensionMotionVectors);
        CHECK(defaults.dlssScreenPercentage ==
              GameSettings::kDefaultDLSSScreenPercentage);
    }

    // Legacy DLSSRayReconstruction key maps to rayTracingQuality.
    {
        WriteSettingsFile("DLSSRayReconstruction=1\n");
        GameSettings read;
        CHECK(LoadGameSettings(read));
        CHECK(read.rayTracingQuality == GameSettings::kRayTracingUltra);
    }

    // RayTracingQuality round-trip.
    {
        GameSettings written;
        written.rayTracingQuality = GameSettings::kRayTracingUltra;
        CHECK(SaveGameSettings(written));

        GameSettings read;
        CHECK(LoadGameSettings(read));
        CHECK(read.rayTracingQuality == GameSettings::kRayTracingUltra);

        WriteSettingsFile("RayTracingQuality=0\n");
        CHECK(LoadGameSettings(read));
        CHECK(read.rayTracingQuality == GameSettings::kRayTracingOff);
    }

    // LumenGI round-trip and default.
    {
        GameSettings written;
        written.lumenGI = true;
        CHECK(SaveGameSettings(written));

        GameSettings read;
        CHECK(LoadGameSettings(read));
        CHECK(read.lumenGI);

        WriteSettingsFile("LumenGI=0\n");
        CHECK(LoadGameSettings(read));
        CHECK(!read.lumenGI);

        GameSettings defaults;
        CHECK(defaults.lumenGI == GameSettings::kDefaultLumenGI);
    }

    // Legacy files retain Lumen, and selecting cascades never stacks diffuse GI.
    {
        WriteSettingsFile("LumenGI=1\n");
        GameSettings legacy;
        CHECK(LoadGameSettings(legacy));
        CHECK(legacy.lumenGI && !legacy.radianceCascadesGI);

        WriteSettingsFile("LumenGI=1\nRadianceCascadesGI=1\n");
        GameSettings cascades;
        CHECK(LoadGameSettings(cascades));
        CHECK(cascades.radianceCascadesGI && !cascades.lumenGI);
        CHECK(SaveGameSettings(cascades));
        GameSettings restored;
        CHECK(LoadGameSettings(restored));
        CHECK(restored.radianceCascadesGI && !restored.lumenGI);
        restored.ResetToDefaults();
        CHECK(!restored.radianceCascadesGI && !restored.lumenGI);

        WriteSettingsFile("RadianceCascadesGI=banana\n");
        GameSettings invalid;
        CHECK(LoadGameSettings(invalid));
        CHECK(!invalid.radianceCascadesGI);
    }

    // Older settings keep the full-resolution reference; the opt-in survives
    // restarts independently of the main Lumen toggle; reset restores it.
    {
        WriteSettingsFile("LumenGI=1\n");
        GameSettings read;
        CHECK(LoadGameSettings(read));
        CHECK(read.lumenGI);
        CHECK(!read.lumenGIHalfResolution);
        read.lumenGIHalfResolution = true;
        CHECK(SaveGameSettings(read));
        GameSettings restored;
        CHECK(LoadGameSettings(restored));
        CHECK(restored.lumenGI && restored.lumenGIHalfResolution);
        restored.ResetToDefaults();
        CHECK(restored.lumenGIHalfResolution ==
              GameSettings::kDefaultLumenGIHalfResolution);
    }

    // RR upscaling uses the current default in older files, persists and resets.
    {
        WriteSettingsFile("RayTracingQuality=1\n");
        GameSettings read;
        CHECK(LoadGameSettings(read));
        CHECK(read.rayReconstructionUpscale ==
              GameSettings::kDefaultRayReconstructionUpscale);
        read.rayReconstructionUpscale = !GameSettings::kDefaultRayReconstructionUpscale;
        CHECK(SaveGameSettings(read));
        GameSettings restored;
        CHECK(LoadGameSettings(restored));
        CHECK(restored.rayReconstructionUpscale ==
              !GameSettings::kDefaultRayReconstructionUpscale);
        restored.ResetToDefaults();
        CHECK(restored.rayReconstructionUpscale ==
              GameSettings::kDefaultRayReconstructionUpscale);
    }

    // RR forward guides: on in older files, survives being turned off, resets.
    {
        WriteSettingsFile("RayReconstructionUpscale=1\n");
        GameSettings read;
        CHECK(LoadGameSettings(read));
        CHECK(read.rrForwardGuides);
        read.rrForwardGuides = false;
        CHECK(SaveGameSettings(read));
        GameSettings restored;
        CHECK(LoadGameSettings(restored));
        CHECK(!restored.rrForwardGuides);
        restored.ResetToDefaults();
        CHECK(restored.rrForwardGuides ==
              GameSettings::kDefaultRRForwardGuides);
    }

    // DLSS forward motion: on in older files, survives being turned off.
    {
        WriteSettingsFile("DLSS=1\n");
        GameSettings read;
        CHECK(LoadGameSettings(read));
        CHECK(read.dlssForwardMotion);
        read.dlssForwardMotion = false;
        CHECK(SaveGameSettings(read));
        GameSettings restored;
        CHECK(LoadGameSettings(restored));
        CHECK(!restored.dlssForwardMotion);
        restored.ResetToDefaults();
        CHECK(restored.dlssForwardMotion ==
              GameSettings::kDefaultDLSSForwardMotion);
    }

    // RT reflection roughness cutoff round-trip, default and clamp.
    {
        GameSettings written;
        written.rtReflectionRoughnessCutoff = 0.4f;
        CHECK(SaveGameSettings(written));

        GameSettings read;
        CHECK(LoadGameSettings(read));
        CHECK(std::fabs(read.rtReflectionRoughnessCutoff - 0.4f) < 1e-4f);

        GameSettings defaults;
        CHECK(defaults.rtReflectionRoughnessCutoff ==
              GameSettings::kDefaultRTReflectionRoughnessCutoff);

        WriteSettingsFile("RTReflectionRoughnessCutoff=7\n");
        CHECK(LoadGameSettings(read));
        read.Clamp();
        CHECK(read.rtReflectionRoughnessCutoff ==
              GameSettings::kMaxRTReflectionRoughnessCutoff);

        WriteSettingsFile("RTReflectionRoughnessCutoff=-1\n");
        CHECK(LoadGameSettings(read));
        read.Clamp();
        CHECK(read.rtReflectionRoughnessCutoff ==
              GameSettings::kMinRTReflectionRoughnessCutoff);
    }

    // Fullscreen round-trip and default.
    {
        GameSettings written;
        written.fullscreen = false;
        CHECK(SaveGameSettings(written));

        GameSettings read;
        CHECK(LoadGameSettings(read));
        CHECK(!read.fullscreen);

        GameSettings defaults;
        CHECK(defaults.fullscreen == GameSettings::kDefaultFullscreen);
    }

    // Field of view clamping and round-trip.
    {
        GameSettings written;
        written.fieldOfView = 75.0f;
        CHECK(SaveGameSettings(written));

        GameSettings read;
        CHECK(LoadGameSettings(read));
        CHECK(std::abs(read.fieldOfView - 75.0f) < 1e-4f);

        // Out of bounds values must be clamped.
        WriteSettingsFile("FieldOfView=150\n");
        GameSettings outOfBounds;
        CHECK(LoadGameSettings(outOfBounds));
        CHECK(outOfBounds.fieldOfView == GameSettings::kMaxFieldOfView);

        WriteSettingsFile("FieldOfView=25\n");
        GameSettings tooLow;
        CHECK(LoadGameSettings(tooLow));
        CHECK(tooLow.fieldOfView == GameSettings::kMinFieldOfView);
    }

    // Invert mouse Y round-trip and default.
    {
        GameSettings written;
        written.invertMouseY = true;
        CHECK(SaveGameSettings(written));

        GameSettings read;
        CHECK(LoadGameSettings(read));
        CHECK(read.invertMouseY);

        GameSettings defaults;
        CHECK(defaults.invertMouseY == GameSettings::kDefaultInvertMouseY);
    }

    // Each weapon keeps its own visual shot kick across restarts. Old settings
    // files retain the normal 100% value for weapons they do not mention.
    {
        GameSettings written;
        written.weaponCameraShake[12] = 25.0f;
        written.weaponCameraShake[1] = 0.0f;
        CHECK(SaveGameSettings(written));

        GameSettings read;
        CHECK(LoadGameSettings(read));
        CHECK(std::abs(read.weaponCameraShake[12] - 25.0f) < 1e-4f);
        CHECK(read.weaponCameraShake[1] == 0.0f);
        CHECK(read.weaponCameraShake[0] ==
              GameSettings::kDefaultWeaponCameraShake);

        WriteSettingsFile("WeaponCameraShake12=90\n"
                          "WeaponCameraShake1=-2\n");
        GameSettings clamped;
        CHECK(LoadGameSettings(clamped));
        CHECK(clamped.weaponCameraShake[12] ==
              GameSettings::kMaxWeaponCameraShake);
        CHECK(clamped.weaponCameraShake[1] == 0.0f);
    }

    std::filesystem::remove(GameSettingsPath());
    if (failures == 0) std::cout << "GameSettingsTests passed\n";
    return failures == 0 ? 0 : 1;
}
