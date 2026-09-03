#ifndef UI_SEARCH_FILTER_H
#define UI_SEARCH_FILTER_H

// Text matching for the debug panel's settings search.
//
// Split out of EngineUI.h so it can be tested without a device: that header
// pulls in ImGui, DX12 and the whole scene, none of which this needs. The
// filtering rule is the part worth pinning down, and it is pure string work.

#include <cctype>
#include <sstream>
#include <string>
#include <vector>

inline std::string UILowerCopy(const char* text) {
    std::string out(text ? text : "");
    for (char& c : out) c = (char)tolower((unsigned char)c);
    return out;
}

// Query split on whitespace. Empty for an empty or all-space query.
inline std::vector<std::string> UISearchTermsIn(const char* query) {
    std::vector<std::string> terms;
    std::istringstream stream(UILowerCopy(query));
    std::string term;
    while (stream >> term) terms.push_back(term);
    return terms;
}

// True when every term in `query` appears somewhere in `haystack`, which must
// already be lowercase. All-terms rather than any-term: typing more words
// should narrow the results, so "gun fov" finds the control that is both
// rather than everything that is either.
//
// A query of only whitespace matches everything, exactly like an empty one --
// a stray space must not blank the whole panel.
inline bool UISearchTextMatches(const std::string& haystack,
                                const char* query) {
    for (const std::string& term : UISearchTermsIn(query))
        if (haystack.find(term) == std::string::npos) return false;
    return true;
}

// Whether a section survives the filter. Matches its visible title or any of
// the keywords it registers, so the wording of a control finds the section it
// lives in -- "sensitivity" is under "Camera Settings", and nobody guesses that
// from the header alone.
inline bool UISearchSectionMatches(const char* label, const char* keywords,
                                   const char* query) {
    const std::string haystack = UILowerCopy(label) + " " + UILowerCopy(keywords);
    return UISearchTextMatches(haystack, query);
}

#endif // UI_SEARCH_FILTER_H
