#include "UISearchFilter.h"

#include <iostream>

static int failures = 0;
#define CHECK(value) do { if (!(value)) { \
    std::cerr << __FILE__ << ':' << __LINE__ << " CHECK failed: " #value "\n"; \
    ++failures; } } while (false)

// The real registrations from EngineUI.h, so the cases below assert against
// what the panel actually offers rather than a convenient stand-in. If a
// section's keywords are edited, these come with it.
static constexpr const char* kViewmodelLabel = "Viewmodel (Gun)";
static constexpr const char* kViewmodelKeywords =
    "weapon rifle offset scale rotation fit attachments "
    "optic sight red dot scope suppressor grip "
    "ads aim down sights fov blend recoil sway "
    "see-through see through transparent transparency "
    "opacity alpha binocular fade "
    "arms hands mirror head auto fire interval muzzle";

static constexpr const char* kCameraLabel = "Camera Settings";
static constexpr const char* kCameraKeywords =
    "position fov near far clip speed movement fps walking sensitivity view";

int main() {
    // An empty query is not a filter: everything survives, which is what the
    // panel looks like before anyone types.
    CHECK(UISearchSectionMatches(kCameraLabel, kCameraKeywords, ""));
    CHECK(UISearchSectionMatches(kViewmodelLabel, kViewmodelKeywords, ""));

    // A query of only spaces must behave like an empty one. Typing a stray
    // space should never blank the entire panel.
    CHECK(UISearchSectionMatches(kCameraLabel, kCameraKeywords, "   "));

    // Matching the visible title still works -- that was the original
    // behaviour and nothing about keywords should have cost it.
    CHECK(UISearchSectionMatches(kCameraLabel, kCameraKeywords, "camera"));
    CHECK(UISearchSectionMatches(kViewmodelLabel, kViewmodelKeywords, "viewmodel"));

    // Case folding, both directions.
    CHECK(UISearchSectionMatches(kCameraLabel, kCameraKeywords, "CAMERA"));
    CHECK(UISearchSectionMatches(kCameraLabel, kCameraKeywords, "CaMeRa"));

    // The point of the keyword list: the words someone types are the words on
    // the controls, and those almost never appear in the header above them.
    // Every one of these used to find nothing.
    CHECK(UISearchSectionMatches(kCameraLabel, kCameraKeywords, "sensitivity"));
    CHECK(UISearchSectionMatches(kViewmodelLabel, kViewmodelKeywords, "opacity"));
    CHECK(UISearchSectionMatches(kViewmodelLabel, kViewmodelKeywords, "transparent"));
    CHECK(UISearchSectionMatches(kViewmodelLabel, kViewmodelKeywords, "see-through"));
    CHECK(UISearchSectionMatches(kViewmodelLabel, kViewmodelKeywords, "binocular"));
    CHECK(UISearchSectionMatches(kViewmodelLabel, kViewmodelKeywords, "ads"));

    // Partial words match, so a search narrows as it is typed rather than only
    // paying off on the last keystroke.
    CHECK(UISearchSectionMatches(kViewmodelLabel, kViewmodelKeywords, "trans"));
    CHECK(UISearchSectionMatches(kCameraLabel, kCameraKeywords, "sens"));

    // Multiple terms are AND, not OR: more words must narrow the result. Both
    // sections carry "fov", only one carries "gun".
    CHECK(UISearchSectionMatches(kViewmodelLabel, kViewmodelKeywords, "fov"));
    CHECK(UISearchSectionMatches(kCameraLabel, kCameraKeywords, "fov"));
    CHECK(UISearchSectionMatches(kViewmodelLabel, kViewmodelKeywords, "gun fov"));
    CHECK(!UISearchSectionMatches(kCameraLabel, kCameraKeywords, "gun fov"));

    // Term order must not matter.
    CHECK(UISearchSectionMatches(kViewmodelLabel, kViewmodelKeywords, "fov gun"));

    // Extra whitespace between terms is harmless.
    CHECK(UISearchSectionMatches(kViewmodelLabel, kViewmodelKeywords, "  gun   fov  "));

    // A term that matches nothing rejects the section even when its companion
    // matches, which is what makes AND useful for narrowing.
    CHECK(!UISearchSectionMatches(kViewmodelLabel, kViewmodelKeywords,
                                  "gun nonsenseterm"));

    // Genuine misses stay misses.
    CHECK(!UISearchSectionMatches(kCameraLabel, kCameraKeywords, "zzzzz"));
    CHECK(!UISearchSectionMatches(kViewmodelLabel, kViewmodelKeywords, "terrain"));

    // A section with no keywords at all must still match on its title, since
    // the parameter is optional and older call sites pass nothing.
    CHECK(UISearchSectionMatches("Destruction", nullptr, "destruction"));
    CHECK(!UISearchSectionMatches("Destruction", nullptr, "shadow"));

    // Tokenising is whitespace-only, so a hyphenated control name is one term
    // and matches the hyphenated keyword literally.
    CHECK(UISearchTermsIn("see-through").size() == 1);
    CHECK(UISearchTermsIn("gun fov").size() == 2);
    CHECK(UISearchTermsIn("").empty());
    CHECK(UISearchTermsIn("   ").empty());

    if (failures == 0) std::cout << "UISearchFilterTests passed\n";
    return failures == 0 ? 0 : 1;
}
