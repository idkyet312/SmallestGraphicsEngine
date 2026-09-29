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
